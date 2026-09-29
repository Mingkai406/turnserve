#include "turnserve/backend.hpp"
#include <algorithm>

namespace turnserve {
namespace {
class SyntheticBackend final : public Backend {
public:
    std::vector<Token> encode(const std::vector<Message>& messages) override {
        std::size_t bytes = 0;
        for (const auto& message : messages) bytes += message.content.size();
        return std::vector<Token>(std::max<std::size_t>(1, bytes / 4), 1);
    }
    std::vector<std::optional<Token>> evaluate(const std::vector<Work>& batch) override {
        std::vector<std::optional<Token>> result;
        for (const auto& work : batch) result.push_back(work.sample ? std::optional<Token>(42) : std::nullopt);
        return result;
    }
    std::string piece(Token) const override { return "x"; }
    bool is_end(Token) const override { return false; }
    void release(int) override {}
    const char* name() const override { return "synthetic (no model inference)"; }
};
}
std::unique_ptr<Backend> make_synthetic_backend() { return std::make_unique<SyntheticBackend>(); }
}
