#ifndef NIX_TARMAC_TREE_HPP
#define NIX_TARMAC_TREE_HPP

#include "pack_cas.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct archive;

auto to_hex(std::string_view raw) -> std::string;
auto from_hex(std::string_view hex) -> std::string;

// 'r' regular, 'x' executable, 's' symlink (id = target blob), 'd' directory
struct TreeEntry {
  std::string name;
  char type = 0;
  std::string id;
};

class TreeStore {
public:
  using Duration = std::chrono::nanoseconds;
  static constexpr Duration kDefaultTtl = std::chrono::days(30);
  static constexpr Duration kDefaultGcInterval = std::chrono::days(1);
  static constexpr Duration kDefaultTouchInterval = std::chrono::hours(1);

  // roots unused for ttl are evicted; the GC pass runs at most once per
  // gc_interval, triggered by ingest or an explicit maybeGc()
  explicit TreeStore(std::unique_ptr<PackCas> cas, Duration ttl = kDefaultTtl,
                     Duration gc_interval = kDefaultGcInterval,
                     Duration touch_interval = kDefaultTouchInterval)
      : cas_(std::move(cas)), ttl_ns_(ttl.count()),
        gc_interval_ns_(gc_interval.count()),
        touch_interval_ns_(touch_interval.count()) {}

  auto putBlob(std::string_view data) -> std::string { return cas_->put(data); }
  auto putTree(const std::vector<TreeEntry> &entries) -> std::string;
  auto readTree(const std::string &tree_id) -> std::vector<TreeEntry>;
  auto readBlob(const std::string &blob_id) -> std::string;
  auto readBlobView(const std::string &blob_id, std::string &scratch,
                    std::string_view &out) -> bool {
    return cas_->get_view(blob_id, scratch, out);
  }
  auto blobSize(const std::string &blob_id, uint64_t &out) -> bool {
    return cas_->size(blob_id, out);
  }
  auto hasTree(const std::string &tree_id) -> bool;
  void sync() { cas_->sync(); }
  auto cas() -> PackCas & { return *cas_; }

  void registerRoot(const std::string &root);
  void touchRoot(const std::string &root);
  void maybeGc();

  struct RootInfo {
    std::string root;
    uint64_t last_access;
  };
  auto roots_info() -> std::vector<RootInfo>;

private:
  void mark_live(const std::string &root,
                 std::unordered_set<std::string> &live);

  std::unique_ptr<PackCas> cas_;
  uint64_t ttl_ns_;
  uint64_t gc_interval_ns_;
  uint64_t touch_interval_ns_;
};

struct IngestResult {
  std::string root;
  uint64_t files = 0;
};

// consumes and frees the archive; caller opened it (file, memory, stream)
auto ingest_archive(TreeStore &store, struct archive *arc) -> IngestResult;
auto ingest_tarball_file(TreeStore &store, const std::string &path)
    -> IngestResult;

class TreeWalker {
public:
  explicit TreeWalker(TreeStore &store) : store_(&store) {}

  auto tree(const std::string &tree_id) -> const std::vector<TreeEntry> &;
  auto lookup(const std::string &root, std::string_view path, TreeEntry &out)
      -> bool;

private:
  TreeStore *store_;
  std::unordered_map<std::string, std::vector<TreeEntry>> cache_;
};

void nar_dump(TreeStore &store, const std::string &root,
              const std::function<void(std::string_view)> &sink);

// returns raw 32-byte sha256 of the NAR
auto nar_sha256(TreeStore &store, const std::string &root, uint64_t &nar_size)
    -> std::string;

#endif
