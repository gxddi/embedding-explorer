#ifndef GEMMA_TOKENIZER_H
#define GEMMA_TOKENIZER_H

#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include <torch/torch.h>

// Re-implements the "GemmaTokenizer" described by tokenizer.json /
// tokenizer_config.json next to a model's weights (e.g.
// models/embeddinggemma-300m/). Scope is deliberately narrow: encode()
// only (no decode), and every call is treated as a batch of one, so
// there's no padding/attention_mask to produce.
//
// The pipeline below was confirmed against the real tokenizer's actual
// output (not just tokenizer.json's declared steps):
//   1. Normalize: every literal space byte becomes the 3-byte UTF-8
//      encoding of U+2581 ("▁").
//   2. Chop the normalized string into one initial BPE symbol per Unicode
//      codepoint.
//   3. Greedily merge adjacent symbol pairs using tokenizer.json's ranked
//      merge list until no ranked pair remains.
//   4. Map surviving symbols to ids via the vocab; a symbol not in the
//      vocab (byte_fallback) is decomposed into raw UTF-8 bytes and
//      mapped through the "<0xXX>" byte tokens instead.
//   5. Wrap with <bos> ... <eos>.
//
// Step 3 is the hot path. Symbols are always contiguous ranges of the
// normalized text, so they are held as (offset, length) into one buffer and
// merging never allocates or copies a string. Candidate merges live in a
// min-heap keyed by (rank, offset) instead of being rescanned each round,
// which takes the whole step from O(n^2) to O(n log n).
//
// This is the tokenizer for Gemma as a family, not for Gemma 3 specifically,
// so it lives beside gemma3/ rather than inside it. Only the vocab it reads
// differs between generations.
class GemmaTokenizer {
public:
  /* - model_dir points to the model root, trailing slash included, matching
   *   load_modules
   * - device is where encode() allocates the id tensor
   */
  GemmaTokenizer(const std::string &model_dir, torch::Device device)
      : device_(device) {
    nlohmann::json tokenizer_json = load_json(model_dir + "tokenizer.json");
    nlohmann::json config_json = load_json(model_dir + "tokenizer_config.json");

    const auto &vocab = tokenizer_json["model"]["vocab"];
    vocab_.reserve(vocab.size() * 2);
    for (auto &[token, id] : vocab.items()) {
      vocab_.emplace(intern(token), id.get<int64_t>());
    }

    const auto &merges = tokenizer_json["model"]["merges"];
    merge_rank_.reserve(merges.size() * 2);
    for (size_t rank = 0; rank < merges.size(); ++rank) {
      std::string_view lhs = intern(merges[rank][0].get<std::string>());
      std::string_view rhs = intern(merges[rank][1].get<std::string>());
      merge_rank_.emplace(SymbolPair{lhs, rhs}, rank);
    }

    bos_id_ = lookup(config_json.value("bos_token", std::string("<bos>")));
    eos_id_ = lookup(config_json.value("eos_token", std::string("<eos>")));
    unk_id_ = lookup(config_json.value("unk_token", std::string("<unk>")));
    add_bos_ = config_json.value("add_bos_token", true);
    add_eos_ = config_json.value("add_eos_token", true);

    // Byte-fallback ids are fixed, so resolve all 256 up front.
    for (int b = 0; b < 256; ++b) {
      char name[8];
      std::snprintf(name, sizeof(name), "<0x%02X>", b);
      auto it = vocab_.find(std::string_view(name));
      byte_id_[b] = it != vocab_.end() ? it->second : unk_id_;
    }
  }

  /* Encodes a single string into a [1, seq_len] int64 tensor of token ids on
   * the device this tokenizer was constructed with.
   */
  torch::Tensor encode(const std::string &text) const {
    const std::string norm = normalize(text);

    std::vector<Symbol> symbols;
    split_codepoints(norm, symbols);
    apply_merges(norm, symbols);

    std::vector<int64_t> ids;
    ids.reserve(symbols.size() + 2);
    if (add_bos_) {
      ids.push_back(bos_id_);
    }
    for (int32_t i = symbols.empty() ? -1 : 0; i != -1; i = symbols[i].next) {
      append_ids(view(norm, symbols[i]), ids);
    }
    if (add_eos_) {
      ids.push_back(eos_id_);
    }

    // torch::tensor copies out of ids, unlike from_blob which would alias a
    // vector that dies at the end of this call.
    return torch::tensor(
               ids, torch::TensorOptions().dtype(torch::kInt64).device(device_))
        .unsqueeze(0);
  }

private:
  // A symbol is a slice of the normalized text plus its neighbours, so a merge
  // is just "grow left, unlink right".
  struct Symbol {
    int32_t start;
    int32_t len;
    int32_t prev;
    int32_t next;
  };

  struct SymbolPair {
    std::string_view lhs, rhs;
  };

  struct PairHash {
    size_t operator()(const SymbolPair &p) const {
      size_t h = std::hash<std::string_view>()(p.lhs);
      return h ^ (std::hash<std::string_view>()(p.rhs) + 0x9e3779b97f4a7c15ULL +
                  (h << 6) + (h >> 2));
    }
  };

  struct PairEq {
    bool operator()(const SymbolPair &a, const SymbolPair &b) const {
      return a.lhs == b.lhs && a.rhs == b.rhs;
    }
  };

  // Pending merge. The lengths pin the exact pair this rank was looked up for;
  // if either side has since grown, the rank no longer applies and the entry
  // must be discarded.
  struct Candidate {
    size_t rank;
    int32_t start;
    int32_t left;
    int32_t right;
    int32_t left_len;
    int32_t right_len;

    /* Lowest rank wins; ties go to the leftmost pair, matching a scan that
     * keeps the first strictly-better pair it finds.
     */
    bool operator>(const Candidate &o) const {
      return rank != o.rank ? rank > o.rank : start > o.start;
    }
  };

  torch::Device device_;
  std::deque<std::string> arena_; // owns the bytes the views below point at
  std::unordered_map<std::string_view, int64_t> vocab_;
  std::unordered_map<SymbolPair, size_t, PairHash, PairEq> merge_rank_;
  int64_t byte_id_[256];
  int64_t bos_id_, eos_id_, unk_id_;
  bool add_bos_, add_eos_;

  std::string_view intern(const std::string &s) {
    arena_.push_back(s); // deque keeps earlier elements pinned
    return arena_.back();
  }

  int64_t lookup(const std::string &token) const {
    return vocab_.at(std::string_view(token));
  }

  static nlohmann::json load_json(const std::string &path) {
    std::ifstream file(path);
    nlohmann::json parsed;
    file >> parsed;
    return parsed;
  }

  static std::string_view view(const std::string &buf, const Symbol &s) {
    return std::string_view(buf.data() + s.start, s.len);
  }

  /* Replaces every literal space with the UTF-8 bytes for "▁" (U+2581). */
  static std::string normalize(const std::string &text) {
    std::string out;
    out.reserve(text.size() + text.size() / 4);
    for (char c : text) {
      if (c == ' ') {
        out.append("\xE2\x96\x81", 3);
      } else {
        out.push_back(c);
      }
    }
    return out;
  }

  /* One symbol per codepoint, linked in order. */
  static void split_codepoints(const std::string &text,
                               std::vector<Symbol> &out) {
    out.clear();
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
      unsigned char c = text[i];
      size_t len = 1;
      if ((c & 0xE0) == 0xC0) {
        len = 2;
      } else if ((c & 0xF0) == 0xE0) {
        len = 3;
      } else if ((c & 0xF8) == 0xF0) {
        len = 4;
      }
      if (i + len > text.size()) {
        len = text.size() - i; // truncated trailing sequence
      }
      int32_t idx = (int32_t)out.size();
      out.push_back({(int32_t)i, (int32_t)len, idx - 1, idx + 1});
      i += len;
    }
    if (!out.empty()) {
      out.back().next = -1;
    }
  }

  void apply_merges(const std::string &buf, std::vector<Symbol> &sym) const {
    if (sym.size() < 2) {
      return;
    }

    std::priority_queue<Candidate, std::vector<Candidate>,
                        std::greater<Candidate>>
        queue;

    auto push = [&](int32_t left) {
      int32_t right = sym[left].next;
      if (right == -1) {
        return;
      }
      auto it = merge_rank_.find({view(buf, sym[left]), view(buf, sym[right])});
      if (it != merge_rank_.end()) {
        queue.push({it->second, sym[left].start, left, right, sym[left].len,
                    sym[right].len});
      }
    };

    for (int32_t i = 0; i + 1 < (int32_t)sym.size(); ++i) {
      push(i);
    }

    while (!queue.empty()) {
      Candidate c = queue.top();
      queue.pop();

      // Stale if either side has been merged into or unlinked since queuing.
      Symbol &left = sym[c.left];
      if (left.len != c.left_len || left.next != c.right ||
          sym[c.right].len != c.right_len) {
        continue;
      }

      int32_t right = c.right;
      left.len += sym[right].len;
      left.next = sym[right].next;
      if (left.next != -1) {
        sym[left.next].prev = c.left;
      }
      sym[right].len = 0; // unlinked

      push(c.left);
      if (left.prev != -1) {
        push(left.prev);
      }
    }
  }

  /* Maps a merged symbol to ids, falling back to "<0xXX>" byte tokens when the
   * symbol itself isn't in the vocab.
   */
  void append_ids(std::string_view symbol, std::vector<int64_t> &ids) const {
    auto it = vocab_.find(symbol);
    if (it != vocab_.end()) {
      ids.push_back(it->second);
      return;
    }
    for (unsigned char byte : symbol) {
      ids.push_back(byte_id_[byte]);
    }
  }
};

#endif
