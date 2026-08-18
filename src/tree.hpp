#pragma once

#include "pack_cas.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct archive;

std::string to_hex(std::string_view raw);
std::string from_hex(std::string_view hex);

// 'r' regular, 'x' executable, 's' symlink (id = target blob), 'd' directory
struct TreeEntry {
  std::string name;
  char type = 0;
  std::string id;
};

class TreeStore {
public:
  // roots unused for ttl are evicted; the GC pass runs at most once per
  // gc_interval, triggered by ingest or an explicit maybeGc()
  explicit TreeStore(std::unique_ptr<PackCas> cas,
                     uint64_t ttl_ns = kDay * 30, uint64_t gc_interval_ns = kDay,
                     uint64_t touch_interval_ns = kDay / 24)
      : cas_(std::move(cas)), ttl_ns_(ttl_ns), gc_interval_ns_(gc_interval_ns),
        touch_interval_ns_(touch_interval_ns) {}

  static constexpr uint64_t kDay = 86'400'000'000'000;

  std::string putBlob(std::string_view data) { return cas_->put(data); }
  std::string putTree(const std::vector<TreeEntry> &entries);
  std::vector<TreeEntry> readTree(const std::string &id);
  std::string readBlob(const std::string &id);
  bool readBlobView(const std::string &id, std::string_view &out) {
    return cas_->get_view(id, out);
  }
  bool hasTree(const std::string &id);
  void sync() { cas_->sync(); }
  PackCas &cas() { return *cas_; }

  void registerRoot(const std::string &root);
  void touchRoot(const std::string &root);
  void maybeGc();

  struct RootInfo {
    std::string root;
    uint64_t last_access;
  };
  std::vector<RootInfo> roots_info();

private:
  void mark_live(const std::string &id,
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
IngestResult ingest_archive(TreeStore &store, struct archive *a);
IngestResult ingest_tarball_file(TreeStore &store, const std::string &path);

struct TreeWalker {
  TreeStore &store;
  std::unordered_map<std::string, std::vector<TreeEntry>> cache{};

  const std::vector<TreeEntry> &tree(const std::string &id);
  bool lookup(const std::string &root, std::string_view path, TreeEntry &out);
};

void nar_dump(TreeStore &store, const std::string &root,
              const std::function<void(std::string_view)> &sink);

// returns raw 32-byte sha256 of the NAR
std::string nar_sha256(TreeStore &store, const std::string &root,
                       uint64_t &nar_size);
