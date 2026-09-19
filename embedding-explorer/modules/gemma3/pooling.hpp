#ifndef POOLING_H
#define POOLING_H

#include <fstream>
#include <iostream>
#include <string>

#include <torch/torch.h>

#include <nlohmann/json.hpp>

// Options/Hyperparameters struct
struct PoolingOptions {

  // OPTIONS
  int embedding_dimension;  // = 768;
  std::string pooling_mode; // = "mean";
  bool include_prompt;      // = true;

  /* Ideal constructor */
  PoolingOptions(int embedding_dimension, std::string pooling_mode,
                 bool include_prompt)
      : embedding_dimension(embedding_dimension), pooling_mode(pooling_mode),
        include_prompt(include_prompt) {}

  /* JSON config constructor */
  explicit PoolingOptions(const std::string &config_path) {
    std::ifstream file(config_path);
    nlohmann::json config = nlohmann::json::parse(file);

    embedding_dimension =
        config.value("word_embedding_dimension", embedding_dimension);
    include_prompt = config.value("include_prompt", include_prompt);

    // The config's mode is a set of mutually exclusive flags.
    if (config.value("pooling_mode_cls_token", false)) {
      pooling_mode = "cls";
    } else if (config.value("pooling_mode_max_tokens", false)) {
      pooling_mode = "max";
    } else if (config.value("pooling_mode_mean_sqrt_len_tokens", false)) {
      pooling_mode = "mean_sqrt_len";
    } else if (config.value("pooling_mode_weightedmean_tokens", false)) {
      pooling_mode = "weightedmean";
    } else if (config.value("pooling_mode_lasttoken", false)) {
      pooling_mode = "lasttoken";
    } else {
      pooling_mode = "mean";
    }
  }
};

class PoolingImpl : public torch::nn::Module {
public:
  PoolingOptions options;

  PoolingImpl(int emb_dim, std::string pool_mode, bool incl_prompt)
      : PoolingImpl(PoolingOptions(emb_dim, pool_mode, incl_prompt)) {}
  explicit PoolingImpl(const PoolingOptions &options_) : options(options_) {
    if (options.pooling_mode != "mean") {
      std::cerr << "src/modules/gemma3/pooling.h (PoolingImpl): pooling mode '"
                << options.pooling_mode
                << "' is not implemented, falling back to mean.\n";
    }
  }

  /* Takes the unbatched [seq, hidden] the rest of the stack passes around
   * (transformer.h, dense.h, embed.cpp) and reduces it to a single [hidden].
   *
   * NOTE: The padded/masked variant sentence-transformers uses is dropped
   * rather than ported. Padding only exists to square off a batch, and nothing
   * here batches, embed.cpp embeds one readme at a time. One sequence means
   * every token is real, so the mask would be all ones and the weighted mean
   * collapses to a plain mean over the sequence.
   */
  torch::Tensor forward(torch::Tensor tok_emb) {

    torch::Tensor logits = tok_emb.mean(0);

    return logits;
  }
};

class Pooling : public torch::nn::ModuleHolder<PoolingImpl> {
public:
  using torch::nn::ModuleHolder<PoolingImpl>::ModuleHolder;
  using Impl = PoolingImpl;
};

#endif
