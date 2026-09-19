#ifndef ATTENTION_H
#define ATTENTION_H

#include "rmsnorm.hpp"

#include <cmath>
#include <string>

#include <torch/torch.h>

// Hyperparameters struct
struct AttentionOptions {
  AttentionOptions(std::string attn_type, int64_t sliding_window,
                   int64_t h_size, int64_t n_heads, int64_t n_kv_heads,
                   int64_t head_dim)
      : attention_type(attn_type), sliding_window(sliding_window),
        hidden_size(h_size), num_heads(n_heads), num_kv_heads(n_kv_heads),
        head_dim(head_dim) {}

  std::string attention_type;
  int64_t sliding_window;
  int64_t hidden_size;
  int64_t num_heads;
  int64_t num_kv_heads;
  int64_t head_dim;
};

// Implementation of the custom module to avoid constantly using
// std::shared_ptr, instead it's passed as a pointer to the class torch
// accepts, ModuleHolder
class AttentionImpl : public torch::nn::Module {
public:
  // Hyperparameters
  AttentionOptions options;

  // Parameters
  // Q, K, V, O
  torch::nn::Linear q_proj{nullptr}, k_proj{nullptr}, v_proj{nullptr},
      o_proj{nullptr};
  RMSNorm q_norm{nullptr}, k_norm{nullptr};

  /* Non option-struct constructor */
  AttentionImpl(std::string attn_type, int64_t sliding_window, int64_t h_size,
                int64_t n_heads, int64_t n_kv_heads, int64_t head_dim)
      : AttentionImpl(AttentionOptions(attn_type, sliding_window, h_size,
                                       n_heads, n_kv_heads, head_dim)) {}

  /* Option-struct constructor */
  explicit AttentionImpl(const AttentionOptions &options_) : options(options_) {

    q_proj = register_module(
        "q_proj", torch::nn::Linear(torch::nn::LinearOptions(
                                        options.hidden_size,
                                        options.num_heads * options.head_dim)
                                        .bias(false)));
    k_proj = register_module(
        "k_proj", torch::nn::Linear(torch::nn::LinearOptions(
                                        options.hidden_size,
                                        options.num_kv_heads * options.head_dim)
                                        .bias(false)));
    v_proj = register_module(
        "v_proj", torch::nn::Linear(torch::nn::LinearOptions(
                                        options.hidden_size,
                                        options.num_kv_heads * options.head_dim)
                                        .bias(false)));
    o_proj = register_module(
        "o_proj", torch::nn::Linear(torch::nn::LinearOptions(
                                        options.num_heads * options.head_dim,
                                        options.hidden_size)
                                        .bias(false)));

    q_norm = register_module("q_norm", RMSNorm(options.head_dim));
    k_norm = register_module("k_norm", RMSNorm(options.head_dim));
  }

  torch::Tensor forward(torch::Tensor hidden_states) {

    torch::Tensor logits;

    // Queries
    torch::Tensor Q = q_proj(hidden_states);
    Q = Q.view({-1, options.num_heads, options.head_dim});
    Q = q_norm(Q);

    // Keys
    torch::Tensor K = k_proj(hidden_states);
    K = K.view({-1, options.num_kv_heads, options.head_dim});
    K = k_norm(K);

    // Rotational Positional Encodings (RoPE)
    double theta =
        options.attention_type == "full_attention" ? 1000000.0 : 10000.0;
    torch::Tensor inv_freq = torch::pow(
        theta, -torch::arange(0, options.head_dim, 2, hidden_states.options()) /
                   static_cast<double>(options.head_dim));
    torch::Tensor freqs = torch::outer(
        torch::arange(hidden_states.size(0), hidden_states.options()),
        inv_freq);
    torch::Tensor emb = torch::cat({freqs, freqs}, -1);
    torch::Tensor cos = emb.cos(), sin = emb.sin();

    auto rotate_half = [](const torch::Tensor &x) {
      int64_t half = x.size(-1) / 2;
      return torch::cat({-x.slice(-1, half), x.slice(-1, 0, half)}, -1);
    };

    Q = Q * cos.unsqueeze(1) + rotate_half(Q) * sin.unsqueeze(1);
    K = K * cos.unsqueeze(1) + rotate_half(K) * sin.unsqueeze(1);

    // Values
    torch::Tensor V = v_proj(hidden_states);
    V = V.view({-1, options.num_kv_heads, options.head_dim});

    // Sliding window mask
    std::optional<torch::Tensor> mask;
    if (options.attention_type == "sliding_attention") {
      int64_t S = hidden_states.size(0);
      torch::Tensor pos =
          torch::arange(S, hidden_states.options().dtype(torch::kLong));
      mask = (pos.unsqueeze(1) - pos.unsqueeze(0)).abs() <=
             options.sliding_window; // [S, S], broadcasts over heads
    }

    // SDPA (expects Q shape=[heads, seq, head_dim])
    double scaling = 1.0 / std::sqrt((double)options.head_dim);
    torch::Tensor QKV = torch::scaled_dot_product_attention(
        Q.transpose(0, 1), K.transpose(0, 1), V.transpose(0, 1), mask,
        /*dropout_p=*/0.0, /*is_causal=*/false, scaling,
        /*enable_gqa=*/options.num_heads != options.num_kv_heads);

    // Back to [seq, heads * head_dim]; transpose leaves it non-contiguous, so
    // reshape rather than view.
    QKV =
        QKV.transpose(0, 1).reshape({-1, options.num_heads * options.head_dim});

    // O
    torch::Tensor O = o_proj(QKV);

    logits = O;
    return logits;
  }
};

class Attention : public torch::nn::ModuleHolder<AttentionImpl> {
public:
  using torch::nn::ModuleHolder<AttentionImpl>::ModuleHolder;
  using Impl = AttentionImpl;
};

#endif
