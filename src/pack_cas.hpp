#ifndef NIX_TARMAC_PACK_CAS_HPP
#define NIX_TARMAC_PACK_CAS_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

auto blake3_hash(std::string_view data) -> std::string;

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
  static auto open(const std::string &dir) -> std::unique_ptr<PackCas>;
  ~PackCas();

  PackCas(const PackCas &) = delete;
  PackCas(PackCas &&) = delete;
  auto operator=(const PackCas &) -> PackCas & = delete;
  auto operator=(PackCas &&) -> PackCas & = delete;

  [[nodiscard]] auto put(std::string_view data) -> std::string;
  [[nodiscard]] auto get(std::string_view hash, std::string &out) -> bool;
  // out points into the mmap (raw blobs) or into scratch (compressed)
  [[nodiscard]] auto get_view(std::string_view hash, std::string &scratch,
                              std::string_view &out) -> bool;
  [[nodiscard]] auto size(std::string_view hash, uint64_t &out) -> bool;
  [[nodiscard]] auto has(std::string_view hash) -> bool;
  void sync();

  auto pack_size() -> uint64_t;
  void compact(const std::function<bool(std::string_view)> &live);

  // separate dbi; writes join an open batch
  void meta_put(std::string_view key, std::string_view val);
  [[nodiscard]] auto meta_get(std::string_view key, std::string &out) -> bool;
  void meta_del(std::string_view key);
  void meta_scan(
      std::string_view prefix,
      const std::function<bool(std::string_view, std::string_view)> &callback);

private:
  static auto open_once(const std::string &dir) -> std::unique_ptr<PackCas>;
  struct Impl;
  explicit PackCas(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

#endif
