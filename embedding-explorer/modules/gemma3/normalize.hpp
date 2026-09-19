#ifndef NORMALIZE_H
#define NORMALIZE_H

#include <string>

#include <torch/torch.h>

// Options/Hyperparameters struct
struct NormalizeOptions {

  // OPTIONS
  int64_t dim = -1;

  NormalizeOptions() = default;

  /* JSON config constructor */
  explicit NormalizeOptions(const std::string &config_path) {}
};

class NormalizeImpl : public torch::nn::Module {
public:
  NormalizeOptions options;

  NormalizeImpl() : NormalizeImpl(NormalizeOptions()) {}
  explicit NormalizeImpl(const NormalizeOptions &options_)
      : options(options_) {}

  torch::Tensor forward(torch::Tensor hidden_states) {

    torch::Tensor logits = torch::nn::functional::normalize(
        hidden_states,
        torch::nn::functional::NormalizeFuncOptions().p(2).dim(options.dim));

    return logits;
  }
};

class Normalize : public torch::nn::ModuleHolder<NormalizeImpl> {
public:
  using torch::nn::ModuleHolder<NormalizeImpl>::ModuleHolder;
  using Impl = NormalizeImpl;
};

#endif
