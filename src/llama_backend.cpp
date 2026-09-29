#include "turnserve/backend.hpp"
#include "llama.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace turnserve {
namespace {
struct GlobalBackend {
    GlobalBackend() { llama_backend_init(); }
    ~GlobalBackend() { llama_backend_free(); }
};
struct Batch {
    llama_batch value;
    explicit Batch(int size) : value(llama_batch_init(size, 0, 1)) {}
    ~Batch() { llama_batch_free(value); }
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
};
class LlamaBackend final : public Backend {
    std::unique_ptr<llama_model, decltype(&llama_model_free)> model_{nullptr, llama_model_free};
    std::unique_ptr<llama_context, decltype(&llama_free)> context_{nullptr, llama_free};
    const llama_vocab* vocab_ = nullptr;
    int batch_tokens_;
public:
    LlamaBackend(const std::string& path, int slots, int context, int batch_tokens, int threads)
        : batch_tokens_(batch_tokens) {
        static GlobalBackend global;
        (void)global;
        auto model_params = llama_model_default_params();
        model_params.n_gpu_layers = 0;
        model_.reset(llama_model_load_from_file(path.c_str(), model_params));
        if (!model_) throw std::runtime_error("could not load GGUF model");
        if (llama_model_is_recurrent(model_.get()) || llama_model_is_hybrid(model_.get()) ||
            llama_model_is_diffusion(model_.get()) || llama_model_has_encoder(model_.get()))
            throw std::runtime_error("v0.1 supports dense decoder-only models; this architecture is unsupported");
        vocab_ = llama_model_get_vocab(model_.get());
        auto params = llama_context_default_params();
        params.n_ctx = static_cast<uint32_t>(context * slots);
        params.n_seq_max = static_cast<uint32_t>(slots);
        params.n_batch = static_cast<uint32_t>(batch_tokens);
        params.n_ubatch = static_cast<uint32_t>(batch_tokens);
        params.n_threads = threads;
        params.n_threads_batch = threads;
        params.offload_kqv = false;
        params.op_offload = false;
        params.kv_unified = true;
        context_.reset(llama_init_from_model(model_.get(), params));
        if (!context_) throw std::runtime_error("could not allocate model context");
        if (llama_n_ctx_seq(context_.get()) < static_cast<unsigned>(context))
            throw std::runtime_error("backend context smaller than requested session budget");
    }
    std::vector<Token> encode(const std::vector<Message>& messages) override {
        const char* tmpl = llama_model_chat_template(model_.get(), nullptr);
        if (!tmpl) throw std::runtime_error("model has no chat template");
        std::vector<llama_chat_message> chat;
        for (const auto& m : messages) chat.push_back({m.role.c_str(), m.content.c_str()});
        int size = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, nullptr, 0);
        if (size <= 0) throw std::runtime_error("unsupported chat template");
        std::string prompt(static_cast<std::size_t>(size), '\0');
        int written = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, prompt.data(), size);
        if (written != size) throw std::runtime_error("chat template size mismatch");
        int n = llama_tokenize(vocab_, prompt.data(), size, nullptr, 0, true, true);
        if (n == std::numeric_limits<int>::min()) throw std::runtime_error("tokenization overflow");
        std::vector<Token> tokens(static_cast<std::size_t>(n < 0 ? -n : n));
        n = llama_tokenize(vocab_, prompt.data(), size, tokens.data(), static_cast<int>(tokens.size()), true, true);
        if (n < 0) throw std::runtime_error("tokenization failed");
        tokens.resize(static_cast<std::size_t>(n));
        return tokens;
    }
    std::vector<std::optional<Token>> evaluate(const std::vector<Work>& work) override {
        int count = 0;
        for (const auto& item : work) count += static_cast<int>(item.tokens.size());
        if (count == 0 || count > batch_tokens_) throw std::runtime_error("invalid batch size");
        Batch batch(count);
        std::vector<int> logits;
        for (const auto& item : work) {
            for (std::size_t j = 0; j < item.tokens.size(); ++j) {
                int i = batch.value.n_tokens++;
                batch.value.token[i] = item.tokens[j];
                batch.value.pos[i] = item.position + static_cast<int>(j);
                batch.value.n_seq_id[i] = 1;
                batch.value.seq_id[i][0] = item.slot;
                batch.value.logits[i] = item.sample && j + 1 == item.tokens.size();
            }
            logits.push_back(item.sample ? batch.value.n_tokens - 1 : -1);
        }
        int code = llama_decode(context_.get(), batch.value);
        if (code != 0) throw std::runtime_error("llama_decode failed: " + std::to_string(code));
        std::vector<std::optional<Token>> result;
        for (int index : logits) {
            if (index < 0) { result.push_back(std::nullopt); continue; }
            const float* scores = llama_get_logits_ith(context_.get(), index);
            if (!scores) throw std::runtime_error("missing logits");
            auto best = std::max_element(scores, scores + llama_vocab_n_tokens(vocab_));
            result.push_back(static_cast<Token>(best - scores));
        }
        return result;
    }
    std::string piece(Token token) const override {
        std::string result(128, '\0');
        int size = llama_token_to_piece(vocab_, token, result.data(), static_cast<int>(result.size()), 0, false);
        if (size < 0) {
            result.resize(static_cast<std::size_t>(-size));
            size = llama_token_to_piece(vocab_, token, result.data(), static_cast<int>(result.size()), 0, false);
        }
        if (size < 0) throw std::runtime_error("token decoding failed");
        result.resize(static_cast<std::size_t>(size)); return result;
    }
    bool is_end(Token token) const override { return llama_vocab_is_eog(vocab_, token); }
    void release(int slot) override {
        if (!llama_memory_seq_rm(llama_get_memory(context_.get()), slot, -1, -1))
            throw std::runtime_error("backend cannot release sequence; aborting to avoid reusing stale state");
    }
    const char* name() const override { return "llama.cpp / CPU / greedy"; }
};
}
std::unique_ptr<Backend> make_llama_backend(const std::string& model, int slots, int context, int batch, int threads) {
    return std::make_unique<LlamaBackend>(model, slots, context, batch, threads);
}
}
