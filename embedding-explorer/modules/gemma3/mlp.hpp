#ifndef MLP_H
#define MLP_H

#include <torch/torch.h>

struct MLPOptions {
  MLPOptions(int64_t h_size, int64_t interm_size)
      : h_size(h_size), interm_size(interm_size) {}

  int64_t h_size;
  int64_t interm_size;
};

class MLPImpl : public torch::nn::Module {
public:
  MLPOptions options;
  torch::nn::Linear gate_proj{nullptr}, up_proj{nullptr}, down_proj{nullptr};

  MLPImpl(int64_t h_size, int64_t interm_size)
      : MLPImpl(MLPOptions(h_size, interm_size)) {}
  explicit MLPImpl(const MLPOptions &options_) : options(options_) {
    gate_proj = register_module(
        "gate_proj", torch::nn::Linear(torch::nn::LinearOptions(
                                           options.h_size, options.interm_size)
                                           .bias(false)));
    up_proj = register_module(
        "up_proj", torch::nn::Linear(torch::nn::LinearOptions(
                                         options.h_size, options.interm_size)
                                         .bias(false)));
    down_proj = register_module(
        "down_proj", torch::nn::Linear(torch::nn::LinearOptions(
                                           options.interm_size, options.h_size)
                                           .bias(false)));
  }

  torch::Tensor forward(torch::Tensor hidden_states) {

    torch::Tensor logits;

    torch::Tensor gate = torch::gelu(gate_proj(hidden_states), "tanh");
    torch::Tensor up = up_proj(hidden_states);
    torch::Tensor down = down_proj(gate * up);

    logits = down;
    return logits;
  }
};

class MLP : public torch::nn::ModuleHolder<MLPImpl> {
public:
  using torch::nn::ModuleHolder<MLPImpl>::ModuleHolder;
  using Impl = MLPImpl;
};

#endif
