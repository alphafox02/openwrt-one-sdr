// SPDX-License-Identifier: MIT
#include "child.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr size_t kMaxLine = 1 << 20;            // a longer line is dropped
constexpr size_t kMaxPendingInput = 1 << 20;    // requests the child has not read yet
constexpr auto kExitGrace = std::chrono::seconds(2);
constexpr auto kTermGrace = std::chrono::seconds(1);

std::string describe(int status)
{
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) return "exited with status 127 (command not found?)";
    if (WIFEXITED(status)) return "exited with status " + std::to_string(WEXITSTATUS(status));
    if (WIFSIGNALED(status))
        return "was killed by signal " + std::to_string(WTERMSIG(status)) + " (" + strsignal(WTERMSIG(status)) + ")";
    return "stopped";
}

}  // namespace

ChildProcess::ChildProcess(const std::string &command, std::function<void()> wake) : wake_(std::move(wake))
{
    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC)) throw std::runtime_error(std::string("pipe: ") + std::strerror(errno));
    if (pipe2(out, O_CLOEXEC)) {
        int e = errno;
        close(in[0]);
        close(in[1]);
        throw std::runtime_error(std::string("pipe: ") + std::strerror(e));
    }
    const char *text = command.c_str();
    pid_ = fork();
    if (pid_ == 0) {
        // Async-signal-safe calls only until exec: the parent has other threads.
        setpgid(0, 0);
        signal(SIGPIPE, SIG_DFL);
        if (dup2(in[0], STDIN_FILENO) < 0 || dup2(out[1], STDOUT_FILENO) < 0) _exit(127);
        execl("/bin/sh", "sh", "-c", text, static_cast<char *>(nullptr));
        _exit(127);
    }
    int e = errno;
    close(in[0]);
    close(out[1]);
    if (pid_ < 0) {
        close(in[1]);
        close(out[0]);
        throw std::runtime_error(std::string("fork: ") + std::strerror(e));
    }
    setpgid(pid_, pid_);  // also here, so that signalling the group cannot race the child's own call
    stdin_fd_ = in[1];
    stdout_fd_ = out[0];
    fcntl(stdin_fd_, F_SETFL, fcntl(stdin_fd_, F_GETFL) | O_NONBLOCK);
    reader_ = std::thread([this] { read_output(); });
}

ChildProcess::~ChildProcess()
{
    if (stdin_fd_ >= 0) close(stdin_fd_);  // end of input: a well-behaved child exits now
    stdin_fd_ = -1;
    std::string how;
    auto wait_for = [&](auto grace) {
        auto until = std::chrono::steady_clock::now() + grace;
        while (!reaped(how) && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return reaped_;
    };
    if (!wait_for(kExitGrace)) {
        kill(-pid_, SIGTERM);
        if (!wait_for(kTermGrace)) {
            kill(-pid_, SIGKILL);
            int status;
            while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
            reaped_ = true;
        }
    }
    kill(-pid_, SIGTERM);  // anything the shell started that outlived it
    stopping_ = true;
    if (reader_.joinable()) reader_.join();
    close(stdout_fd_);
}

bool ChildProcess::write_line(const std::string &line)
{
    if (stdin_fd_ < 0 || pending_input_.size() + line.size() + 1 > kMaxPendingInput) return false;
    pending_input_ += line;
    pending_input_ += '\n';
    flush();
    return stdin_fd_ >= 0;
}

void ChildProcess::flush()
{
    while (stdin_fd_ >= 0 && !pending_input_.empty()) {
        ssize_t n = write(stdin_fd_, pending_input_.data(), pending_input_.size());
        if (n > 0) {
            pending_input_.erase(0, size_t(n));
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && errno == EAGAIN) {
            return;
        } else {  // EPIPE: the child closed its input
            close(stdin_fd_);
            stdin_fd_ = -1;
            pending_input_.clear();
        }
    }
}

std::vector<std::string> ChildProcess::take_lines(bool &ended)
{
    std::lock_guard<std::mutex> lock(lines_mutex_);
    std::vector<std::string> lines;
    lines.swap(lines_);
    ended = ended_;
    return lines;
}

bool ChildProcess::reaped(std::string &how)
{
    if (!reaped_) {
        int status;
        pid_t r = waitpid(pid_, &status, WNOHANG);
        if (r == pid_) {
            reaped_ = true;
            exit_text_ = describe(status);
        } else if (r < 0 && errno == ECHILD) {
            reaped_ = true;
            exit_text_ = "exited";
        }
    }
    how = exit_text_;
    return reaped_;
}

void ChildProcess::kill_group(int sig)
{
    if (!reaped_) kill(-pid_, sig);
}

void ChildProcess::read_output()
{
    std::string partial;
    char buffer[65536];
    bool discarding = false;  // inside a line that grew past kMaxLine
    while (!stopping_) {
        pollfd p{stdout_fd_, POLLIN, 0};
        int r = poll(&p, 1, 100);
        if (r <= 0) continue;
        ssize_t n = read(stdout_fd_, buffer, sizeof(buffer));
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) break;
        partial.append(buffer, size_t(n));
        std::vector<std::string> complete;
        size_t from = 0;
        for (size_t nl; (nl = partial.find('\n', from)) != std::string::npos; from = nl + 1) {
            if (!discarding) complete.push_back(partial.substr(from, nl - from));
            discarding = false;
        }
        partial.erase(0, from);
        if (partial.size() > kMaxLine) {
            partial.clear();
            discarding = true;
        }
        if (!complete.empty()) {
            {
                std::lock_guard<std::mutex> lock(lines_mutex_);
                for (auto &line : complete) lines_.push_back(std::move(line));
            }
            if (wake_) wake_();
        }
    }
    {
        std::lock_guard<std::mutex> lock(lines_mutex_);
        ended_ = true;
    }
    if (wake_) wake_();
}
