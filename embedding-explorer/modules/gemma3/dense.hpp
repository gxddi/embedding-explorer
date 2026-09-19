#ifndef DENSE_H
#define DENSE_H

#include <fstream>
#include <string>

#include <torch/torch.h>

#include <nlohmann/json.hpp>

struct DenseOptions {

  // OPTIONS
  int64_t in_feat;  // = 768;
  int64_t out_feat; // = 768;
  bool bias;        // = false;

  /* Ideal constructor */
  DenseOptions(int64_t in_feat, int64_t out_feat, bool bias)
      : in_feat(in_feat), out_feat(out_feat), bias(bias) {}

  /* JSON config constructor */
  explicit DenseOptions(const std::string &config_path) {
    std::ifstream file(config_path);
    nlohmann::json config = nlohmann::json::parse(file);

    in_feat = config.value("in_features", in_feat);
    out_feat = config.value("out_features", out_feat);
    bias = config.value("bias", bias);
  }
};

// Both Dense layers in embeddinggemma-300m declare an Identity
// activation_function, so the config's activation is ignored and forward is a
// bare projection.
class DenseImpl : public torch::nn::Module {
public:
  DenseOptions options;
  torch::nn::Linear linear{nullptr};

  DenseImpl(int64_t in_feat, int64_t out_feat, bool bias)
      : DenseImpl(DenseOptions(in_feat, out_feat, bias)) {}
  explicit DenseImpl(const DenseOptions &options_) : options(options_) {
    linear = register_module(
        "linear", torch::nn::Linear(torch::nn::LinearOptions(options.in_feat,
                                                             options.out_feat)
                                        .bias(options.bias)));
  }

  torch::Tensor forward(torch::Tensor hidden_states) {

    torch::Tensor logits = linear(hidden_states);

    return logits;
  }
};

class Dense : public torch::nn::ModuleHolder<DenseImpl> {
public:
  using torch::nn::ModuleHolder<DenseImpl>::ModuleHolder;
  using Impl = DenseImpl;
};

#endif
