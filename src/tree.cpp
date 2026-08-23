#include "tree.hpp"
#include "pack_cas.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <openssl/evp.h>
#include <sys/types.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace {

using ArchivePtr = std::unique_ptr<archive, int (*)(archive *)>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, void (*)(EVP_MD_CTX *)>;

constexpr unsigned kByteBits = 8;
constexpr unsigned kByteMask = 0xff;
constexpr unsigned kNibbleBits = 4;
constexpr unsigned kNibbleMask = 0xf;
constexpr int kHexBase = 16;
constexpr std::string_view kHexDigits = "0123456789abcdef";

constexpr size_t kMaxNameLen = 0xffff;
constexpr size_t kMaxIdLen = 0xff;
constexpr size_t kMaxTreeDepth = 512;
// type byte, two length bytes for the name, one for the id
constexpr size_t kTreeEntryHeader = 3;

constexpr mode_t kExecBits = 0111;
constexpr int64_t kMaxReserve = 1 << 20;
constexpr size_t kReadChunk = 64 << 10;
constexpr size_t kArchiveBlockSize = 1 << 20;

constexpr size_t kSha256Len = 32;
constexpr size_t kHashBufSize = 1 << 16;
// chunks this big are hashed directly instead of being copied into the buffer
constexpr size_t kHashDirectThreshold = 256;

} // namespace

auto to_hex(std::string_view raw) -> std::string {
  std::string out;
  out.reserve(raw.size() * 2);
  for (const unsigned char byte : raw) {
    out.push_back(kHexDigits[byte >> kNibbleBits]);
    out.push_back(kHexDigits[byte & kNibbleMask]);
  }
  return out;
}

auto from_hex(std::string_view hex) -> std::string {
  std::string out;
  out.reserve(hex.size() / 2);
  for (size_t pos = 0; pos + 1 < hex.size(); pos += 2) {
    const std::string_view pair = hex.substr(pos, 2);
    const char *pair_end = std::to_address(pair.end());
    uint8_t byte = 0;
    auto [last, err] = std::from_chars(std::to_address(pair.begin()), pair_end,
                                       byte, kHexBase);
    if (err != std::errc{} || last != pair_end) {
      throw std::invalid_argument("bad hex string");
    }
    out.push_back(static_cast<char>(byte));
  }
  return out;
}

namespace {

struct MemDir {
  std::map<std::string, MemDir> dirs;
  std::map<std::string, TreeEntry> leaves;
};

// consumes the tree; children are flushed before their parent by walking
// a pre-order listing backwards
auto flush_tree(TreeStore &store, MemDir &root) -> std::string {
  struct Pending {
    MemDir *dir;
    MemDir *parent;
    std::string name;
  };
  std::vector<Pending> order;
  order.push_back({&root, nullptr, ""});
  for (size_t pos = 0; pos < order.size(); pos++) {
    MemDir *dir = order[pos].dir;
    for (auto &[name, sub] : dir->dirs) {
      order.push_back({&sub, dir, name});
    }
  }
  std::string root_id;
  for (auto &pending : std::views::reverse(order)) {
    std::vector<TreeEntry> entries;
    entries.reserve(pending.dir->leaves.size());
    for (auto &[name, leaf] : pending.dir->leaves) {
      entries.push_back(std::move(leaf));
    }
    std::ranges::sort(entries, {}, &TreeEntry::name);
    std::string tree_id = store.putTree(entries);
    if (pending.parent == nullptr) {
      root_id = std::move(tree_id);
    } else {
      pending.parent->leaves[pending.name] = {pending.name, 'd',
                                              std::move(tree_id)};
    }
  }
  return root_id;
}

struct TarItem {
  std::string path;
  char type{};
  std::string data;
};

struct ItemQueue {
  std::mutex mutex;
  std::condition_variable cond;
  std::deque<TarItem> items;
  size_t bytes = 0;
  bool done = false;
  bool closed = false; // consumer bailed out
  std::exception_ptr err;
  static constexpr size_t kMaxItems = 512;
  // bound memory, not just item count: a tarball of large files must not
  // buffer gigabytes; one oversized item is still admitted when empty
  static constexpr size_t kMaxBytes = 128 << 20;

  void push(TarItem &&item) {
    std::unique_lock lock(mutex);
    cond.wait(lock, [&] -> bool {
      return closed || items.empty() ||
             (items.size() < kMaxItems &&
              bytes + item.data.size() <= kMaxBytes);
    });
    if (closed) {
      throw std::runtime_error("consumer gone");
    }
    bytes += item.data.size();
    items.push_back(std::move(item));
    cond.notify_all();
  }

  void close() {
    const std::scoped_lock lock(mutex);
    closed = true;
    cond.notify_all();
  }

  auto pop(TarItem &out) -> bool {
    std::unique_lock lock(mutex);
    cond.wait(lock, [&] -> bool { return !items.empty() || done; });
    if (items.empty()) {
      return false;
    }
    out = std::move(items.front());
    items.pop_front();
    bytes -= out.data.size();
    cond.notify_all();
    return true;
  }

  void finish(std::exception_ptr error = nullptr) {
    const std::scoped_lock lock(mutex);
    done = true;
    err = std::move(error);
    cond.notify_all();
  }
};

auto archive_err(archive *arc) -> std::string {
  const char *msg = archive_error_string(arc); // NULL for some errors
  return msg != nullptr ? msg : "unknown archive error";
}

using ReadBuf = std::array<char, kReadChunk>;

void read_regular(archive *arc, archive_entry *header, ReadBuf &buf,
                  TarItem &item) {
  const auto size = archive_entry_size(header);
  if (size < 0) {
    throw std::runtime_error("negative entry size");
  }
  item.type = (archive_entry_perm(header) & kExecBits) != 0 ? 'x' : 'r';
  // the header size is attacker-controlled: allocate as data arrives
  item.data.reserve(static_cast<size_t>(std::min(size, kMaxReserve)));
  for (;;) {
    const auto got = archive_read_data(arc, buf.data(), buf.size());
    if (got < 0) {
      throw std::runtime_error(archive_err(arc));
    }
    if (got == 0) {
      break;
    }
    item.data.append(buf.data(), static_cast<size_t>(got));
  }
}

auto read_item(archive *arc, archive_entry *header, ReadBuf &buf)
    -> std::optional<TarItem> {
  const auto type = archive_entry_filetype(header);
  if (type != AE_IFREG && type != AE_IFLNK && type != AE_IFDIR) {
    return std::nullopt;
  }
  const char *name = archive_entry_pathname(header);
  if (name == nullptr) {
    throw std::runtime_error("archive entry without name");
  }
  TarItem item;
  item.path = name;
  if (type == AE_IFDIR) {
    item.type = 'd';
  } else if (type == AE_IFLNK) {
    const char *target = archive_entry_symlink(header);
    if (target == nullptr) {
      throw std::runtime_error("symlink without target");
    }
    item.type = 's';
    item.data = target;
  } else {
    read_regular(arc, header, buf, item);
  }
  return item;
}

void read_items(archive *arc, ItemQueue &queue) {
  archive_entry *header = nullptr;
  auto buf = std::make_unique<ReadBuf>();
  int res = 0;
  while ((res = archive_read_next_header(arc, &header)) == ARCHIVE_OK) {
    if (auto item = read_item(arc, header, *buf)) {
      queue.push(std::move(*item));
    }
  }
  if (res != ARCHIVE_EOF) {
    throw std::runtime_error("archive error: " + archive_err(arc));
  }
}

// reject entries that escape the root or nest absurdly deep
auto safe_component(std::string_view comp) -> bool {
  return comp != ".." && comp.size() <= kMaxNameLen && !comp.contains('\0');
}

// "./a//b/" -> {"a", "b"}; nullopt for unsafe or too deep paths
auto split_path(std::string_view path)
    -> std::optional<std::vector<std::string_view>> {
  std::vector<std::string_view> comps;
  while (!path.empty()) {
    const size_t slash = path.find('/');
    const std::string_view comp = path.substr(0, slash);
    path = slash == std::string_view::npos ? "" : path.substr(slash + 1);
    if (comp.empty() || comp == ".") {
      continue;
    }
    if (!safe_component(comp) || comps.size() >= kMaxTreeDepth) {
      return std::nullopt;
    }
    comps.push_back(comp);
  }
  return comps;
}

} // namespace

auto TreeStore::putTree(const std::vector<TreeEntry> &entries) -> std::string {
  std::string buf;
  for (const auto &entry : entries) {
    if (entry.name.size() > kMaxNameLen || entry.id.size() > kMaxIdLen) {
      throw std::invalid_argument("tree entry too large");
    }
    buf.push_back(entry.type);
    buf.push_back(static_cast<char>(entry.name.size() & kByteMask));
    buf.push_back(static_cast<char>(entry.name.size() >> kByteBits));
    buf += entry.name;
    buf.push_back(static_cast<char>(entry.id.size()));
    buf += entry.id;
  }
  return cas_->put(buf);
}

auto TreeStore::readTree(const std::string &tree_id) -> std::vector<TreeEntry> {
  std::string data;
  if (!cas_->get(tree_id, data)) {
    throw CorruptError("missing tree object");
  }
  const std::string_view buf = data;
  std::vector<TreeEntry> out;
  size_t pos = 0;
  while (pos < buf.size()) {
    if (pos + kTreeEntryHeader > buf.size()) {
      throw CorruptError("corrupt tree object");
    }
    TreeEntry entry;
    entry.type = buf[pos++];
    if (entry.type != 'r' && entry.type != 'x' && entry.type != 's' &&
        entry.type != 'd') {
      throw CorruptError("corrupt tree object: bad type");
    }
    const size_t name_len =
        static_cast<uint8_t>(buf[pos]) |
        (static_cast<size_t>(static_cast<uint8_t>(buf[pos + 1])) << kByteBits);
    pos += 2;
    if (pos + name_len + 1 > buf.size()) {
      throw CorruptError("corrupt tree object: truncated");
    }
    entry.name = std::string(buf.substr(pos, name_len));
    pos += name_len;
    const auto id_len = static_cast<uint8_t>(buf[pos++]);
    if (pos + id_len > buf.size()) {
      throw CorruptError("corrupt tree object: truncated");
    }
    entry.id = std::string(buf.substr(pos, id_len));
    pos += id_len;
    // lookup() binary-searches: entries must be strictly sorted
    if (!out.empty() && out.back().name >= entry.name) {
      throw CorruptError("corrupt tree object: not sorted");
    }
    out.push_back(std::move(entry));
  }
  return out;
}

auto TreeStore::readBlob(const std::string &blob_id) -> std::string {
  std::string out;
  if (!cas_->get(blob_id, out)) {
    throw CorruptError("missing blob");
  }
  return out;
}

auto TreeStore::hasTree(const std::string &tree_id) -> bool {
  return cas_->has(tree_id);
}

auto ingest_archive(TreeStore &store, archive *arc) -> IngestResult {
  const ArchivePtr guard(arc, archive_read_free);
  ItemQueue queue;
  std::jthread producer([&] -> void {
    try {
      read_items(arc, queue);
      queue.finish();
    } catch (...) {
      queue.finish(std::current_exception());
    }
  });

  IngestResult res;
  MemDir root;
  TarItem item;
  try {
    while (queue.pop(item)) {
      auto comps = split_path(item.path);
      if (!comps || comps->empty()) {
        continue; // hostile or root entry: skip
      }
      std::string leaf(comps->back());
      comps->pop_back();
      MemDir *dir = &root;
      for (const auto comp : *comps) {
        dir = &dir->dirs[std::string(comp)];
      }
      if (item.type == 'd') {
        dir->dirs[std::move(leaf)];
      } else {
        auto &slot = dir->leaves[leaf];
        slot = {std::move(leaf), item.type, store.putBlob(item.data)};
        res.files++;
      }
    }
  } catch (...) {
    queue.close(); // unblock the producer so jthread can join
    throw;
  }
  producer.join();
  if (queue.err) {
    std::rethrow_exception(queue.err);
  }

  res.root = flush_tree(store, root);
  store.registerRoot(res.root);
  store.sync();
  store.maybeGc();
  return res;
}

auto ingest_tarball_file(TreeStore &store, const std::string &path)
    -> IngestResult {
  ArchivePtr arc(archive_read_new(), archive_read_free);
  archive_read_support_filter_all(arc.get());
  archive_read_support_format_all(arc.get());
  if (archive_read_open_filename(arc.get(), path.c_str(), kArchiveBlockSize) !=
      ARCHIVE_OK) {
    throw std::runtime_error(path + ": " + archive_err(arc.get()));
  }
  return ingest_archive(store, arc.release());
}

// GC: roots idle past the TTL are dropped, then mark-and-sweep compaction

namespace {

auto be64(uint64_t value) -> std::string {
  std::array<char, sizeof(uint64_t)> buf{};
  for (auto &byte : std::views::reverse(buf)) {
    byte = static_cast<char>(value & kByteMask);
    value >>= kByteBits;
  }
  return {buf.begin(), buf.end()};
}

auto rbe64(std::string_view encoded) -> uint64_t {
  uint64_t value = 0;
  for (const unsigned char byte : encoded) {
    value = (value << kByteBits) | byte;
  }
  return value;
}

auto is_timestamp(std::string_view encoded) -> bool {
  return encoded.size() == sizeof(uint64_t);
}

auto now_ns() -> uint64_t {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

auto root_key(const std::string &root, bool tree) -> std::string {
  return (tree ? "R" : "B") + root;
}

} // namespace

void TreeStore::registerRoot(const std::string &root, bool tree) {
  cas_->meta_put(root_key(root, tree), be64(now_ns()));
}

void TreeStore::touchRoot(const std::string &root, bool tree) {
  const std::string key = root_key(root, tree);
  std::string stamp;
  if (!cas_->meta_get(key, stamp) || !is_timestamp(stamp)) {
    return;
  }
  const uint64_t now = now_ns();
  if (now - rbe64(stamp) < touch_interval_ns_) {
    return; // steady state: cache hits write nothing
  }
  cas_->meta_put(key, be64(now));
}

auto TreeStore::expire_roots(std::string_view prefix, uint64_t now) -> size_t {
  std::vector<std::string> expired;
  cas_->meta_scan(prefix,
                  [&](std::string_view key, std::string_view val) -> bool {
                    if (is_timestamp(val) && now - rbe64(val) >= ttl_ns_) {
                      expired.emplace_back(key);
                    }
                    return true;
                  });
  for (const auto &key : expired) {
    cas_->meta_del(key);
  }
  return expired.size();
}

void TreeStore::mark_live(const std::string &root,
                          std::unordered_set<std::string> &live) {
  std::vector<std::string> pending;
  if (live.insert(root).second) {
    pending.push_back(root);
  }
  while (!pending.empty()) {
    const std::string tree_id = std::move(pending.back());
    pending.pop_back();
    for (auto &entry : readTree(tree_id)) {
      if (live.insert(entry.id).second && entry.type == 'd') {
        pending.push_back(std::move(entry.id));
      }
    }
  }
}

void TreeStore::maybeGc() {
  const uint64_t now = now_ns();
  std::string stamp;
  if (cas_->meta_get("lastgc", stamp) && is_timestamp(stamp) &&
      now - rbe64(stamp) < gc_interval_ns_) {
    return;
  }
  cas_->meta_put("lastgc", be64(now));
  if (expire_roots("R", now) + expire_roots("B", now) == 0) {
    return;
  }
  std::unordered_set<std::string> live;
  std::vector<std::string> broken;
  cas_->meta_scan("R", [&](std::string_view key, std::string_view) -> bool {
    const std::string root(key.substr(1));
    try {
      mark_live(root, live);
    } catch (const std::exception &) {
      broken.push_back(root);
    }
    return true;
  });
  for (const auto &root : broken) {
    cas_->meta_del("R" + root);
  }
  cas_->meta_scan("B", [&](std::string_view key, std::string_view) -> bool {
    live.insert(std::string(key.substr(1)));
    return true;
  });
  cas_->compact([&](std::string_view hash) -> bool {
    return live.contains(std::string(hash));
  });
}

auto TreeStore::roots_info() -> std::vector<TreeStore::RootInfo> {
  std::vector<RootInfo> out;
  cas_->meta_scan("R", [&](std::string_view key, std::string_view val) -> bool {
    if (is_timestamp(val)) {
      out.push_back({std::string(key.substr(1)), rbe64(val)});
    }
    return true;
  });
  return out;
}

auto TreeWalker::tree(const std::string &tree_id)
    -> const std::vector<TreeEntry> & {
  auto [slot, inserted] = cache_.try_emplace(tree_id);
  if (inserted) {
    slot->second = store_->readTree(tree_id);
  }
  return slot->second;
}

auto TreeWalker::lookup(const std::string &root, std::string_view path,
                        TreeEntry &out) -> bool {
  const auto comps = split_path(path);
  if (!comps) {
    return false;
  }
  out = {"", 'd', root};
  for (const auto comp : *comps) {
    if (out.type != 'd') {
      return false;
    }
    const auto &entries = tree(out.id);
    const auto found =
        std::ranges::lower_bound(entries, comp, {}, &TreeEntry::name);
    if (found == entries.end() || found->name != comp) {
      return false;
    }
    out = *found;
  }
  return true;
}

namespace {

using Sink = std::function<void(std::string_view)>;

void nar_str(const Sink &sink, std::string_view str) {
  const uint64_t len = str.size();
  std::array<char, sizeof(uint64_t)> lenbuf{};
  for (size_t idx = 0; idx < lenbuf.size(); idx++) {
    lenbuf.at(idx) = static_cast<char>(len >> (kByteBits * idx));
  }
  sink({lenbuf.data(), lenbuf.size()});
  sink(str);
  static constexpr std::array<char, sizeof(uint64_t)> kPad{};
  const size_t rem = len % kPad.size();
  if (rem != 0) {
    sink({kPad.data(), kPad.size() - rem});
  }
}

struct NarNode {
  TreeEntry entry;
  size_t depth;
};

// work items are processed LIFO, so children are pushed in reverse order
using NarWork = std::variant<std::string, NarNode>;

void nar_leaf(TreeStore &store, const TreeEntry &entry, const Sink &sink) {
  std::string scratch;
  std::string_view contents;
  if (!store.readBlobView(entry.id, scratch, contents)) {
    throw CorruptError("missing blob");
  }
  if (entry.type == 's') {
    nar_str(sink, "symlink");
    nar_str(sink, "target");
  } else {
    nar_str(sink, "regular");
    if (entry.type == 'x') {
      nar_str(sink, "executable");
      nar_str(sink, "");
    }
    nar_str(sink, "contents");
  }
  nar_str(sink, contents);
  nar_str(sink, ")");
}

void nar_directory(TreeStore &store, const NarNode &node, const Sink &sink,
                   std::vector<NarWork> &work) {
  nar_str(sink, "directory");
  work.emplace_back(")");
  auto children = store.readTree(node.entry.id);
  for (auto &child : std::views::reverse(children)) {
    work.emplace_back(")");
    std::string name = child.name;
    work.emplace_back(NarNode{std::move(child), node.depth + 1});
    work.emplace_back("node");
    work.emplace_back(std::move(name));
    work.emplace_back("name");
    work.emplace_back("(");
    work.emplace_back("entry");
  }
}

} // namespace

void nar_dump(TreeStore &store, const std::string &root, const Sink &sink) {
  nar_str(sink, "nix-archive-1");
  std::vector<NarWork> work;
  work.emplace_back(NarNode{{"", 'd', root}, 0});
  while (!work.empty()) {
    NarWork item = std::move(work.back());
    work.pop_back();
    if (const auto *str = std::get_if<std::string>(&item)) {
      nar_str(sink, *str);
      continue;
    }
    const auto &node = std::get<NarNode>(item);
    if (node.depth > kMaxTreeDepth) {
      throw std::runtime_error("tree too deep");
    }
    nar_str(sink, "(");
    nar_str(sink, "type");
    if (node.entry.type == 'd') {
      nar_directory(store, node, sink, work);
    } else {
      nar_leaf(store, node.entry, sink);
    }
  }
}

auto nar_sha256(TreeStore &store, const std::string &root, uint64_t &nar_size)
    -> std::string {
  const MdCtxPtr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
    throw std::runtime_error("sha256 init failed");
  }
  nar_size = 0;
  std::string buf;
  buf.reserve(kHashBufSize);
  auto update = [&](std::string_view chunk) -> void {
    if (EVP_DigestUpdate(ctx.get(), chunk.data(), chunk.size()) != 1) {
      throw std::runtime_error("sha256 update failed");
    }
  };
  auto flush = [&] -> void {
    if (!buf.empty()) {
      update(buf);
      buf.clear();
    }
  };
  nar_dump(store, root, [&](std::string_view chunk) -> void {
    nar_size += chunk.size();
    if (chunk.size() >= kHashDirectThreshold) {
      flush();
      update(chunk);
      return;
    }
    if (buf.size() + chunk.size() > kHashBufSize) {
      flush();
    }
    buf += chunk;
  });
  flush();
  std::array<unsigned char, kSha256Len> digest{};
  unsigned int len = digest.size();
  if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &len) != 1 ||
      len != digest.size()) {
    throw std::runtime_error("sha256 final failed");
  }
  return {digest.begin(), digest.end()};
}
