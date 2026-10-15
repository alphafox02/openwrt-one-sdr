// SPDX-License-Identifier: MIT
// The receiver's controller: a child process (by default `w1700k serve`) speaking
// newline-delimited JSON on its stdin and stdout. Every request carries an integer id and gets
// exactly one reply, {"id":N,"ok":true,...} or {"id":N,"ok":false,"error":"..."}; lines without
// an id are events: state, progress and log. The controller is restarted, with a growing delay,
// whenever it exits or stops answering.
#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "child.h"
#include "json.h"

class Controller {
public:
    struct Listener {
        std::function<void(const JsonValue &state)> state;  // a STATE, from an event or a reply
        std::function<void(const std::string &level, const std::string &text)> message;  // for the viewer
        std::function<void()> changed;  // status_json() may differ now
    };
    using Done = std::function<void(const std::string &error)>;  // error is "" on success
    struct Op {
        std::string name;               // "configure", "start", "stop", "tune", "gain", "filter", "status"
        std::string fields;             // further request members as JSON text: ",\"hz\":5250000000"
        std::function<void()> sending;  // just before the request is written
        Done done;                      // its outcome, unless the controller died first
    };

    // All calls, and the listener's, are on one thread (the web loop): poll() drives everything.
    Controller(const std::string &command, std::function<void()> wake, Listener listener);
    ~Controller();
    Controller(const Controller &) = delete;
    Controller &operator=(const Controller &) = delete;

    void poll();  // on every wake and tick

    // Queues a job: its ops are sent one at a time, after every job queued before it, and the
    // first that fails ends it. With a key, a queued job with the same key that has not started
    // is replaced instead, so that only the latest of a quick series (a dragged slider) is sent.
    void submit(std::vector<Op> ops, const std::string &key = "");

    bool alive() const { return child_ && !terminating_; }
    bool ready() const { return alive() && have_state_; }  // running, and its state is known
    // A long operation (configure, start, stop) is queued or running, or the controller reports
    // itself busy: conflicting requests are refused meanwhile.
    bool busy() const;
    std::string busy_text() const;
    const JsonValue &state() const { return state_; }
    std::string status_json() const;

private:
    using Clock = std::chrono::steady_clock;
    struct Job {
        std::vector<Op> ops;
        std::string key;
        size_t next = 0;       // next op to send
        bool started = false;
    };
    struct Request {
        uint64_t id;
        Op op;
        Clock::time_point deadline;
    };

    void spawn();
    void send_next();
    void handle_line(const std::string &line);
    void handle_reply(const JsonValue &m);
    void handle_state(const JsonValue &state);
    void finish(Request request, const std::string &error);
    void exited(const std::string &how);
    void fail(const std::string &level, const std::string &text);

    const std::string command_;
    std::function<void()> wake_;
    Listener listener_;

    std::unique_ptr<ChildProcess> child_;
    bool terminating_ = false;          // asked to stop (it ended its output or stopped answering)
    Clock::time_point terminate_at_, started_at_, restart_at_;
    std::string stop_reason_;           // why it is being stopped
    std::string down_text_;             // why it is not running, while it is not
    unsigned restarts_ = 0, failures_ = 0;

    std::deque<Job> queue_;             // the front job is the running one once started
    std::optional<Request> in_flight_;
    uint64_t next_id_ = 1;

    JsonValue state_;
    bool have_state_ = false;
    std::string progress_text_;
    double progress_fraction_ = -1;     // < 0: unknown
};
