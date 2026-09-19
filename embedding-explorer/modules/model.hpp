#ifndef MODEL_H
#define MODEL_H

#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include <torch/torch.h>

#include "gemma3/dense.hpp"
#include "gemma3/normalize.hpp"
#include "gemma3/pooling.hpp"
#include "gemma3/transformer.hpp"
#include "safetensors.hpp"

/* Mathes the module type to the corresponding custom torch::nn::Module. It
 * constructs it by using it's options struct, which is in turn constructed with
 * it's config constructor, and appends it to model.
 *
 * NOTE: The module is pushed from inside each branch rather than returned.
 * Because every ModuleHolder<T> is a distinct type sharing no polymorphic base.
 * We use ModuleHolder because it's a neat internal and thin torch::nn::Module
 * wrapper
 *
 *  - model is the Sequential the module gets appended to
 *  - module_type used to identify module
 *  - module_root_path points to the model root
 *  - module_config_path points to the modules config relative to model root
 *
 * -> Returns false if the module type is unknown or config parsing fails at any
 *    point.
 */
inline bool build_append_module(torch::nn::Sequential &model,
                                std::string module_type,
                                std::string model_root_path,
                                std::string module_config_path) {

  std::string full_module_path(model_root_path);
  full_module_path.append(module_config_path).append("/config.json");

  // Catch JSON exceptions
  try {
    // Match module type
    if (module_type == "sentence_transformers.models.Transformer") {
      model->push_back(Transformer(TransformerOptions(full_module_path)));
    } else if (module_type == "sentence_transformers.models.Pooling") {
      model->push_back(Pooling(PoolingOptions(full_module_path)));
    } else if (module_type == "sentence_transformers.models.Dense") {
      model->push_back(Dense(DenseOptions(full_module_path)));
    } else if (module_type == "sentence_transformers.models.Normalize") {
      model->push_back(Normalize(NormalizeOptions(full_module_path)));
    } else {
      std::cerr << "src/modules/model.h (build_append_module): Unknown module "
                   "type "
                << module_type << ".\n";
      return false;
    }
  } catch (const nlohmann::json::exception &e) {
    std::cerr << "src/modules/model.h (build_append_module): "
              << full_module_path << ": " << e.what() << "\n";
    return false;
  }

  return true;
}

/* Builds every module listed in the model's modules.json, in order, then fills
 * their parameters from the safetensors sitting next to each module's config.
 *
 *  - model_path points to the model root, trailing slash included
 *  - device is where the built model ends up
 *
 * -> Returns an empty Sequential if modules.json, any module config or any
 *    weight file is missing or malformed at any point.
 */
inline torch::nn::Sequential load_modules(const char *model_path,
                                          torch::Device device) {

  std::string modules_path(model_path);
  modules_path.append("modules.json");

  // Parse raw
  nlohmann::json raw;
  try {
    std::ifstream file(modules_path);
    raw = nlohmann::json::parse(file);
  } catch (const nlohmann::json::exception &e) {
    std::cerr << "src/modules/model.h (load_modules): " << modules_path << ": "
              << e.what() << "\n";
    return nullptr;
  }
  if (!raw.is_array()) {
    std::cerr << "src/modules/model.h (load_modules): " << modules_path
              << " is a json but not a list of modules.\n";
    return nullptr;
  }

  // Build and append modules to model over iterations. The paths are kept
  // because the weight pass below needs them again to find each module's
  // safetensors.
  torch::nn::Sequential model;
  std::vector<std::string> module_paths;
  for (size_t i = 0; i < raw.size(); i++) {
    const nlohmann::json &item = raw[i];
    if (!item.is_object() || !item.contains("path") || !item.contains("type") ||
        !item["path"].is_string() || !item["type"].is_string()) {
      std::cerr << "src/modules/model.h (load_modules): module " << i << " of "
                << modules_path << " has no path or type.\n";
      return nullptr;
    }

    if (!build_append_module(model, item["type"], model_path, item["path"])) {
      return nullptr;
    }
    module_paths.push_back(item["path"]);
  }

  // Load each module's parameters from the safetensors sitting next to its
  // config. Pooling and Normalize hold none, so they have no file to read and
  // 4_Normalize does not even exist on disk.
  //
  // Every name lines up as is: the transformer registers embed_tokens.weight /
  // layers.N.* / norm.weight and Dense registers linear.weight, which is
  // exactly how sentence-transformers wrote them out.
  torch::NoGradGuard no_grad;
  for (size_t i = 0; i < model->size(); i++) {
    std::shared_ptr<torch::nn::Module> module = model->ptr(i);
    if (module->parameters().empty()) {
      continue;
    }

    std::string weights_path(model_path);
    weights_path.append(module_paths[i]).append("/model.safetensors");

    std::unordered_map<std::string, torch::Tensor> tensors =
        load_safetensor(weights_path);
    if (tensors.empty()) {
      return nullptr;
    }

    for (auto &param : module->named_parameters()) {
      auto tensor = tensors.find(param.key());
      if (tensor == tensors.end()) {
        std::cerr << "src/modules/model.h (load_modules): " << weights_path
                  << " has no tensor '" << param.key() << "'.\n";
        return nullptr;
      }
      if (tensor->second.sizes() != param.value().sizes()) {
        std::cerr << "src/modules/model.h (load_modules): " << weights_path
                  << ": tensor '" << param.key() << "' is "
                  << tensor->second.sizes() << " but the module wants "
                  << param.value().sizes() << ".\n";
        return nullptr;
      }
      param.value().copy_(tensor->second);
    }
  }

  // Weights land on the CPU because that is where load_safetensor reads them,
  // so the move happens once here rather than per tensor.
  model->to(device);

  return model;
}

#endif
