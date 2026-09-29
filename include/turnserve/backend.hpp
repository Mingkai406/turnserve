#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace turnserve {
using Token = std::int32_t;
struct Message { std::string role; std::string content; };
struct Work {
    int slot;
    int position;
    std::vector<Token> tokens;
    bool sample;
};
// All backend methods are called by the runtime's single owner thread.
class Backend {
public:
    virtual ~Backend() = default;
    virtual std::vector<Token> encode(const std::vector<Message>& messages) = 0;
    virtual std::vector<std::optional<Token>> evaluate(const std::vector<Work>& batch) = 0;
    virtual std::string piece(Token token) const = 0;
    virtual bool is_end(Token token) const = 0;
    virtual void release(int slot) = 0;
    virtual const char* name() const = 0;
};
std::unique_ptr<Backend> make_synthetic_backend();
#ifdef TURNSERVE_HAS_LLAMA
std::unique_ptr<Backend> make_llama_backend(const std::string& model, int slots,
                                          int context_per_slot, int batch_tokens, int threads);
#endif
}
