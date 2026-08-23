// SourceAccessor over a TreeStore root. Shared by the fetcher and the
// eval store.
#ifndef NIX_TARMAC_PACK_ACCESSOR_HH
#define NIX_TARMAC_PACK_ACCESSOR_HH

#include "pack_cas.hpp"
#include "tree.hpp"

#include <nix/util/canon-path.hh>
#include <nix/util/error.hh>
#include <nix/util/source-accessor.hh>

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

inline auto tarmac_entry_type(char type) -> nix::SourceAccessor::Type {
  switch (type) {
  case 'd':
    return nix::SourceAccessor::tDirectory;
  case 's':
    return nix::SourceAccessor::tSymlink;
  default:
    return nix::SourceAccessor::tRegular;
  }
}

class PackAccessor : public nix::SourceAccessor {
public:
  using CorruptHook = std::function<void()>;

  PackAccessor(TreeStore &store, std::string root, CorruptHook on_corrupt = {})
      : store_(&store), root_(std::move(root)), walker_(store),
        on_corrupt_(std::move(on_corrupt)) {
    fingerprint = "tarmac:" + to_hex(root_);
  }

#if NIX_COMPAT_VERSION_MAJOR > 2 ||                                            \
    (NIX_COMPAT_VERSION_MAJOR == 2 && NIX_COMPAT_VERSION_MINOR >= 35)
  void anchor() override {}
#endif

  void readFile(const nix::CanonPath &path, nix::Sink &sink,
                nix::fun<void(uint64_t)> sizeCallback) override {
    const auto entry = find(path);
    if (!entry || entry->type == 'd' || entry->type == 's') {
      throw nix::Error("path '%s' is not a regular file", path);
    }
    std::string scratch;
    std::string_view contents;
    if (!healing([&]() -> bool {
          return store_->readBlobView(entry->id, scratch, contents);
        })) {
      throw nix::Error("missing blob for '%s'", path);
    }
    sizeCallback(contents.size());
    sink(contents);
  }

  auto pathExists(const nix::CanonPath &path) -> bool override {
    return find(path).has_value();
  }

  auto maybeLstat(const nix::CanonPath &path) -> std::optional<Stat> override {
    const auto entry = find(path);
    if (!entry) {
      return std::nullopt;
    }
    Stat info;
    info.type = tarmac_entry_type(entry->type);
    if (info.type == tRegular) {
      info.isExecutable = entry->type == 'x';
      uint64_t size = 0;
      if (store_->blobSize(entry->id, size)) {
        info.fileSize = size;
      }
    }
    return info;
  }

  auto readDirectory(const nix::CanonPath &path) -> DirEntries override {
    const auto entry = find(path);
    if (!entry || entry->type != 'd') {
      throw nix::Error("path '%s' is not a directory", path);
    }
    DirEntries res;
    for (const auto &child : healing([&]() -> std::vector<TreeEntry> {
           return store_->readTree(entry->id);
         })) {
      res.emplace(child.name, tarmac_entry_type(child.type));
    }
    return res;
  }

  auto readLink(const nix::CanonPath &path) -> std::string override {
    const auto entry = find(path);
    if (!entry || entry->type != 's') {
      throw nix::Error("path '%s' is not a symlink", path);
    }
    return healing(
        [&]() -> std::string { return store_->readBlob(entry->id); });
  }

private:
  template <typename Func> auto healing(Func func) -> decltype(func()) {
    try {
      return func();
    } catch (const CorruptError &err) {
      if (on_corrupt_) {
        on_corrupt_();
      }
      throw nix::Error("tarmac cache is corrupt (%s)", err.what());
    }
  }

  auto find(const nix::CanonPath &path) -> std::optional<TreeEntry> {
    return healing([&]() -> std::optional<TreeEntry> {
      const std::scoped_lock lock(mutex_);
      TreeEntry entry;
      if (!walker_.lookup(root_, path.rel(), entry)) {
        return std::nullopt;
      }
      return entry;
    });
  }

  TreeStore *store_;
  std::string root_;
  TreeWalker walker_;
  CorruptHook on_corrupt_;
  std::mutex mutex_; // TreeWalker cache is not thread-safe
};

#endif
