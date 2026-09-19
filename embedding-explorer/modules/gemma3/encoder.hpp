#ifndef ENCODER_H
#define ENCODER_H

#include "attention.hpp"
#include "mlp.hpp"
#include "rmsnorm.hpp"
#include <string>
#include <torch/torch.h>

// Options/Hyperparameters struct
struct EncoderLayerOptions {
  // OPTIONS
  std::string layer_type;
  int64_t sliding_window;
  int64_t h_size;
  int64_t interm_size;
  int64_t n_heads;
  int64_t n_kv_heads;
  int64_t head_dim;

  /* Ideal constructor */
  EncoderLayerOptions(std::string layer_type, int64_t sliding_window,
                      int64_t h_size, int64_t interm_size, int64_t n_heads,
                      int64_t n_kv_heads, int64_t head_dim)
      : layer_type(layer_type), sliding_window(sliding_window), h_size(h_size),
        interm_size(interm_size), n_heads(n_heads), n_kv_heads(n_kv_heads),
        head_dim(head_dim) {}
};

class EncoderLayerImpl : public torch::nn::Module {
public:
  // Hyperparameters
  EncoderLayerOptions options;

  // Parameters
  RMSNorm input_layernorm{nullptr}, post_attention_layernorm{nullptr},
      pre_feedforward_layernorm{nullptr}, post_feedforward_layernorm{nullptr};
  Attention self_attn{nullptr};
  MLP mlp{nullptr};

  /* Non option-struct constructor */
  EncoderLayerImpl(std::string layer_type, int64_t sliding_window,
                   int64_t h_size, int64_t interm_size, int64_t n_heads,
                   int64_t n_kv_heads, int64_t head_dim)
      : EncoderLayerImpl(EncoderLayerOptions(layer_type, sliding_window, h_size,
                                             interm_size, n_heads, n_kv_heads,
                                             head_dim)) {}

  /* Option-struct constructor */
  explicit EncoderLayerImpl(const EncoderLayerOptions &options_)
      : options(options_) {
    // Register all modules
    input_layernorm =
        register_module("input_layernorm", RMSNorm(options.h_size));

    self_attn = register_module(
        "self_attn",
        Attention(options.layer_type, options.sliding_window, options.h_size,
                  options.n_heads, options.n_kv_heads, options.head_dim));

    post_attention_layernorm =
        register_module("post_attention_layernorm", RMSNorm(options.h_size));

    pre_feedforward_layernorm =
        register_module("pre_feedforward_layernorm", RMSNorm(options.h_size));

    mlp = register_module("mlp", MLP(options.h_size, options.interm_size));

    post_feedforward_layernorm =
        register_module("post_feedforward_layernorm", RMSNorm(options.h_size));
  }

  torch::Tensor forward(torch::Tensor hidden_states) {
    torch::Tensor logits, residual, attn, fw;

    // Attention
    residual = hidden_states;
    attn = input_layernorm(hidden_states);
    attn = self_attn(attn);
    attn = post_attention_layernorm(attn);
    attn = attn + residual;

    // Feed forward
    residual = attn;
    fw = pre_feedforward_layernorm(attn);
    fw = mlp(fw);
    fw = post_feedforward_layernorm(fw);
    fw = fw + attn;

    // Output
    logits = fw;
    return logits;
  }
};

class EncoderLayer : public torch::nn::ModuleHolder<EncoderLayerImpl> {
public:
  using torch::nn::ModuleHolder<EncoderLayerImpl>::ModuleHolder;
  using Impl = EncoderLayerImpl;
};

#endif
