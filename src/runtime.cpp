#include "turnserve/runtime.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace turnserve {
Policy parse_policy(const std::string& name) {
    if (name == "fifo") return Policy::fifo;
    if (name == "round-robin") return Policy::round_robin;
    if (name == "interleave") return Policy::interleave;
    throw std::invalid_argument("policy must be fifo, round-robin or interleave");
}
Runtime::Runtime(Backend& backend, Config config, Sink sink)
    : backend_(backend), config_(config), sink_(std::move(sink)) {
    if (config.slots < 1 || config.batch_tokens <= config.slots || config.prefill_chunk < 1 ||
        config.context_tokens < 2 || !config.max_commands || !config.max_requests ||
        !config.max_sessions || !config.history_turns || !sink_)
        throw std::invalid_argument("invalid runtime limits (batch_tokens must exceed slots)");
    for (int i = config.slots - 1; i >= 0; --i) free_slots_.push_back(i);
}
std::optional<RequestId> Runtime::submit(std::string session, std::string prompt, int max_output, bool replace) {
    std::lock_guard lock(mutex_);
    if (commands_.size() >= config_.max_commands) return std::nullopt;
    auto id = next_id_.fetch_add(1);
    commands_.push_back({false, id, std::move(session), std::move(prompt), max_output, replace, Clock::now()});
    return id;
}
bool Runtime::cancel(RequestId request) {
    std::lock_guard lock(mutex_);
    if (commands_.size() >= config_.max_commands) return false;
    commands_.push_back({true, request, {}, {}, 0, false, Clock::now()});
    return true;
}
double Runtime::now_ms() const { return std::chrono::duration<double, std::milli>(Clock::now() - origin_).count(); }
void Runtime::emit(const std::string& type, const Job& job, const std::string& text, std::size_t count) {
    sink_({type, job.id, job.session, job.generation, now_ms(), text, count});
}
void Runtime::finish(RequestId id, const std::string& status, const std::string& reason) {
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return;
    auto& job = it->second;
    if (job.slot >= 0) {
        backend_.release(job.slot);
        free_slots_.push_back(job.slot);
    }
    auto& session = sessions_.at(job.session);
    if (session.current == id && session.generation == job.generation) {
        if (status == "completed") {
            session.history.push_back({"user", job.prompt});
            session.history.push_back({"assistant", job.text});
            while (session.history.size() > config_.history_turns * 2)
                session.history.erase(session.history.begin(), session.history.begin() + 2);
            emit("committed", job);
        }
        session.current = 0;
    }
    emit(status, job, reason, job.output.size());
    jobs_.erase(it);
}
void Runtime::process(Command command) {
    if (command.cancel) {
        sink_({"cancel_requested", command.id, {}, 0,
               std::chrono::duration<double, std::milli>(command.submitted-origin_).count(), {}, 0});
        finish(command.id, "cancelled", "explicit cancellation");
        return;
    }
    sink_({"submitted", command.id, command.session, 0,
           std::chrono::duration<double, std::milli>(command.submitted-origin_).count(), {}, 0});
    auto reject = [&](const std::string& reason) {
        sink_({"rejected", command.id, command.session, 0, now_ms(), reason, 0});
    };
    if (command.session.empty() || command.session.size() > 128 || command.prompt.empty() ||
        command.prompt.size() > 1024 * 1024 || command.max_output < 1 || command.max_output >= config_.context_tokens) {
        reject("invalid request or size limit"); return;
    }
    auto existing = sessions_.find(command.session);
    if (existing == sessions_.end() && sessions_.size() >= config_.max_sessions) {
        reject("session limit reached"); return;
    }
    const RequestId current = existing == sessions_.end() ? 0 : existing->second.current;
    if (current && !command.replace) { reject("session busy; use explicit replacement"); return; }
    if (jobs_.size() >= config_.max_requests && !current) { reject("request limit reached"); return; }
    // Validate a replacement before cancelling useful work from the old generation.
    auto messages = existing == sessions_.end() ? std::vector<Message>{} : existing->second.history;
    messages.push_back({"user", command.prompt});
    std::vector<Token> input;
    try { input = backend_.encode(messages); }
    catch (const std::exception& error) { reject(error.what()); return; }
    if (input.empty() || input.size() + static_cast<std::size_t>(command.max_output) > static_cast<std::size_t>(config_.context_tokens)) {
        reject("prompt plus output exceeds per-session context"); return;
    }
    if (current) finish(current, "cancelled", "replaced by a new generation");
    auto& session = sessions_[command.session];
    session.current = command.id;
    ++session.generation;
    Job job{command.id, std::move(command.session), std::move(command.prompt), session.generation,
            command.max_output, std::move(input), 0, {}, {}, -1};
    auto [it, inserted] = jobs_.emplace(job.id, std::move(job));
    (void)inserted;
    emit("accepted", it->second, {}, it->second.input.size());
}
bool Runtime::tick() {
    std::deque<Command> commands;
    { std::lock_guard lock(mutex_); commands.swap(commands_); }
    for (auto& command : commands) process(std::move(command));
    for (auto& [id, job] : jobs_) {
        (void)id;
        if (job.slot < 0 && !free_slots_.empty()) {
            job.slot = free_slots_.back(); free_slots_.pop_back();
            emit("admitted", job);
        }
    }
    std::vector<RequestId> active;
    for (const auto& [id, job] : jobs_) if (job.slot >= 0) active.push_back(id);
    if (active.empty()) return !idle();
    if (config_.policy == Policy::fifo) active.resize(1);
    else {
        auto offset = rotation_++ % active.size();
        std::rotate(active.begin(), active.begin() + static_cast<std::ptrdiff_t>(offset), active.end());
    }
    if (config_.policy == Policy::interleave) {
        std::stable_partition(active.begin(), active.end(), [&](RequestId id) {
            return jobs_.at(id).consumed == jobs_.at(id).input.size();
        });
    }
    int budget = config_.batch_tokens;
    std::vector<Work> batch;
    std::vector<RequestId> owners;
    for (auto id : active) {
        auto& job = jobs_.at(id);
        if (!budget) break;
        Work work{job.slot, 0, {}, false};
        if (job.consumed < job.input.size()) {
            auto amount = std::min({static_cast<std::size_t>(budget), static_cast<std::size_t>(config_.prefill_chunk), job.input.size()-job.consumed});
            work.position = static_cast<int>(job.consumed);
            work.tokens.assign(job.input.begin()+static_cast<std::ptrdiff_t>(job.consumed),
                               job.input.begin()+static_cast<std::ptrdiff_t>(job.consumed+amount));
            work.sample = job.consumed + amount == job.input.size();
        } else {
            work.position = static_cast<int>(job.input.size() + job.output.size() - 1);
            work.tokens = {job.output.back()};
            work.sample = true;
        }
        budget -= static_cast<int>(work.tokens.size());
        emit(job.consumed < job.input.size() ? "prefill" : "decode", job, {}, work.tokens.size());
        owners.push_back(id); batch.push_back(std::move(work));
    }
    std::vector<std::optional<Token>> result;
    try {
        result = backend_.evaluate(batch);
        if (result.size() != batch.size()) throw std::runtime_error("backend result size mismatch");
        for (std::size_t i = 0; i < batch.size(); ++i)
            if (result[i].has_value() != batch[i].sample) throw std::runtime_error("backend sample contract violation");
    } catch (const std::exception& error) {
        // A failed shared decode may have partly mutated any participating sequence.
        std::vector<RequestId> failed;
        for (const auto& [id, job] : jobs_) if (job.slot >= 0) failed.push_back(id);
        for (auto id : failed) finish(id, "failed", error.what());
        return !idle();
    }
    for (std::size_t i = 0; i < owners.size(); ++i) {
        auto& job = jobs_.at(owners[i]);
        if (job.consumed < job.input.size()) job.consumed += batch[i].tokens.size();
        if (!result[i]) continue;
        auto token = *result[i];
        if (backend_.is_end(token)) { finish(job.id, "completed", "eos"); continue; }
        job.output.push_back(token);
        auto piece = backend_.piece(token);
        job.text += piece;
        emit("token", job, piece, job.output.size());
        if (job.output.size() >= static_cast<std::size_t>(job.max_output)) finish(job.id, "completed", "length");
    }
    return !idle();
}
bool Runtime::forget_session(const std::string& session) {
    std::lock_guard lock(mutex_);
    for (const auto& command : commands_)
        if (!command.cancel && command.session == session) return false;
    auto it = sessions_.find(session);
    if (it == sessions_.end()) return true;
    if (it->second.current) return false;
    sessions_.erase(it);
    return true;
}
bool Runtime::idle() const {
    std::lock_guard lock(mutex_);
    return commands_.empty() && jobs_.empty();
}
std::vector<Message> Runtime::history(const std::string& session) const {
    auto it = sessions_.find(session);
    return it == sessions_.end() ? std::vector<Message>{} : it->second.history;
}
namespace {
std::string quote(const std::string& value) {
    std::ostringstream out; out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 0x20 || c >= 0x80) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(c) << std::dec;
        else out << c;
    }
    out << '"'; return out.str();
}
}
std::string event_json(const Event& e) {
    // text uses byte-preserving Latin-1 JSON escapes: reassemble token bytes before UTF-8 decoding.
    std::ostringstream out;
    out << "{\"type\":" << quote(e.type) << ",\"request\":" << e.request << ",\"session\":" << quote(e.session)
        << ",\"generation\":" << e.generation << ",\"ms\":" << std::fixed << std::setprecision(6) << e.ms
        << ",\"text\":" << quote(e.text) << ",\"count\":" << e.count << '}';
    return out.str();
}
}
