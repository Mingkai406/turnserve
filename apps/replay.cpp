#include "turnserve/runtime.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace turnserve;
int main(int argc, char** argv) try {
    Config config;
    std::string model, trace, policy = "interleave";
    int threads = 4;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help") {
            std::cout << "turnserve-replay [--model file.gguf] [--policy fifo|round-robin|interleave]\n"
                         "                 [--trace events.jsonl] [--threads N]\n"
                         "Without --model, runs the synthetic contract demonstration (not a performance benchmark).\n";
            return 0;
        }
        if (++i >= argc) throw std::invalid_argument("missing argument for " + arg);
        if (arg == "--model") model = argv[i];
        else if (arg == "--trace") trace = argv[i];
        else if (arg == "--policy") policy = argv[i];
        else if (arg == "--threads") threads = std::stoi(argv[i]);
        else throw std::invalid_argument("unknown option " + arg);
    }
    if (threads < 1 || threads > 256) throw std::invalid_argument("threads must be in [1,256]");
    config.policy = parse_policy(policy);
    auto backend = make_synthetic_backend();
    if (!model.empty()) {
#ifdef TURNSERVE_HAS_LLAMA
        backend = make_llama_backend(model, config.slots, config.context_tokens, config.batch_tokens, threads);
#else
        throw std::runtime_error("rebuild with -DTURNSERVE_LLAMA=ON to use real weights");
#endif
    }
    std::ofstream file;
    if (!trace.empty()) {
        file.open(trace);
        if (!file) throw std::runtime_error("cannot open trace file");
    }
    std::ostream& output = trace.empty() ? std::cout : file;
    output << event_json({"run", 0, policy, 0, 0, backend->name(), 0}) << '\n';
    Runtime* runtime_ptr = nullptr;
    RequestId interrupted = 0;
    bool replaced = false;
    int completed = 0, cancelled = 0, failed = 0, rejected = 0;
    Runtime runtime(*backend, config, [&](const Event& event) {
        output << event_json(event) << '\n';
        if (!output) throw std::runtime_error("trace write failed");
        if (event.type == "completed") ++completed;
        if (event.type == "cancelled") ++cancelled;
        if (event.type == "failed") ++failed;
        if (event.type == "rejected") ++rejected;
        if (event.type == "token" && event.request == interrupted && event.count == 3 && !replaced) {
            replaced = true;
            if (!runtime_ptr->submit("revision", "Change of plan: name one planet in the solar system.", 24, true))
                throw std::runtime_error("replacement queue unexpectedly full");
        }
    });
    runtime_ptr = &runtime;
    std::string document = "Summarize the following observation in one sentence.\n";
    for (int i = 0; i < 40; ++i) document += "The station records temperature, wind, and rainfall each morning. ";
    runtime.submit("document", document, 48);
    runtime.submit("short", "What is the capital of France? Answer briefly.", 24);
    interrupted = *runtime.submit("revision", "List ten practical ways to organize a busy research week.", 80);
    int ticks = 0;
    while (!runtime.idle()) {
        runtime.tick();
        if (++ticks > 10000) throw std::runtime_error("runtime did not drain");
    }
    std::cerr << "backend=" << backend->name() << " policy=" << policy << " completed=" << completed
              << " cancelled=" << cancelled << " rejected=" << rejected << " failed=" << failed << '\n';
    if (failed || rejected || cancelled != 1 || completed != 3) return 2;
    return 0;
} catch (const std::exception& error) {
    std::cerr << "turnserve: " << error.what() << '\n'; return 1;
}
