#pragma once
#include "backend.hpp"
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace turnserve {
using RequestId = std::uint64_t;
enum class Policy { fifo, round_robin, interleave };
Policy parse_policy(const std::string& name);
struct Config {
    Policy policy = Policy::interleave;
    int slots = 4;
    int context_tokens = 2048;
    int batch_tokens = 128;
    int prefill_chunk = 64;
    std::size_t max_commands = 64;
    std::size_t max_requests = 32;
    std::size_t max_sessions = 128;
    std::size_t history_turns = 8;
};
struct Event {
    std::string type;
    RequestId request = 0;
    std::string session;
    std::uint64_t generation = 0;
    double ms = 0;
    std::string text;
    std::size_t count = 0;
};
class Runtime {
public:
    using Sink = std::function<void(const Event&)>;
    Runtime(Backend& backend, Config config, Sink sink);
    // Producers may call these concurrently. nullopt / false means queue backpressure.
    std::optional<RequestId> submit(std::string session, std::string prompt,
                                    int max_output = 64, bool replace = false);
    bool cancel(RequestId request);
    // tick / idle / history must be called by one owner thread, never concurrently.
    bool tick();
    bool idle() const;
    // Owner-thread only; refuses sessions with active or queued work.
    bool forget_session(const std::string& session);
    std::vector<Message> history(const std::string& session) const;
private:
    using Clock = std::chrono::steady_clock;
    struct Command {
        bool cancel;
        RequestId id;
        std::string session, prompt;
        int max_output;
        bool replace;
        Clock::time_point submitted;
    };
    struct Session { std::uint64_t generation = 0; RequestId current = 0; std::vector<Message> history; };
    struct Job {
        RequestId id;
        std::string session, prompt;
        std::uint64_t generation;
        int max_output;
        std::vector<Token> input;
        std::size_t consumed = 0;
        std::vector<Token> output;
        std::string text;
        int slot = -1;
    };
    void process(Command command);
    void finish(RequestId id, const std::string& status, const std::string& reason);
    void emit(const std::string& type, const Job& job, const std::string& text = "", std::size_t count = 0);
    double now_ms() const;
    Backend& backend_;
    Config config_;
    Sink sink_;
    Clock::time_point origin_ = Clock::now();
    std::atomic<RequestId> next_id_{1};
    mutable std::mutex mutex_;
    std::deque<Command> commands_;
    std::map<RequestId, Job> jobs_;
    std::map<std::string, Session> sessions_;
    std::vector<int> free_slots_;
    std::size_t rotation_ = 0;
};
std::string event_json(const Event& event);
}
