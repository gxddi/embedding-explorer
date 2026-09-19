#ifndef RMSNORM_H
#define RMSNORM_H

#include <torch/torch.h>

struct RMSNormOptions {
  RMSNormOptions(int64_t dim, double eps = 1e-6) : dim(dim), eps(eps) {}

  int64_t dim;
  double eps;
};

class RMSNormImpl : public torch::nn::Module {
public:
  RMSNormOptions options;
  torch::Tensor weight;

  RMSNormImpl(int64_t dim) : RMSNormImpl(RMSNormOptions(dim)) {}
  explicit RMSNormImpl(const RMSNormOptions &options_) : options(options_) {
    weight = register_parameter("weight", torch::zeros({options.dim}));
  }

  torch::Tensor forward(torch::Tensor input) {
    return input * torch::rsqrt(input.pow(2).mean(-1, true) + options.eps) *
           (1.0 + weight);
  }
};

// Hand-written equivalent of what `TORCH_MODULE(RMSNorm)` would generate: a
// `shared_ptr<RMSNormImpl>` wrapper with constructors that forward straight
// to `RMSNormImpl`.
class RMSNorm : public torch::nn::ModuleHolder<RMSNormImpl> {
public:
  using torch::nn::ModuleHolder<RMSNormImpl>::ModuleHolder;
  using Impl = RMSNormImpl;
};

#endif
