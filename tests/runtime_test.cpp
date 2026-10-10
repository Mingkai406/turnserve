#include "turnserve/runtime.hpp"
#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

using namespace turnserve;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void drain(Runtime& runtime) {
    int n = 0;
    while (!runtime.idle()) { runtime.tick(); require(++n < 10000, "drain timeout"); }
}
int count(const std::vector<Event>& events, RequestId id, const std::string& type) {
    return static_cast<int>(std::count_if(events.begin(), events.end(), [&](const auto& e) { return e.request == id && e.type == type; }));
}
struct Fixture {
    std::unique_ptr<Backend> backend = make_synthetic_backend();
    std::vector<Event> events;
    Runtime runtime;
    explicit Fixture(Config c = {}) : runtime(*backend, c, [&](const Event& e) { events.push_back(e); }) {}
};
void cancellation_and_replacement() {
    Fixture f;
    auto old = *f.runtime.submit("s", "a sufficiently long question", 30);
    f.runtime.tick();
    auto next = *f.runtime.submit("s", "a new question", 3, true);
    drain(f.runtime);
    require(count(f.events, old, "cancelled") == 1, "old request must cancel exactly once");
    require(count(f.events, old, "committed") == 0, "cancelled text leaked into history");
    require(count(f.events, next, "completed") == 1, "replacement did not complete");
    auto h = f.runtime.history("s");
    require(h.size() == 2 && h[0].content == "a new question", "replacement history incorrect");
    f.runtime.cancel(old); f.runtime.cancel(next); drain(f.runtime);
    require(count(f.events, next, "cancelled") == 0, "late cancel changed a terminal result");
    bool after = false;
    for (const auto& e : f.events) {
        if (e.request == old && e.type == "cancelled") after = true;
        require(!(after && e.request == old && e.type == "token"), "token emitted after cancel acknowledgement");
    }
}
void queued_cancel_and_limits() {
    Config c; c.slots = 1; c.max_commands = 3; c.max_requests = 2;
    Fixture f(c);
    auto a = *f.runtime.submit("a", "one", 10);
    auto b = *f.runtime.submit("b", "two", 10);
    auto rejected = *f.runtime.submit("c", "three", 10);
    require(!f.runtime.submit("d", "four"), "command backpressure missing");
    f.runtime.tick(); f.runtime.cancel(b); drain(f.runtime);
    require(count(f.events, b, "cancelled") == 1 && count(f.events, b, "token") == 0, "queued cancellation failed");
    require(count(f.events, rejected, "rejected") == 1, "request admission cap ignored");
    require(count(f.events, a, "completed") == 1, "active request damaged");
}
void invalid_replacement_keeps_old() {
    Config c; c.context_tokens = 32;
    Fixture f(c);
    auto a = *f.runtime.submit("a", "old", 10);
    f.runtime.tick();
    auto b = *f.runtime.submit("a", std::string(400, 'z'), 3, true);
    drain(f.runtime);
    require(count(f.events, b, "rejected") == 1, "oversized replacement accepted");
    require(count(f.events, a, "completed") == 1, "invalid replacement cancelled useful work");
}
void histories_and_busy_sessions() {
    Config c; c.history_turns = 1; c.max_sessions = 2;
    Fixture f(c);
    f.runtime.submit("a", "first", 2);
    auto busy = *f.runtime.submit("a", "conflict", 2);
    f.runtime.submit("b", "separate", 2); drain(f.runtime);
    require(count(f.events, busy, "rejected") == 1, "overlapping same-session turn accepted");
    f.runtime.submit("a", "second", 2); drain(f.runtime);
    require(f.runtime.history("a").size() == 2 && f.runtime.history("a")[0].content == "second", "history retention broken");
    require(f.runtime.history("b")[0].content == "separate", "cross-session history contamination");
    auto overflow = *f.runtime.submit("c", "extra", 2); drain(f.runtime);
    require(count(f.events, overflow, "rejected") == 1, "session cap ignored");
}
void session_reclamation() {
    Config c; c.max_sessions = 1;
    Fixture f(c);
    f.runtime.submit("a", "first", 2);
    require(!f.runtime.forget_session("a"), "forgot queued session");
    f.runtime.tick();
    require(!f.runtime.forget_session("a"), "forgot active session");
    drain(f.runtime);
    require(f.runtime.forget_session("a"), "terminal session not reclaimed");
    require(f.runtime.history("a").empty(), "forgotten history remains");
    auto next = *f.runtime.submit("b", "second", 2); drain(f.runtime);
    require(count(f.events, next, "completed") == 1, "session capacity not reclaimed");
}
void policy_progress() {
    for (auto policy : {Policy::fifo, Policy::round_robin, Policy::interleave}) {
        Config c; c.policy = policy; c.batch_tokens = 8; c.prefill_chunk = 4;
        Fixture f(c);
        auto a = *f.runtime.submit("long", std::string(400, 'a'), 8);
        auto b = *f.runtime.submit("short", "hi", 4);
        drain(f.runtime);
        require(count(f.events, a, "completed") == 1 && count(f.events, b, "completed") == 1, "policy failed to make progress");
        if (policy != Policy::fifo) {
            auto short_token = std::find_if(f.events.begin(), f.events.end(), [&](auto& e) { return e.request == b && e.type == "token"; });
            auto long_token = std::find_if(f.events.begin(), f.events.end(), [&](auto& e) { return e.request == a && e.type == "token"; });
            require(short_token < long_token, "short turn blocked behind full long prefill");
        }
    }
}
class FailureBackend final : public Backend {
    std::unique_ptr<Backend> inner = make_synthetic_backend();
public:
    int releases = 0;
    std::vector<Token> encode(const std::vector<Message>& m) override { return inner->encode(m); }
    std::vector<std::optional<Token>> evaluate(const std::vector<Work>&) override { throw std::runtime_error("injected decode failure"); }
    std::string piece(Token t) const override { return inner->piece(t); }
    bool is_end(Token) const override { return false; }
    void release(int) override { ++releases; }
    const char* name() const override { return "failure"; }
};
void failure_cleanup() {
    FailureBackend b; std::vector<Event> events;
    Runtime r(b, {}, [&](const Event& e) { events.push_back(e); });
    auto a = *r.submit("a", "hello", 3); auto other = *r.submit("b", "world", 3);
    drain(r);
    require(count(events, a, "failed") == 1 && count(events, other, "failed") == 1, "shared failure not propagated");
    require(b.releases == 2 && r.history("a").empty(), "failed state not cleaned");
}
void concurrent_producers() {
    Config c; c.max_commands = 128; c.max_requests = 128; c.max_sessions = 128;
    Fixture f(c);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back([&, t] {
        for (int i = 0; i < 16; ++i)
            require(f.runtime.submit(std::to_string(t)+"-"+std::to_string(i), "hello", 2).has_value(), "unexpected backpressure");
    });
    for (auto& thread : threads) thread.join();
    drain(f.runtime);
    std::set<RequestId> completed;
    for (auto& e : f.events) if (e.type == "completed") require(completed.insert(e.request).second, "duplicate terminal");
    require(completed.size() == 64, "concurrent submissions lost");
}
class BlockingBackend final : public Backend {
    std::unique_ptr<Backend> inner = make_synthetic_backend();
public:
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, proceed = false;
    std::vector<Token> encode(const std::vector<Message>& m) override { return inner->encode(m); }
    std::vector<std::optional<Token>> evaluate(const std::vector<Work>& batch) override {
        std::unique_lock lock(mutex); entered = true; cv.notify_all();
        cv.wait(lock, [&] { return proceed; });
        return inner->evaluate(batch);
    }
    std::string piece(Token t) const override { return inner->piece(t); }
    bool is_end(Token) const override { return false; }
    void release(int) override {}
    const char* name() const override { return "blocking"; }
};
void cancel_during_decode() {
    BlockingBackend b; std::vector<Event> events;
    Runtime r(b, {}, [&](const Event& e) { events.push_back(e); });
    auto id = *r.submit("a", "hello", 10);
    std::thread producer([&] {
        std::unique_lock lock(b.mutex);
        b.cv.wait(lock, [&] { return b.entered; });
        require(r.cancel(id), "concurrent cancellation rejected");
        b.proceed = true; b.cv.notify_all();
    });
    r.tick(); producer.join(); drain(r);
    require(count(events, id, "cancelled") == 1 && count(events, id, "committed") == 0, "in-flight cancellation not applied");
    require(count(events, id, "token") == 1, "expected one already-in-flight token before acknowledgement");
    require(events.back().type == "cancelled", "event after cancellation acknowledgement");
}
int main() try {
    cancellation_and_replacement(); queued_cancel_and_limits(); invalid_replacement_keeps_old();
    histories_and_busy_sessions(); policy_progress(); failure_cleanup(); concurrent_producers();
    cancel_during_decode(); session_reclamation();
    std::cout << "9 contract suites passed\n"; return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
