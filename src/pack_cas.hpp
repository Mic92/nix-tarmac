#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

std::string blake3_hash(std::string_view data);

struct CorruptError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// LMDB index (BLAKE3 -> offset/length) plus an append-only packfile read
// through a shared mmap: lock-free reads from any process, writes batch
// under an flock. Blobs are LZ4-compressed when that shrinks them; hashes
// are over the raw content. compact() rewrites the pack as a new generation;
// readers remap, old mappings stay mapped until close.
class PackCas {
public:
  static std::unique_ptr<PackCas> open(const std::string &dir);
  ~PackCas();

  PackCas(const PackCas &) = delete;
  PackCas &operator=(const PackCas &) = delete;

  [[nodiscard]] std::string put(std::string_view data);
  [[nodiscard]] bool get(std::string_view hash, std::string &out);
  // out points into the mmap (raw blobs) or into scratch (compressed)
  [[nodiscard]] bool get_view(std::string_view hash, std::string &scratch,
                              std::string_view &out);
  [[nodiscard]] bool size(std::string_view hash, uint64_t &out);
  [[nodiscard]] bool has(std::string_view hash);
  void sync();

  uint64_t pack_size();
  void compact(const std::function<bool(std::string_view)> &live);

  // separate dbi; writes join an open batch
  void meta_put(std::string_view key, std::string_view val);
  [[nodiscard]] bool meta_get(std::string_view key, std::string &out);
  void meta_del(std::string_view key);
  void meta_scan(std::string_view prefix,
                 const std::function<bool(std::string_view, std::string_view)>
                     &cb);

private:
  static std::unique_ptr<PackCas> open_once(const std::string &dir);
  struct Impl;
  explicit PackCas(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
