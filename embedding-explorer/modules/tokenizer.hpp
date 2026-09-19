#ifndef TOKENIZER_H
#define TOKENIZER_H

#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>
#include <torch/torch.h>

#include "gemma3/gemma_tokenizer.hpp"

/* A loaded tokenizer, called as tokenizer(text) to get a [1, seq_len] int64
 * tensor of ids. Empty when loading failed, so it tests like a pointer.
 *
 * NOTE: This is a std::function rather than an abstract base with a virtual
 * encode(). load_modules gets to lean on torch::nn::Module as a shared base
 * that torch already provides, but tokenizers have no such base, and a hand
 * rolled one would have to sit in a third header to stop tokenizer.h and
 * gemma/gemma_tokenizer.h from including each other. Erasing to the call
 * signature is the whole interface anyway, encode() is all a caller wants.
 */
using Tokenizer = std::function<torch::Tensor(const std::string &)>;

/* Matches tokenizer_config.json's tokenizer_class to the corresponding
 * implementation and constructs it, keeping it alive inside the returned
 * callable.
 *
 *  - model_path points to the model root, trailing slash included
 *  - device is where the returned id tensors are allocated
 *
 * -> Returns an empty Tokenizer if the class is unknown, or if the config is
 *    missing or malformed at any point.
 */
inline Tokenizer load_tokenizer(const char *model_path, torch::Device device) {

  std::string config_path(model_path);
  config_path.append("tokenizer_config.json");

  // Parse raw
  nlohmann::json config;
  try {
    std::ifstream file(config_path);
    config = nlohmann::json::parse(file);
  } catch (const nlohmann::json::exception &e) {
    std::cerr << "src/modules/tokenizer.h (load_tokenizer): " << config_path
              << ": " << e.what() << "\n";
    return nullptr;
  }
  if (!config.is_object() || !config.contains("tokenizer_class") ||
      !config["tokenizer_class"].is_string()) {
    std::cerr << "src/modules/tokenizer.h (load_tokenizer): " << config_path
              << " has no tokenizer_class.\n";
    return nullptr;
  }
  std::string tokenizer_class = config["tokenizer_class"];

  // Catch construction failures. Not only json ones, the vocab lookups for
  // <bos>/<eos>/<unk> throw std::out_of_range when a token is missing.
  try {
    // Match tokenizer class. The tokenizer is held by shared_ptr so the
    // returned callable stays copyable.
    if (tokenizer_class == "GemmaTokenizer") {
      std::shared_ptr<GemmaTokenizer> tokenizer =
          std::make_shared<GemmaTokenizer>(model_path, device);
      return [tokenizer](const std::string &text) {
        return tokenizer->encode(text);
      };
    }
  } catch (const std::exception &e) {
    std::cerr << "src/modules/tokenizer.h (load_tokenizer): " << model_path
              << ": " << e.what() << "\n";
    return nullptr;
  }

  std::cerr << "src/modules/tokenizer.h (load_tokenizer): Unknown tokenizer "
               "class "
            << tokenizer_class << ".\n";
  return nullptr;
}

#endif
