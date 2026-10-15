// SPDX-License-Identifier: MIT
#include "controller.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <iterator>
#include <stdexcept>

namespace {

using namespace std::chrono_literals;

// Restart delays after consecutive failures; a controller that ran this long counts as healthy.
constexpr std::chrono::seconds kBackoff[] = {1s, 2s, 5s, 10s, 30s};
constexpr auto kHealthy = 60s;
constexpr auto kKillAfter = 2s;  // SIGTERM, then SIGKILL this much later

// How long each request may take. A configure can reload the firmware (about 100 s).
std::chrono::seconds timeout_of(const std::string &op)
{
    if (op == "configure") return 600s;
    if (op == "start" || op == "stop") return 120s;
    return 30s;
}

bool is_long(const std::string &op) { return op == "configure" || op == "start" || op == "stop"; }

std::string progressive(const std::string &op)
{
    if (op == "configure") return "configuring";
    if (op == "start") return "starting";
    if (op == "stop") return "stopping";
    return op;
}

std::string shorten(const std::string &text)
{
    return text.size() > 200 ? text.substr(0, 200) + "..." : text;
}

}  // namespace

Controller::Controller(const std::string &command, std::function<void()> wake, Listener listener)
    : command_(command), wake_(std::move(wake)), listener_(std::move(listener)), restart_at_(Clock::now())
{
    down_text_ = "starting";
}

Controller::~Controller() = default;  // the child is told to stop (its stdin closes) and reaped

void Controller::poll()
{
    const auto now = Clock::now();
    if (!child_) {
        if (now >= restart_at_) spawn();
        if (!child_) return;
    }
    child_->flush();
    bool ended = false;
    for (const std::string &line : child_->take_lines(ended)) handle_line(line);
    if (!child_) return;

    std::string how;
    if (child_->reaped(how)) {
        exited(stop_reason_.empty() ? how : stop_reason_ + " (" + how + ")");
        return;
    }
    if (ended && !terminating_) {  // closed its output: it is of no use any more
        terminating_ = true;
        terminate_at_ = now;
        stop_reason_ = "closed its output";
        child_->kill_group(SIGTERM);
    }
    if (in_flight_ && now > in_flight_->deadline && !terminating_) {
        const std::string op = in_flight_->op.name;
        const auto limit = std::chrono::duration_cast<std::chrono::seconds>(timeout_of(op)).count();
        terminating_ = true;
        terminate_at_ = now;
        stop_reason_ = "did not answer " + op + " within " + std::to_string(limit) + " s";
        child_->kill_group(SIGTERM);
        listener_.changed();
    }
    if (terminating_ && now - terminate_at_ > kKillAfter) child_->kill_group(SIGKILL);
    if (!in_flight_ && !terminating_) send_next();
}

void Controller::spawn()
{
    try {
        child_ = std::make_unique<ChildProcess>(command_, wake_);
    } catch (const std::exception &e) {
        exited(e.what());
        return;
    }
    std::fprintf(stderr, "controller: started `%s` (pid %d)\n", command_.c_str(), int(child_->pid()));
    terminating_ = false;
    stop_reason_.clear();
    down_text_.clear();
    started_at_ = Clock::now();
    have_state_ = false;
    // Its state first, before anything queued meanwhile.
    Job status;
    status.ops.push_back(Op{"status", "", nullptr, nullptr});
    queue_.push_front(std::move(status));
    listener_.changed();
}

void Controller::submit(std::vector<Op> ops, const std::string &key)
{
    if (ops.empty()) return;
    bool replaced = false;
    if (!key.empty())
        for (Job &job : queue_)
            if (!job.started && job.key == key) {
                job.ops = std::move(ops);
                replaced = true;
                break;
            }
    if (!replaced) {
        Job job;
        job.ops = std::move(ops);
        job.key = key;
        queue_.push_back(std::move(job));
    }
    if (child_ && !in_flight_ && !terminating_) send_next();
    listener_.changed();
}

void Controller::send_next()
{
    if (queue_.empty()) return;
    Job &job = queue_.front();
    job.started = true;
    Request request{next_id_++, job.ops[job.next], Clock::now() + timeout_of(job.ops[job.next].name)};
    const std::string line = "{\"id\":" + std::to_string(request.id) + ",\"op\":" + json_quote(request.op.name) +
                             request.op.fields + "}";
    if (request.op.sending) request.op.sending();
    in_flight_ = request;
    if (!child_->write_line(line)) {
        in_flight_.reset();
        finish(std::move(request), "the controller is not reading its requests");
        return;
    }
    if (is_long(request.op.name)) listener_.changed();
}

void Controller::handle_line(const std::string &line)
{
    if (line.find_first_not_of(" \t\r") == std::string::npos) return;
    JsonValue m;
    try {
        m = parse_json(line);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "controller: unreadable line (%s): %s\n", e.what(), shorten(line).c_str());
        return;
    }
    if (m.kind != JsonValue::Object) {
        std::fprintf(stderr, "controller: not an object: %s\n", shorten(line).c_str());
    } else if (m.has("id")) {
        handle_reply(m);
    } else if (m.has("event")) {
        const std::string event = m.string_or("event");
        if (event == "state") {
            if (const JsonValue *s = m.find("state"); s && s->kind == JsonValue::Object) handle_state(*s);
        } else if (event == "progress") {
            progress_text_ = m.string_or("text");
            progress_fraction_ = m.number_or("fraction", -1);
            listener_.changed();
        } else if (event == "log") {
            const std::string level = m.string_or("level", "info"), text = m.string_or("text");
            std::fprintf(stderr, "controller %s: %s\n", level.c_str(), text.c_str());
            if (level != "info") listener_.message(level, text);
        }
    } else {
        std::fprintf(stderr, "controller: neither a reply nor an event: %s\n", shorten(line).c_str());
    }
}

void Controller::handle_reply(const JsonValue &m)
{
    const double id = m.number_or("id", -1);
    if (!in_flight_ || id != double(in_flight_->id)) {
        std::fprintf(stderr, "controller: reply to no pending request (id %.0f)\n", id);
        return;
    }
    Request request = std::move(*in_flight_);
    in_flight_.reset();
    if (const JsonValue *s = m.find("state"); s && s->kind == JsonValue::Object) handle_state(*s);
    const bool ok = m.boolean_or("ok", false);
    finish(std::move(request), ok ? "" : m.string_or("error", "failed without saying why"));
}

void Controller::handle_state(const JsonValue &state)
{
    state_ = state;
    have_state_ = true;
    const JsonValue *busy = state_.find("busy");
    const bool long_running = in_flight_ && is_long(in_flight_->op.name);
    if ((!busy || busy->is_null()) && !long_running) {
        progress_text_.clear();
        progress_fraction_ = -1;
    }
    listener_.state(state_);
    listener_.changed();
}

// The queue moves on before `done` runs, which may submit more.
void Controller::finish(Request request, const std::string &error)
{
    if (!queue_.empty()) {
        Job &job = queue_.front();
        if (!error.empty() || ++job.next >= job.ops.size()) queue_.pop_front();
    }
    if (is_long(request.op.name)) {
        progress_text_.clear();
        progress_fraction_ = -1;
    }
    if (!error.empty())
        std::fprintf(stderr, "controller: %s failed: %s\n", request.op.name.c_str(), error.c_str());
    if (request.op.done) request.op.done(error);
    listener_.changed();
}

void Controller::exited(const std::string &how)
{
    const auto now = Clock::now();
    const bool healthy = child_ && now - started_at_ >= kHealthy;
    child_.reset();
    terminating_ = false;
    stop_reason_.clear();
    have_state_ = false;
    progress_text_.clear();
    progress_fraction_ = -1;
    std::optional<Request> lost = std::move(in_flight_);
    in_flight_.reset();
    size_t dropped = 0;
    for (const Job &job : queue_) dropped += job.ops.size() - job.next - (lost && &job == &queue_.front() ? 1 : 0);
    queue_.clear();

    if (healthy) failures_ = 0;
    const auto delay = kBackoff[std::min<size_t>(failures_, std::size(kBackoff) - 1)];
    ++failures_;
    ++restarts_;
    restart_at_ = now + delay;
    down_text_ = "the controller " + how + "; restarting in " + std::to_string(delay.count()) + " s";
    std::string text = down_text_;
    if (dropped) text += " (" + std::to_string(dropped) + " queued requests dropped)";
    std::fprintf(stderr, "%s\n", text.c_str());
    if (lost && lost->op.done) lost->op.done("the controller " + how);
    fail("error", text);
}

void Controller::fail(const std::string &level, const std::string &text)
{
    listener_.message(level, text);
    listener_.changed();
}

bool Controller::busy() const
{
    if (in_flight_ && is_long(in_flight_->op.name)) return true;
    for (const Job &job : queue_)
        for (size_t i = job.next; i < job.ops.size(); ++i)
            if (is_long(job.ops[i].name)) return true;
    if (!ready()) return false;
    const JsonValue *b = state_.find("busy");
    return b && b->is_string() && !b->string.empty();
}

std::string Controller::busy_text() const
{
    const JsonValue *b = ready() ? state_.find("busy") : nullptr;
    if (b && b->is_string() && !b->string.empty()) return b->string;
    if (in_flight_ && is_long(in_flight_->op.name)) return progressive(in_flight_->op.name);
    for (const Job &job : queue_)
        for (size_t i = job.next; i < job.ops.size(); ++i)
            if (is_long(job.ops[i].name)) return progressive(job.ops[i].name);
    return "";
}

std::string Controller::status_json() const
{
    const std::string busy = busy_text();
    std::string error;
    if (!alive()) error = down_text_;
    else if (const JsonValue *e = ready() ? state_.find("error") : nullptr; e && e->is_string()) error = e->string;
    std::string progress = "null";
    if (!progress_text_.empty() || progress_fraction_ >= 0)
        progress = "{\"text\":" + json_quote(progress_text_) + ",\"fraction\":" +
                   (progress_fraction_ >= 0 ? json_number(std::min(1.0, progress_fraction_)) : "null") + "}";
    size_t queued = 0;
    for (const Job &job : queue_) queued += job.ops.size() - job.next;
    if (in_flight_ && queued) --queued;
    return "{\"mode\":\"managed\",\"command\":" + json_quote(command_) + ",\"alive\":" + (alive() ? "true" : "false") +
           ",\"pid\":" + (child_ ? std::to_string(child_->pid()) : "null") + ",\"restarts\":" + std::to_string(restarts_) +
           ",\"busy\":" + (busy.empty() ? "null" : json_quote(busy)) + ",\"progress\":" + progress +
           ",\"error\":" + (error.empty() ? "null" : json_quote(error)) + ",\"queued\":" + std::to_string(queued) +
           ",\"state\":" + (ready() ? json_write(state_) : "null") + "}";
}
