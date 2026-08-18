#include "tree.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {
using ArchivePtr = std::unique_ptr<archive, int (*)(archive *)>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, void (*)(EVP_MD_CTX *)>;
} // namespace

std::string to_hex(std::string_view raw) {
  static const char *d = "0123456789abcdef";
  std::string out;
  out.reserve(raw.size() * 2);
  for (unsigned char c : raw) {
    out.push_back(d[c >> 4]);
    out.push_back(d[c & 15]);
  }
  return out;
}

std::string from_hex(std::string_view hex) {
  std::string out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    uint8_t byte = 0;
    auto [p, ec] = std::from_chars(hex.data() + i, hex.data() + i + 2, byte, 16);
    if (ec != std::errc{} || p != hex.data() + i + 2)
      throw std::invalid_argument("bad hex string");
    out.push_back(static_cast<char>(byte));
  }
  return out;
}

namespace {

constexpr size_t kMaxNameLen = 0xffff;
constexpr size_t kMaxTreeDepth = 512;

struct MemDir {
  std::map<std::string, MemDir> dirs;
  std::map<std::string, TreeEntry> leaves;
};

std::string flush_dir(TreeStore &store, MemDir &dir) {
  std::vector<TreeEntry> entries;
  entries.reserve(dir.dirs.size() + dir.leaves.size());
  for (auto &[name, sub] : dir.dirs)
    entries.push_back({name, 'd', flush_dir(store, sub)});
  for (auto &[name, leaf] : dir.leaves)
    entries.push_back(std::move(leaf)); // dir is consumed by the flush
  std::ranges::sort(entries, {}, &TreeEntry::name);
  return store.putTree(entries);
}

struct TarItem {
  std::string path;
  char type;
  std::string data;
};

struct ItemQueue {
  std::mutex m;
  std::condition_variable cv;
  std::deque<TarItem> q;
  size_t bytes = 0;
  bool done = false;
  bool closed = false; // consumer bailed out
  std::exception_ptr err;
  static constexpr size_t kMaxItems = 512;
  // bound memory, not just item count: a tarball of large files must not
  // buffer gigabytes; one oversized item is still admitted when empty
  static constexpr size_t kMaxBytes = 128 << 20;

  void push(TarItem &&item) {
    std::unique_lock l(m);
    cv.wait(l, [&] {
      return closed || q.empty() ||
             (q.size() < kMaxItems && bytes + item.data.size() <= kMaxBytes);
    });
    if (closed)
      throw std::runtime_error("consumer gone");
    bytes += item.data.size();
    q.push_back(std::move(item));
    cv.notify_all();
  }

  void close() {
    std::lock_guard l(m);
    closed = true;
    cv.notify_all();
  }

  bool pop(TarItem &out) {
    std::unique_lock l(m);
    cv.wait(l, [&] { return !q.empty() || done; });
    if (q.empty())
      return false;
    out = std::move(q.front());
    q.pop_front();
    bytes -= out.data.size();
    cv.notify_all();
    return true;
  }

  void finish(std::exception_ptr e = nullptr) {
    std::lock_guard l(m);
    done = true;
    err = e;
    cv.notify_all();
  }
};

std::string archive_err(archive *a) {
  const char *msg = archive_error_string(a); // NULL for some errors
  return msg ? msg : "unknown archive error";
}

void read_items(archive *a, ItemQueue &queue) {
  archive_entry *e;
  int rc;
  while ((rc = archive_read_next_header(a, &e)) == ARCHIVE_OK) {
    auto type = archive_entry_filetype(e);
    if (type != AE_IFREG && type != AE_IFLNK && type != AE_IFDIR)
      continue;
    const char *name = archive_entry_pathname(e);
    if (!name)
      throw std::runtime_error("archive entry without name");
    TarItem item;
    item.path = name;
    if (type == AE_IFDIR) {
      item.type = 'd';
    } else if (type == AE_IFLNK) {
      const char *target = archive_entry_symlink(e);
      if (!target)
        throw std::runtime_error("symlink without target");
      item.type = 's';
      item.data = target;
    } else {
      auto size = archive_entry_size(e);
      if (size < 0)
        throw std::runtime_error("negative entry size");
      item.type = (archive_entry_perm(e) & 0111) ? 'x' : 'r';
      // the header size is attacker-controlled: allocate as data arrives
      item.data.reserve(std::min<int64_t>(size, 1 << 20));
      std::array<char, 64 << 10> buf;
      for (;;) {
        auto n = archive_read_data(a, buf.data(), buf.size());
        if (n < 0)
          throw std::runtime_error(archive_err(a));
        if (n == 0)
          break;
        item.data.append(buf.data(), static_cast<size_t>(n));
      }
    }
    queue.push(std::move(item));
  }
  if (rc != ARCHIVE_EOF)
    throw std::runtime_error("archive error: " + archive_err(a));
}

// reject entries that escape the root or nest absurdly deep
bool safe_component(std::string_view comp) {
  return comp != ".." && comp.size() <= kMaxNameLen && !comp.contains('\0');
}

// "./a//b/" -> {"a", "b"}; nullopt for unsafe or too deep paths
std::optional<std::vector<std::string_view>> split_path(std::string_view path) {
  std::vector<std::string_view> comps;
  while (!path.empty()) {
    size_t slash = path.find('/');
    std::string_view comp = path.substr(0, slash);
    path = slash == std::string_view::npos ? "" : path.substr(slash + 1);
    if (comp.empty() || comp == ".")
      continue;
    if (!safe_component(comp) || comps.size() >= kMaxTreeDepth)
      return std::nullopt;
    comps.push_back(comp);
  }
  return comps;
}

} // namespace

std::string TreeStore::putTree(const std::vector<TreeEntry> &entries) {
  std::string buf;
  for (auto &e : entries) {
    if (e.name.size() > kMaxNameLen || e.id.size() > 0xff)
      throw std::invalid_argument("tree entry too large");
    buf.push_back(e.type);
    buf.push_back(static_cast<char>(e.name.size() & 0xff));
    buf.push_back(static_cast<char>(e.name.size() >> 8));
    buf += e.name;
    buf.push_back(static_cast<char>(e.id.size()));
    buf += e.id;
  }
  return cas_->put(buf);
}

std::vector<TreeEntry> TreeStore::readTree(const std::string &id) {
  std::string data;
  if (!cas_->get(id, data))
    throw CorruptError("missing tree object");
  std::string_view buf = data;
  std::vector<TreeEntry> out;
  size_t p = 0;
  while (p < buf.size()) {
    if (p + 3 > buf.size())
      throw CorruptError("corrupt tree object");
    TreeEntry e;
    e.type = buf[p++];
    if (e.type != 'r' && e.type != 'x' && e.type != 's' && e.type != 'd')
      throw CorruptError("corrupt tree object: bad type");
    size_t nl = static_cast<uint8_t>(buf[p]) |
                (static_cast<size_t>(static_cast<uint8_t>(buf[p + 1])) << 8);
    p += 2;
    if (p + nl + 1 > buf.size())
      throw CorruptError("corrupt tree object: truncated");
    e.name = std::string(buf.substr(p, nl));
    p += nl;
    uint8_t il = static_cast<uint8_t>(buf[p++]);
    if (p + il > buf.size())
      throw CorruptError("corrupt tree object: truncated");
    e.id = std::string(buf.substr(p, il));
    p += il;
    // lookup() binary-searches: entries must be strictly sorted
    if (!out.empty() && out.back().name >= e.name)
      throw CorruptError("corrupt tree object: not sorted");
    out.push_back(std::move(e));
  }
  return out;
}

std::string TreeStore::readBlob(const std::string &id) {
  std::string out;
  if (!cas_->get(id, out))
    throw CorruptError("missing blob");
  return out;
}

bool TreeStore::hasTree(const std::string &id) { return cas_->has(id); }

IngestResult ingest_archive(TreeStore &store, archive *a) {
  ArchivePtr guard(a, archive_read_free);
  ItemQueue queue;
  std::jthread producer([&] {
    try {
      read_items(a, queue);
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
      if (!comps || comps->empty())
        continue; // hostile or root entry: skip
      std::string leaf(comps->back());
      comps->pop_back();
      MemDir *dir = &root;
      for (auto comp : *comps)
        dir = &dir->dirs[std::string(comp)];
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
  if (queue.err)
    std::rethrow_exception(queue.err);

  res.root = flush_dir(store, root);
  store.registerRoot(res.root);
  store.sync();
  store.maybeGc();
  return res;
}

IngestResult ingest_tarball_file(TreeStore &store, const std::string &path) {
  ArchivePtr a(archive_read_new(), archive_read_free);
  archive_read_support_filter_all(a.get());
  archive_read_support_format_all(a.get());
  if (archive_read_open_filename(a.get(), path.c_str(), 1 << 20) != ARCHIVE_OK)
    throw std::runtime_error(path + ": " + archive_err(a.get()));
  return ingest_archive(store, a.release());
}

// GC: roots idle past the TTL are dropped, then mark-and-sweep compaction

namespace {

std::string be64(uint64_t v) {
  std::string s(8, '\0');
  for (int i = 0; i < 8; i++)
    s[7 - i] = static_cast<char>(v >> (8 * i));
  return s;
}

uint64_t rbe64(std::string_view s) {
  uint64_t v = 0;
  for (unsigned char c : s)
    v = (v << 8) | c;
  return v;
}

uint64_t now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

} // namespace

void TreeStore::registerRoot(const std::string &root) {
  cas_->meta_put("R" + root, be64(now_ns()));
}

void TreeStore::touchRoot(const std::string &root) {
  std::string v;
  if (!cas_->meta_get("R" + root, v) || v.size() != 8)
    return;
  uint64_t now = now_ns();
  if (now - rbe64(v) < touch_interval_ns_)
    return; // steady state: cache hits write nothing
  cas_->meta_put("R" + root, be64(now));
}

void TreeStore::mark_live(const std::string &id,
                          std::unordered_set<std::string> &live) {
  if (!live.insert(id).second)
    return;
  for (auto &e : readTree(id)) {
    if (e.type == 'd')
      mark_live(e.id, live);
    else
      live.insert(e.id);
  }
}

void TreeStore::maybeGc() {
  uint64_t now = now_ns();
  std::string v;
  if (cas_->meta_get("lastgc", v) && v.size() == 8 &&
      now - rbe64(v) < gc_interval_ns_)
    return;
  cas_->meta_put("lastgc", be64(now));
  size_t expired = 0;
  for (auto &r : roots_info()) {
    if (now - r.last_access < ttl_ns_)
      continue;
    cas_->meta_del("R" + r.root);
    expired++;
  }
  if (expired == 0)
    return;
  std::unordered_set<std::string> live;
  std::vector<std::string> broken;
  cas_->meta_scan("R", [&](std::string_view k, std::string_view) {
    std::string root(k.substr(1));
    try {
      mark_live(root, live);
    } catch (const std::exception &) {
      broken.push_back(root);
    }
    return true;
  });
  for (auto &root : broken)
    cas_->meta_del("R" + root);
  cas_->compact(
      [&](std::string_view h) { return live.contains(std::string(h)); });
}

std::vector<TreeStore::RootInfo> TreeStore::roots_info() {
  std::vector<RootInfo> out;
  cas_->meta_scan("R", [&](std::string_view k, std::string_view v) {
    if (v.size() == 8)
      out.push_back({std::string(k.substr(1)), rbe64(v)});
    return true;
  });
  return out;
}

const std::vector<TreeEntry> &TreeWalker::tree(const std::string &id) {
  auto [it, inserted] = cache.try_emplace(id);
  if (inserted)
    it->second = store.readTree(id);
  return it->second;
}

bool TreeWalker::lookup(const std::string &root, std::string_view path,
                        TreeEntry &out) {
  auto comps = split_path(path);
  if (!comps)
    return false;
  out = {"", 'd', root};
  for (auto comp : *comps) {
    if (out.type != 'd')
      return false;
    auto &entries = tree(out.id);
    auto it = std::ranges::lower_bound(entries, comp, {}, &TreeEntry::name);
    if (it == entries.end() || it->name != comp)
      return false;
    out = *it;
  }
  return true;
}

namespace {

void nar_str(const std::function<void(std::string_view)> &sink,
             std::string_view s) {
  uint64_t len = s.size();
  char lenbuf[8];
  for (int i = 0; i < 8; i++)
    lenbuf[i] = static_cast<char>(len >> (8 * i));
  sink({lenbuf, 8});
  sink(s);
  static const char pad[8] = {};
  if (len % 8)
    sink({pad, 8 - len % 8});
}

void nar_node(TreeStore &store, const TreeEntry &e,
              const std::function<void(std::string_view)> &sink,
              size_t depth) {
  if (depth > kMaxTreeDepth)
    throw std::runtime_error("tree too deep");
  auto blob = [&](const std::string &id) -> std::string_view {
    std::string_view v;
    if (!store.readBlobView(id, v))
      throw CorruptError("missing blob");
    return v;
  };
  nar_str(sink, "(");
  nar_str(sink, "type");
  if (e.type == 's') {
    nar_str(sink, "symlink");
    nar_str(sink, "target");
    nar_str(sink, blob(e.id));
  } else if (e.type == 'd') {
    nar_str(sink, "directory");
    for (auto &child : store.readTree(e.id)) {
      nar_str(sink, "entry");
      nar_str(sink, "(");
      nar_str(sink, "name");
      nar_str(sink, child.name);
      nar_str(sink, "node");
      nar_node(store, child, sink, depth + 1);
      nar_str(sink, ")");
    }
  } else {
    nar_str(sink, "regular");
    if (e.type == 'x') {
      nar_str(sink, "executable");
      nar_str(sink, "");
    }
    nar_str(sink, "contents");
    nar_str(sink, blob(e.id));
  }
  nar_str(sink, ")");
}

} // namespace

void nar_dump(TreeStore &store, const std::string &root,
              const std::function<void(std::string_view)> &sink) {
  nar_str(sink, "nix-archive-1");
  nar_node(store, {"", 'd', root}, sink, 0);
}

std::string nar_sha256(TreeStore &store, const std::string &root,
                       uint64_t &nar_size) {
  MdCtxPtr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("sha256 init failed");
  nar_size = 0;
  std::string buf;
  buf.reserve(1 << 16);
  auto update = [&](std::string_view d) {
    if (EVP_DigestUpdate(ctx.get(), d.data(), d.size()) != 1)
      throw std::runtime_error("sha256 update failed");
  };
  auto flush = [&] {
    if (!buf.empty()) {
      update(buf);
      buf.clear();
    }
  };
  nar_dump(store, root, [&](std::string_view chunk) {
    nar_size += chunk.size();
    if (chunk.size() >= 256) {
      flush();
      update(chunk);
      return;
    }
    if (buf.size() + chunk.size() > (1 << 16))
      flush();
    buf += chunk;
  });
  flush();
  std::string out(32, '\0');
  unsigned int len = 32;
  if (EVP_DigestFinal_ex(ctx.get(),
                         reinterpret_cast<unsigned char *>(out.data()),
                         &len) != 1 ||
      len != 32)
    throw std::runtime_error("sha256 final failed");
  return out;
}
