// Deliberately small loopback HTTP/1.1 transport. Runtime and sockets share one owner.
#include "turnserve/runtime.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>

using namespace turnserve;
using Clock = std::chrono::steady_clock;
static volatile std::sig_atomic_t stopping = 0;
static void stop(int) { stopping = 1; }
static bool integer(const std::string& s, int& value) {
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    return !s.empty() && ec == std::errc{} && end == s.data() + s.size();
}
static void nonblocking(int fd) {
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) throw std::runtime_error("fcntl failed");
}
struct Options {
    int port = 8080, clients = 64, inflight = 16, buffer = 65536;
    int timeout = 5000, grace = 5000, delay = 0, threads = 4;
    std::string model;
};
struct Client {
    int fd;
    std::string input, output, session;
    RequestId request = 0;
    bool streaming = false, done = false, dead = false;
    Clock::time_point touched = Clock::now();
};
struct Metrics {
    std::uint64_t submitted = 0, completed = 0, cancelled = 0, failed = 0;
    std::uint64_t rejected = 0, disconnects = 0, slow = 0, peak = 0;
};
class Server {
    Options o;
    Config config;
    std::unique_ptr<Backend> backend;
    std::unique_ptr<Runtime> runtime;
    int listener = -1;
    std::vector<Client> clients;
    std::map<RequestId, std::string> active;
    std::vector<RequestId> pending_cancel;
    std::vector<std::string> cleanup;
    Metrics metrics;
    std::uint64_t sessions = 0;
    Clock::time_point shutdown_at{};

    void queue(Client& c, const std::string& data) {
        if (c.dead) return;
        if (c.output.size() + data.size() > static_cast<std::size_t>(o.buffer)) {
            ++metrics.slow;
            c.dead = true;
            return;
        }
        c.output += data;
        metrics.peak = std::max(metrics.peak, static_cast<std::uint64_t>(c.output.size()));
    }
    void reply(Client& c, int code, const std::string& body, const std::string& type = "text/plain") {
        std::string reason = code == 200 ? "OK" : code == 503 ? "Service Unavailable" : "Bad Request";
        queue(c, "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\nContent-Type: " + type +
                 "\r\nContent-Length: " + std::to_string(body.size()) +
                 "\r\nConnection: close\r\n\r\n" + body);
        c.done = true;
    }
    std::string stats() const {
        std::ostringstream s;
        s << "{\"submitted\":" << metrics.submitted << ",\"completed\":" << metrics.completed
          << ",\"cancelled\":" << metrics.cancelled << ",\"failed\":" << metrics.failed
          << ",\"rejected\":" << metrics.rejected << ",\"disconnects\":" << metrics.disconnects
          << ",\"slow_clients\":" << metrics.slow << ",\"peak_buffer_bytes\":" << metrics.peak
          << ",\"active\":" << active.size() << ",\"connections\":" << clients.size()
          << ",\"buffer_limit_bytes\":" << o.buffer << "}\n";
        return s.str();
    }
    void event(const Event& e) {
        const bool terminal = e.type == "completed" || e.type == "cancelled" ||
                              e.type == "failed" || e.type == "rejected";
        for (auto& c : clients) if (c.request == e.request) {
            queue(c, "event: " + e.type + "\ndata: " + event_json(e) + "\n\n");
            if (terminal) c.done = true;
            break;
        }
        if (!terminal) return;
        if (e.type == "completed") ++metrics.completed;
        if (e.type == "cancelled") ++metrics.cancelled;
        if (e.type == "failed") ++metrics.failed;
        if (e.type == "rejected") ++metrics.rejected;
        auto it = active.find(e.request);
        if (it != active.end()) { cleanup.push_back(it->second); active.erase(it); }
    }
    void parse(Client& c) {
        auto split = c.input.find("\r\n\r\n");
        if (split == std::string::npos) {
            if (c.input.size() > 8192) reply(c, 431, "headers too large\n");
            return;
        }
        if (split > 8192) { reply(c, 431, "headers too large\n"); return; }
        std::istringstream lines(c.input.substr(0, split));
        std::string line, method, target, protocol, extra;
        std::getline(lines, line);
        std::istringstream first(line);
        first >> method >> target >> protocol;
        if (protocol != "HTTP/1.1" || (first >> extra)) { reply(c, 400, "invalid request line\n"); return; }
        int length = -1, output = 64;
        bool length_seen = false, output_seen = false;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto colon = line.find(':');
            if (colon == std::string::npos || colon == 0) { reply(c, 400, "invalid header\n"); return; }
            auto name = line.substr(0, colon), value = line.substr(colon + 1);
            for (auto& ch : name) {
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '-')) {
                    reply(c, 400, "invalid header name\n"); return;
                }
                if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
            }
            auto start = value.find_first_not_of(" \t");
            value = start == std::string::npos ? "" : value.substr(start);
            auto end = value.find_last_not_of(" \t");
            if (end != std::string::npos) value.resize(end + 1);
            if (name == "transfer-encoding" || name == "expect") {
                reply(c, 400, "chunked bodies and Expect are unsupported\n"); return;
            }
            if (name == "content-length") {
                if (length_seen || !integer(value, length) || length < 0) { reply(c, 400, "invalid length\n"); return; }
                length_seen = true;
            }
            if (name == "x-max-tokens") {
                if (output_seen || !integer(value, output) || output < 1 || output > 1024) {
                    reply(c, 400, "X-Max-Tokens must be 1..1024\n"); return;
                }
                output_seen = true;
            }
        }
        if (method == "GET" && (target == "/health" || target == "/metrics")) {
            if (length > 0 || c.input.size() != split + 4) { reply(c, 400, "unexpected body\n"); return; }
            reply(c, 200, target == "/health" ? "ok\n" : stats(), target == "/health" ? "text/plain" : "application/json");
            return;
        }
        if (method != "POST" || target != "/generate") { reply(c, 404, "unknown endpoint\n"); return; }
        if (length < 1 || length > 65536) { reply(c, 413, "body must be 1..65536 bytes\n"); return; }
        const auto needed = split + 4 + static_cast<std::size_t>(length);
        if (c.input.size() < needed) return;
        if (c.input.size() != needed) { reply(c, 400, "pipelining unsupported\n"); return; }
        if (stopping || active.size() >= static_cast<std::size_t>(o.inflight)) {
            ++metrics.rejected; reply(c, 503, "admission limit; retry later\n"); return;
        }
        c.session = "http-" + std::to_string(++sessions);
        auto id = runtime->submit(c.session, c.input.substr(split + 4), output);
        if (!id) { ++metrics.rejected; reply(c, 503, "command queue full\n"); return; }
        c.request = *id; c.streaming = true; ++metrics.submitted;
        active[*id] = c.session;
        c.input.clear();
        queue(c, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n");
    }
public:
    explicit Server(Options options) : o(std::move(options)) {
        config.max_requests = o.inflight;
        config.max_sessions = o.inflight;
        config.max_commands = 2 * static_cast<std::size_t>(o.inflight);
        if (o.model.empty()) backend = make_synthetic_backend();
        else {
#ifdef TURNSERVE_HAS_LLAMA
            backend = make_llama_backend(o.model, config.slots, config.context_tokens, config.batch_tokens, o.threads);
#else
            throw std::runtime_error("rebuild with TURNSERVE_LLAMA=ON to use a model");
#endif
        }
        runtime = std::make_unique<Runtime>(*backend, config, [this](const Event& e) { event(e); });
        listener = socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0) throw std::runtime_error("socket failed");
        int yes = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(o.port);
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener, 64))
            throw std::runtime_error("bind/listen failed");
        nonblocking(listener);
        socklen_t size = sizeof(address);
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size);
        std::cout << "{\"port\":" << ntohs(address.sin_port) << ",\"backend\":\"" << backend->name()
                  << "\",\"synthetic_tick_delay_ms\":" << o.delay << "}" << std::endl;
    }
    ~Server() {
        if (listener >= 0) close(listener);
        for (const auto& c : clients) close(c.fd);
    }
    void run() {
        while (!stopping || !clients.empty() || !runtime->idle()) {
            if (stopping && listener >= 0) {
                close(listener); listener = -1;
                shutdown_at = Clock::now();
            }
            const bool expired = stopping && Clock::now() - shutdown_at >= std::chrono::milliseconds(o.grace);
            std::vector<pollfd> fds;
            if (listener >= 0) fds.push_back({listener, POLLIN, 0});
            for (const auto& c : clients) fds.push_back({c.fd, static_cast<short>(POLLIN | (c.output.empty() ? 0 : POLLOUT)), 0});
            if (poll(fds.data(), fds.size(), runtime->idle() ? 10 : 0) < 0 && errno != EINTR)
                throw std::runtime_error("poll failed");
            std::size_t offset = listener >= 0 ? 1 : 0;
            for (std::size_t i = 0; i < clients.size(); ++i) {
                auto& c = clients[i]; auto flags = fds[i + offset].revents;
                if (expired) c.dead = true;
                if (flags & (POLLERR | POLLNVAL)) c.dead = true;
                if (flags & (POLLIN | POLLHUP)) {
                    char data[4096]; auto n = recv(c.fd, data, sizeof(data), 0);
                    if (n == 0) { c.dead = true; if (c.request && !c.done) ++metrics.disconnects; }
                    else if (n < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) c.dead = true; }
                    else if (c.streaming || c.done) c.dead = true;
                    else {
                        c.input.append(data, static_cast<std::size_t>(n));
                        parse(c);
                    }
                }
                if (!c.dead && (flags & POLLOUT)) {
                    auto n = send(c.fd, c.output.data(), c.output.size(), 0);
                    if (n > 0) { c.output.erase(0, static_cast<std::size_t>(n)); c.touched = Clock::now(); }
                    else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) c.dead = true;
                }
                // Request deadline is absolute until admission; streaming timeout measures stalled writes.
                if (Clock::now() - c.touched > std::chrono::milliseconds(o.timeout)) {
                    if (!c.streaming && !c.done) reply(c, 408, "request deadline exceeded\n");
                    else if (!c.output.empty()) { c.dead = true; ++metrics.slow; }
                }
            }
            if (listener >= 0 && (fds[0].revents & POLLIN)) {
                // One accept per iteration bounds work even during a connection flood.
                int fd = accept(listener, nullptr, nullptr);
                if (fd >= 0) {
                    if (clients.size() >= static_cast<std::size_t>(o.clients)) close(fd);
                    else {
                        nonblocking(fd);
                        int bytes = 8192; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
                        clients.push_back(Client{fd, {}, {}, {}});
                    }
                }
            }
            for (auto it = clients.begin(); it != clients.end();) {
                if (it->dead || (it->done && it->output.empty())) {
                    if (it->request && active.count(it->request)) pending_cancel.push_back(it->request);
                    close(it->fd); it = clients.erase(it);
                } else ++it;
            }
            for (auto it = pending_cancel.begin(); it != pending_cancel.end();) {
                if (!active.count(*it) || runtime->cancel(*it)) it = pending_cancel.erase(it);
                else ++it;
            }
            if (!runtime->idle()) {
                runtime->tick();
                if (o.delay) std::this_thread::sleep_for(std::chrono::milliseconds(o.delay));
            }
            for (const auto& session : cleanup) runtime->forget_session(session);
            cleanup.clear();
        }
        std::cout << stats() << std::flush;
    }
};
int main(int argc, char** argv) try {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        if (i + 1 >= argc) throw std::runtime_error("missing option value");
        std::string value = argv[++i];
        if (key == "--model") { o.model = value; continue; }
        int n;
        if (!integer(value, n) || n < 0) throw std::runtime_error("invalid numeric option");
        if (key == "--port" && n <= 65535) o.port = n;
        else if (key == "--max-clients" && n >= 2 && n <= 1024) o.clients = n;
        else if (key == "--max-inflight" && n >= 1 && n <= 256) o.inflight = n;
        else if (key == "--buffer-bytes" && n >= 1024 && n <= 1048576) o.buffer = n;
        else if (key == "--timeout-ms" && n >= 100 && n <= 60000) o.timeout = n;
        else if (key == "--grace-ms" && n <= 60000) o.grace = n;
        else if (key == "--synthetic-tick-delay-ms" && n <= 100) o.delay = n;
        else if (key == "--threads" && n >= 1 && n <= 128) o.threads = n;
        else throw std::runtime_error("unknown or out-of-range option: " + key);
    }
    if (!o.model.empty() && o.delay) throw std::runtime_error("synthetic delay cannot be used with a real model");
    std::signal(SIGINT, stop); std::signal(SIGTERM, stop); std::signal(SIGPIPE, SIG_IGN);
    Server server(o); server.run();
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
