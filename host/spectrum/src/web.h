// SPDX-License-Identifier: MIT
// A small HTTP/1.1 and WebSocket server (RFC 6455, with permessage-deflate,
// RFC 7692) on one epoll thread: static files, and WebSocket clients at /ws.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// A file of the web UI compiled into the tool (build/web_assets.cpp).
struct EmbeddedFile {
    const char *path;  // "/index.html"
    const unsigned char *data;
    size_t size;
};
extern const EmbeddedFile kWebFiles[];
extern const size_t kWebFileCount;

class WebServer {
public:
    struct Handler {
        std::function<void(unsigned client)> open;
        std::function<void(unsigned client, const std::string &message)> text;
        std::function<void(unsigned client)> close;
        std::function<void()> wake;  // after wake() was called on any thread
        std::function<void()> tick;  // every 20 ms or so
    };

    // listen: "ADDRESS:PORT" or "PORT". web_dir: serve files from there,
    // read on every request, instead of the built-in copy.
    WebServer(const std::string &listen, const std::string &web_dir, Handler handler);
    ~WebServer();
    WebServer(const WebServer &) = delete;
    WebServer &operator=(const WebServer &) = delete;

    const std::string &address() const { return address_; }
    // Runs the event loop on the calling thread until `stop` is set.
    void run(const std::atomic<bool> &stop);
    // Wakes the loop, which then calls Handler::wake. Any thread.
    void wake();

    // The rest is for the loop thread (inside the handler callbacks).
    void send_text(unsigned client, const std::string &text);
    void send_binary(unsigned client, const std::string &data);
    size_t queued(unsigned client) const;       // bytes the socket has not taken yet
    uint64_t sent(unsigned client) const;       // bytes on the wire so far
    bool compressing(unsigned client) const;    // permessage-deflate in use
    std::string peer(unsigned client) const;
    void close(unsigned client, uint16_t code, const std::string &reason);

private:
    struct Asset;
    struct Connection;

    void accept_clients();
    void receive(Connection &c);
    void flush(Connection &c);
    void handle_http(Connection &c);
    bool upgrade(Connection &c, const std::map<std::string, std::string> &headers);
    void respond(Connection &c, const std::string &method, const std::string &path,
                 const std::map<std::string, std::string> &headers);
    void handle_frames(Connection &c);
    void send_frame(Connection &c, int opcode, const std::string &payload, bool compress);
    void reap();  // removes dead connections; safe only between events
    Connection *find(unsigned client) const;

    Handler handler_;
    std::string web_dir_;
    std::vector<std::unique_ptr<Asset>> assets_;
    std::string address_;
    int listener_ = -1, epoll_ = -1, event_ = -1;
    unsigned next_id_ = 1;
    std::map<unsigned, std::unique_ptr<Connection>> connections_;
};
