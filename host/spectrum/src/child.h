// SPDX-License-Identifier: MIT
// A command run through /bin/sh with line-oriented pipes on its stdin and stdout.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>

class ChildProcess {
public:
    // Starts `sh -c command` in a process group of its own, so that it and anything it starts can
    // be signalled together. Its stderr is this process's. `wake` is called on the reader thread
    // whenever lines, or the end of the output, arrive. Throws std::runtime_error.
    ChildProcess(const std::string &command, std::function<void()> wake);
    // Closes stdin, gives the process a moment to exit, then signals the group, and reaps it.
    ~ChildProcess();
    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;

    pid_t pid() const { return pid_; }

    // Queues `line` and a newline for stdin and writes what the pipe takes now; flush() writes
    // the rest later. False when stdin is closed or too much is already waiting.
    bool write_line(const std::string &line);
    void flush();

    // The complete lines received since the last call. `ended` becomes true once stdout closed.
    std::vector<std::string> take_lines(bool &ended);

    // Reaps the process if it has exited (never blocks). True once reaped; `how` describes it.
    bool reaped(std::string &how);
    // Sends `sig` to the process group while the process has not been reaped.
    void kill_group(int sig);

private:
    void read_output();

    pid_t pid_ = -1;
    int stdin_fd_ = -1, stdout_fd_ = -1;
    bool reaped_ = false;
    std::string exit_text_;
    std::string pending_input_;
    std::function<void()> wake_;

    std::atomic<bool> stopping_{false};
    std::thread reader_;
    std::mutex lines_mutex_;
    std::vector<std::string> lines_;
    bool ended_ = false;
};
