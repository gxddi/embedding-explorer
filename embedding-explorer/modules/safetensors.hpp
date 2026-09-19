#ifndef SAFETENSORS_H
#define SAFETENSORS_H

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include <torch/torch.h>

/* Matches a safetensors dtype string to its torch::ScalarType.
 *
 *  - dtype is the header's dtype string
 *  - scalar_type is written only on a match
 *
 * -> Returns false if the dtype has no torch equivalent here.
 */
inline bool safetensors_dtype(const std::string &dtype,
                              torch::ScalarType &scalar_type) {
  if (dtype == "F64") {
    scalar_type = torch::kFloat64;
  } else if (dtype == "F32") {
    scalar_type = torch::kFloat32;
  } else if (dtype == "F16") {
    scalar_type = torch::kFloat16;
  } else if (dtype == "BF16") {
    scalar_type = torch::kBFloat16;
  } else if (dtype == "I64") {
    scalar_type = torch::kInt64;
  } else if (dtype == "I32") {
    scalar_type = torch::kInt32;
  } else if (dtype == "I16") {
    scalar_type = torch::kInt16;
  } else if (dtype == "I8") {
    scalar_type = torch::kInt8;
  } else if (dtype == "U8") {
    scalar_type = torch::kUInt8;
  } else if (dtype == "BOOL") {
    scalar_type = torch::kBool;
  } else {
    return false;
  }

  return true;
}

/* Reads a .safetensors file into a name -> tensor map.
 *
 * The layout is a little endian uint64 header length, that many bytes of JSON
 * header mapping each tensor name to its dtype/shape/data_offsets, then one
 * contiguous block of raw tensor data. The offsets are relative to the start of
 * that block, not to the start of the file.
 *
 * NOTE: Each tensor is read straight into its own torch::empty buffer rather
 * than slurping the file first. The root model.safetensors is 1.2GB and a
 * staging copy would double that for no gain.
 *
 * NOTE: safetensors is little endian by spec and so is every target here, so
 * the raw bytes are copied in without any swapping.
 *
 *  - path points to the .safetensors file
 *
 * -> Returns an empty map if the file is missing or malformed at any point.
 */
inline std::unordered_map<std::string, torch::Tensor>
load_safetensor(const std::string &path) {

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    std::cerr << "src/modules/safetensors.h (load_safetensor): " << path
              << ": cannot open.\n";
    return {};
  }

  // File size, needed to sanity check the header length before allocating it
  file.seekg(0, std::ios::end);
  const std::streamoff file_size = file.tellg();
  file.seekg(0, std::ios::beg);

  // Header length
  uint64_t header_size = 0;
  file.read(reinterpret_cast<char *>(&header_size), sizeof(header_size));
  if (!file || static_cast<std::streamoff>(sizeof(header_size) + header_size) >
                   file_size) {
    std::cerr << "src/modules/safetensors.h (load_safetensor): " << path
              << ": header length does not fit the file, not a safetensors.\n";
    return {};
  }

  // Header
  std::vector<char> header_bytes(header_size);
  file.read(header_bytes.data(), header_size);
  if (!file) {
    std::cerr << "src/modules/safetensors.h (load_safetensor): " << path
              << ": header is short.\n";
    return {};
  }

  nlohmann::json header;
  try {
    header = nlohmann::json::parse(header_bytes.begin(), header_bytes.end());
  } catch (const nlohmann::json::exception &e) {
    std::cerr << "src/modules/safetensors.h (load_safetensor): " << path << ": "
              << e.what() << "\n";
    return {};
  }
  if (!header.is_object()) {
    std::cerr << "src/modules/safetensors.h (load_safetensor): " << path
              << ": header is a json but not a map of tensors.\n";
    return {};
  }

  // The data block starts right after the length prefix and the header
  const std::streamoff data_start = sizeof(header_size) + header_size;

  // Iterate over tensors
  std::unordered_map<std::string, torch::Tensor> tensors;
  for (const auto &entry : header.items()) {
    // Free form file metadata, not a tensor
    if (entry.key() == "__metadata__") {
      continue;
    }

    const nlohmann::json &info = entry.value();
    if (!info.is_object() || !info.contains("dtype") ||
        !info.contains("shape") || !info.contains("data_offsets")) {
      std::cerr << "src/modules/safetensors.h (load_safetensor): " << path
                << ": tensor '" << entry.key()
                << "' has no dtype, shape or data_offsets.\n";
      return {};
    }

    std::string dtype = info["dtype"];
    torch::ScalarType scalar_type;
    if (!safetensors_dtype(dtype, scalar_type)) {
      std::cerr << "src/modules/safetensors.h (load_safetensor): " << path
                << ": tensor '" << entry.key() << "' has unsupported dtype "
                << dtype << ".\n";
      return {};
    }

    std::vector<int64_t> shape = info["shape"];
    std::vector<int64_t> offsets = info["data_offsets"];
    if (offsets.size() != 2) {
      std::cerr << "src/modules/safetensors.h (load_safetensor): " << path
                << ": tensor '" << entry.key()
                << "' needs exactly two data_offsets.\n";
      return {};
    }

    // torch::empty is contiguous,
    // so the file's row major bytes land as is
    torch::Tensor tensor =
        torch::empty(shape, torch::TensorOptions().dtype(scalar_type));

    const int64_t byte_count = offsets[1] - offsets[0];
    if (byte_count != tensor.numel() * tensor.element_size() ||
        data_start + offsets[1] > file_size) {
      std::cerr << "src/modules/safetensors.h (load_safetensor): " << path
                << ": tensor '" << entry.key()
                << "' offsets do not match its dtype and shape.\n";
      return {};
    }

    file.seekg(data_start + offsets[0], std::ios::beg);
    file.read(reinterpret_cast<char *>(tensor.data_ptr()), byte_count);
    if (!file) {
      std::cerr << "src/modules/safetensors.h (load_safetensor): " << path
                << ": tensor '" << entry.key() << "' data is short.\n";
      return {};
    }

    tensors.emplace(entry.key(), tensor);
  }

  return tensors;
}

#endif
