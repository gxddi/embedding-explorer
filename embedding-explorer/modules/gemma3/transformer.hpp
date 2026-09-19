#ifndef TRANSFORMER_H
#define TRANSFORMER_H

#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <torch/torch.h>

#include "encoder.hpp"
#include "rmsnorm.hpp"

// Options/Hyperparameters struct
struct TransformerOptions {

  // OPTIONS
  // Embedding
  int64_t vocab_size;  // = 262144;
  int64_t hidden_size; // = 768;
  // Encoders - Self Attention
  int64_t num_layers;              // = 24;
  std::vector<std::string> layers; /* = {
      "sliding_attention", "sliding_attention", "sliding_attention",
      "sliding_attention", "sliding_attention", "full_attention",
      "sliding_attention", "sliding_attention", "sliding_attention",
      "sliding_attention", "sliding_attention", "full_attention",
      "sliding_attention", "sliding_attention", "sliding_attention",
      "sliding_attention", "sliding_attention", "full_attention",
      "sliding_attention", "sliding_attention", "sliding_attention",
      "sliding_attention", "sliding_attention", "full_attention"}; */
  int64_t sliding_window;          // = 512;
  int64_t num_heads;               // = 3;
  int64_t num_kv_heads;            // = 1;
  int64_t head_dim;                // = 256;
  // Encoders - MLP
  int64_t intermediate_size; // = 1152;

  /* Default constructor */
  TransformerOptions() = default;

  /* JSON config constructor */
  TransformerOptions(const std::string &config_path) {
    std::ifstream file(config_path);
    nlohmann::json config = nlohmann::json::parse(file);

    vocab_size = config["vocab_size"]; // vocab_size);
    hidden_size = config["hidden_size"];

    num_layers = config["num_hidden_layers"];
    layers = config["layer_types"];
    sliding_window = config["sliding_window"];
    num_heads = config["num_attention_heads"];
    num_kv_heads = config["num_key_value_heads"];
    head_dim = config["head_dim"];

    intermediate_size = config["intermediate_size"];
  }
};

class TransformerImpl : public torch::nn::Module {
public:
  TransformerOptions options;
  torch::nn::Embedding embed_tokens{nullptr};
  torch::nn::Sequential layers;
  RMSNorm norm{nullptr};

  TransformerImpl() : TransformerImpl(TransformerOptions()) {}

  explicit TransformerImpl(const TransformerOptions &options_)
      : options(options_) {
    embed_tokens = register_module(
        "embed_tokens",
        torch::nn::Embedding(options.vocab_size, options.hidden_size));

    for (int64_t i = 0; i < options.num_layers; ++i) {
      layers->push_back(EncoderLayer(
          options.layers[i], options.sliding_window, options.hidden_size,
          options.intermediate_size, options.num_heads, options.num_kv_heads,
          options.head_dim));
    }
    register_module("layers", layers);

    norm = register_module("norm", RMSNorm(options.hidden_size));
  }

  torch::Tensor forward(torch::Tensor input_ids) {

    // Embed token IDs + scale
    torch::Tensor logits =
        embed_tokens(input_ids) * std::sqrt((double)options.hidden_size);

    // Go through transformer layers
    logits = layers->forward(logits);

    // Final norm
    logits = norm(logits);

    return logits;
  }
};

class Transformer : public torch::nn::ModuleHolder<TransformerImpl> {
public:
  using torch::nn::ModuleHolder<TransformerImpl>::ModuleHolder;
  using Impl = TransformerImpl;
};

#endif
