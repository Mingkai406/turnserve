#include "turnserve/runtime.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

using namespace turnserve;
using Clock = std::chrono::steady_clock;
struct Arrival { double ms; std::string session; int tokens; std::string prompt; };
int main(int argc, char** argv) try {
    Config config;
    std::string model, trace, workload, policy = "interleave";
    int threads = 2;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (++i >= argc) throw std::invalid_argument("missing option value");
        if (arg == "--model") model = argv[i];
        else if (arg == "--trace") trace = argv[i];
        else if (arg == "--workload") workload = argv[i];
        else if (arg == "--policy") policy = argv[i];
        else if (arg == "--threads") threads = std::stoi(argv[i]);
        else throw std::invalid_argument("unknown option " + arg);
    }
    config.policy = parse_policy(policy);
    if (threads < 1 || threads > 256 || workload.empty() || trace.empty())
        throw std::invalid_argument("require --workload TSV --trace JSONL; threads in [1,256]");
    std::ifstream input(workload);
    if (!input) throw std::runtime_error("cannot open workload");
    std::vector<Arrival> arrivals;
    std::set<std::string> sessions;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string time, session, tokens, prompt;
        if (!std::getline(row, time, '\t') || !std::getline(row, session, '\t') ||
            !std::getline(row, tokens, '\t') || !std::getline(row, prompt))
            throw std::invalid_argument("expected arrival_ms TAB unique_session TAB tokens TAB prompt");
        size_t consumed = 0;
        double ms = std::stod(time, &consumed);
        if (consumed != time.size()) throw std::invalid_argument("invalid time");
        int n = std::stoi(tokens, &consumed);
        if (consumed != tokens.size() || !std::isfinite(ms) || ms < 0 || ms > 60000 ||
            (!arrivals.empty() && ms < arrivals.back().ms) || n < 1 || n > 256 ||
            session.empty() || !sessions.insert(session).second || prompt.empty() ||
            prompt.size() > 10000 || arrivals.size() >= 10000)
            throw std::invalid_argument("invalid or unbounded workload");
        arrivals.push_back({ms, session, n, prompt});
    }
    if (arrivals.empty()) throw std::invalid_argument("empty workload");
    auto backend = make_synthetic_backend();
    if (!model.empty()) {
#ifdef TURNSERVE_HAS_LLAMA
        backend = make_llama_backend(model, config.slots, config.context_tokens, config.batch_tokens, threads);
#else
        throw std::runtime_error("build with TURNSERVE_LLAMA=ON for model inference");
#endif
    }
    std::ofstream output(trace);
    if (!output) throw std::runtime_error("cannot open trace");
    std::mutex output_mutex;
    auto start = Clock::now();
    auto ms = [&] { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };
    auto emit = [&](const Event& event) {
        std::lock_guard lock(output_mutex);
        output << event_json(event) << '\n';
    };
    emit({"run", 0, policy, 0, 0, backend->name(), static_cast<size_t>(threads)});
    Runtime runtime(*backend, config, [&](const Event& event) {
        auto observed = event;
        observed.ms = ms(); // Same clock as offered and dispatched events.
        emit(observed);
    });
    std::atomic<bool> done = false, stop = false;
    std::exception_ptr producer_error;
    // The producer does not wait for completions; saturated runs retain every offered request.
    std::jthread producer([&] {
        try {
            for (const auto& a : arrivals) {
                while (!stop && ms() < a.ms) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (stop) break;
                auto dispatched = ms();
                auto id = runtime.submit(a.session, a.prompt, a.tokens);
                emit({"offered", id.value_or(0), a.session, 0, a.ms, "", 0});
                emit({"dispatched", id.value_or(0), a.session, 0, dispatched, "", 0});
                if (!id) emit({"queue_rejected", 0, a.session, 0, ms(), "command_queue_full", 0});
            }
        } catch (...) { producer_error = std::current_exception(); }
        done = true;
    });
    try {
        while (!done || !runtime.idle()) {
            if (ms() > arrivals.back().ms + 120000) throw std::runtime_error("drain deadline exceeded");
            if (!runtime.tick()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } catch (...) {
        stop = true;
        producer.join();
        throw;
    }
    producer.join();
    if (producer_error) std::rethrow_exception(producer_error);
    output.flush();
    if (!output) throw std::runtime_error("trace write failed");
    return 0;
} catch (const std::exception& e) {
    std::cerr << "turnserve-load: " << e.what() << '\n';
    return 1;
}
