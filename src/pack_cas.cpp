#include "pack_cas.hpp"

#include <blake3.h>
#include <fcntl.h>
#include <lmdb.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <vector>

std::string blake3_hash(std::string_view data) {
  blake3_hasher h;
  blake3_hasher_init(&h);
  blake3_hasher_update(&h, data.data(), data.size());
  std::string out(BLAKE3_OUT_LEN, '\0');
  blake3_hasher_finalize(&h, reinterpret_cast<uint8_t *>(out.data()),
                         BLAKE3_OUT_LEN);
  return out;
}

namespace {

constexpr size_t kMapSize = 1UL << 40;
constexpr size_t kIndexMapSize = 32UL << 30;
constexpr uint64_t kAutoSyncBytes = 256UL << 20;
constexpr size_t kHashLen = 32;
constexpr int kLockTimeoutSec = 600;

[[noreturn]] void sys_err(const std::string &what) {
  throw std::system_error(errno, std::generic_category(), what);
}

void mdb_check(int rc, const char *what) {
  if (rc != MDB_SUCCESS)
    throw std::runtime_error(std::string(what) + ": " + mdb_strerror(rc));
}

struct IndexEntry {
  uint64_t off;
  uint64_t len;
};
static_assert(sizeof(IndexEntry) == 16);

// aborts the txn unless it is the caller-owned write txn
class ReadTxn {
public:
  ReadTxn(MDB_env *env, MDB_txn *wtxn) : owned_(!wtxn), txn_(wtxn) {
    if (owned_)
      mdb_check(mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn_), "txn_begin");
  }
  ~ReadTxn() {
    if (owned_)
      mdb_txn_abort(txn_);
  }
  ReadTxn(const ReadTxn &) = delete;
  ReadTxn &operator=(const ReadTxn &) = delete;
  MDB_txn *get() const { return txn_; }

private:
  bool owned_;
  MDB_txn *txn_;
};

// commits or aborts an owned write txn; joins an open batch txn instead
class WriteTxn {
public:
  WriteTxn(MDB_env *env, MDB_txn *wtxn) : owned_(!wtxn), txn_(wtxn) {
    if (owned_)
      mdb_check(mdb_txn_begin(env, nullptr, 0, &txn_), "txn_begin");
  }
  ~WriteTxn() {
    if (owned_ && txn_)
      mdb_txn_abort(txn_);
  }
  WriteTxn(const WriteTxn &) = delete;
  WriteTxn &operator=(const WriteTxn &) = delete;
  MDB_txn *get() const { return txn_; }
  void commit() {
    if (owned_) {
      MDB_txn *t = txn_;
      txn_ = nullptr;
      mdb_check(mdb_txn_commit(t), "txn_commit");
    }
  }

private:
  bool owned_;
  MDB_txn *txn_;
};

struct Mapping {
  char *base = static_cast<char *>(MAP_FAILED);
  int fd = -1;
  uint64_t gen = 0;
  std::atomic<uint64_t> size{0};

  ~Mapping() {
    if (fd >= 0)
      close(fd);
  }
};

void write_all(int fd, std::string_view data) {
  size_t off = 0;
  while (off < data.size()) {
    ssize_t n = write(fd, data.data() + off, data.size() - off);
    if (n < 0)
      sys_err("write pack");
    off += static_cast<size_t>(n);
  }
}

MDB_val to_val(std::string_view s) {
  return {s.size(), const_cast<char *>(s.data())};
}

} // namespace

struct PackCas::Impl {
  std::string dir;
  MDB_env *env = nullptr;
  MDB_dbi blobs{};
  MDB_dbi meta{};
  int lock_fd = -1;

  std::mutex map_mu;
  std::shared_ptr<Mapping> cur;
  std::vector<std::shared_ptr<Mapping>> retired;

  // writer state, valid while the flock is held
  MDB_txn *wtxn = nullptr;
  uint64_t committed_end = 0; // pack size covered by committed index
  uint64_t end = 0;           // pack size including current batch
  uint64_t batch_bytes = 0;

  ~Impl() {
    if (wtxn)
      abort_batch();
    for (auto &m : retired)
      if (m->base != MAP_FAILED)
        munmap(m->base, kMapSize);
    if (cur && cur->base != MAP_FAILED)
      munmap(cur->base, kMapSize);
    retired.clear();
    cur.reset();
    if (lock_fd >= 0)
      close(lock_fd);
    if (env)
      mdb_env_close(env);
  }

  std::string pack_path(uint64_t gen) const {
    return dir + "/pack." + std::to_string(gen);
  }

  uint64_t read_gen(MDB_txn *txn) {
    MDB_val k = to_val("gen"), v;
    int rc = mdb_get(txn, meta, &k, &v);
    if (rc == MDB_NOTFOUND)
      return 0;
    mdb_check(rc, "mdb_get gen");
    if (v.mv_size != 8)
      throw CorruptError("corrupt generation record");
    uint64_t g;
    memcpy(&g, v.mv_data, 8);
    return g;
  }

  void write_gen(MDB_txn *txn, uint64_t g) {
    MDB_val k = to_val("gen"), v{8, &g};
    mdb_check(mdb_put(txn, meta, &k, &v, 0), "mdb_put gen");
  }

  std::shared_ptr<Mapping> map_pack(uint64_t gen, bool create) {
    auto m = std::make_shared<Mapping>();
    m->gen = gen;
    int flags = O_RDWR | O_CLOEXEC | (create ? O_CREAT : 0);
    m->fd = ::open(pack_path(gen).c_str(), flags, 0644);
    if (m->fd < 0) {
      if (!create && errno == ENOENT)
        return nullptr;
      sys_err("open " + pack_path(gen));
    }
    struct stat st;
    if (fstat(m->fd, &st) < 0)
      sys_err("fstat pack");
    m->size = static_cast<uint64_t>(st.st_size);
    m->base = static_cast<char *>(
        mmap(nullptr, kMapSize, PROT_READ, MAP_SHARED, m->fd, 0));
    if (m->base == MAP_FAILED)
      sys_err("mmap pack");
    return m;
  }

  // never unmaps: in-flight views into old generations must stay valid
  std::shared_ptr<Mapping> mapping_for(uint64_t gen, bool create) {
    std::lock_guard l(map_mu);
    if (cur && cur->gen == gen)
      return cur;
    for (auto &m : retired)
      if (m->gen == gen)
        return m;
    auto m = map_pack(gen, create);
    if (!m)
      return nullptr;
    if (!cur || gen > cur->gen) {
      if (cur)
        retired.push_back(cur);
      cur = m;
    } else {
      retired.push_back(m);
    }
    return m;
  }

  std::shared_ptr<Mapping> current() {
    std::lock_guard l(map_mu);
    return cur;
  }

  void lock_writer() {
    for (int waited_ms = 0;; waited_ms += 50) {
      if (flock(lock_fd, LOCK_EX | LOCK_NB) == 0)
        return;
      if (errno != EWOULDBLOCK)
        sys_err("flock");
      if (waited_ms >= kLockTimeoutSec * 1000)
        throw std::runtime_error("timed out waiting for the pack writer "
                                 "lock; is another process stuck?");
      usleep(50 * 1000);
    }
  }

  void begin_batch() {
    lock_writer();
    try {
      mdb_check(mdb_txn_begin(env, nullptr, 0, &wtxn), "txn_begin");
      auto m = mapping_for(read_gen(wtxn), true);
      struct stat st;
      if (fstat(m->fd, &st) < 0)
        sys_err("fstat pack");
      end = committed_end = static_cast<uint64_t>(st.st_size);
      m->size = end;
      batch_bytes = 0;
    } catch (...) {
      if (wtxn) {
        mdb_txn_abort(wtxn);
        wtxn = nullptr;
      }
      flock(lock_fd, LOCK_UN);
      throw;
    }
  }

  void commit_batch() {
    auto m = current();
    if (fdatasync(m->fd) < 0)
      sys_err("fdatasync pack");
    mdb_check(mdb_txn_commit(wtxn), "txn_commit");
    wtxn = nullptr;
    mdb_check(mdb_env_sync(env, 1), "env_sync");
    committed_end = end;
    m->size = end;
    flock(lock_fd, LOCK_UN);
  }

  void abort_batch() {
    mdb_txn_abort(wtxn);
    wtxn = nullptr;
    // drop appended-but-uncommitted bytes; we still hold the flock
    auto m = current();
    if (ftruncate(m->fd, static_cast<off_t>(committed_end)) == 0)
      end = committed_end;
    m->size = end;
    flock(lock_fd, LOCK_UN);
  }

  bool lookup(std::string_view hash, uint64_t &off, uint64_t &len,
              uint64_t *gen_out) {
    if (hash.size() != kHashLen)
      throw std::invalid_argument("bad hash length");
    ReadTxn txn(env, wtxn);
    if (gen_out)
      *gen_out = read_gen(txn.get());
    MDB_val k = to_val(hash), v;
    int rc = mdb_get(txn.get(), blobs, &k, &v);
    if (rc == MDB_NOTFOUND)
      return false;
    mdb_check(rc, "mdb_get");
    if (v.mv_size != sizeof(IndexEntry))
      throw CorruptError("corrupt index entry");
    IndexEntry e;
    memcpy(&e, v.mv_data, sizeof(e));
    off = e.off;
    len = e.len;
    return true;
  }

  // only safe with the writer lock
  void cleanup_orphans(uint64_t gen) {
    namespace fs = std::filesystem;
    for (auto &entry : fs::directory_iterator(dir)) {
      auto name = entry.path().filename().string();
      if (!name.starts_with("pack."))
        continue;
      uint64_t g = 0;
      auto num = std::string_view(name).substr(5);
      auto [p, ec] =
          std::from_chars(num.data(), num.data() + num.size(), g);
      if (ec != std::errc{} || p != num.data() + num.size())
        continue;
      if (g != gen)
        unlink(entry.path().c_str());
    }
  }
};

PackCas::PackCas(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

PackCas::~PackCas() = default;

std::unique_ptr<PackCas> PackCas::open(const std::string &dir) {
  try {
    return open_once(dir);
  } catch (const std::exception &) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return open_once(dir);
  }
}

std::unique_ptr<PackCas> PackCas::open_once(const std::string &dir) {
  auto impl = std::make_unique<Impl>();
  impl->dir = dir;
  std::filesystem::create_directories(dir);
  std::string idx_dir = dir + "/index";
  if (mkdir(idx_dir.c_str(), 0755) < 0 && errno != EEXIST)
    sys_err("mkdir " + idx_dir);

  mdb_check(mdb_env_create(&impl->env), "env_create");
  mdb_check(mdb_env_set_mapsize(impl->env, kIndexMapSize), "set_mapsize");
  mdb_check(mdb_env_set_maxdbs(impl->env, 2), "set_maxdbs");
  mdb_check(mdb_env_open(impl->env, idx_dir.c_str(),
                         MDB_NOSYNC | MDB_WRITEMAP | MDB_NOTLS, 0644),
            "env_open");
  MDB_txn *txn;
  mdb_check(mdb_txn_begin(impl->env, nullptr, 0, &txn), "txn_begin");
  mdb_check(mdb_dbi_open(txn, "blobs", MDB_CREATE, &impl->blobs), "dbi_open");
  mdb_check(mdb_dbi_open(txn, "meta", MDB_CREATE, &impl->meta), "dbi_open");
  uint64_t gen = impl->read_gen(txn);
  mdb_check(mdb_txn_commit(txn), "txn_commit");

  std::string lock = dir + "/lock";
  impl->lock_fd = ::open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (impl->lock_fd < 0)
    sys_err("open " + lock);

  if (flock(impl->lock_fd, LOCK_EX | LOCK_NB) == 0) {
    impl->cleanup_orphans(gen);
    flock(impl->lock_fd, LOCK_UN);
  }
  if (!impl->mapping_for(gen, true))
    throw std::runtime_error("cannot map pack");

  return std::unique_ptr<PackCas>(new PackCas(std::move(impl)));
}

std::string PackCas::put(std::string_view data) {
  std::string hash = blake3_hash(data);
  auto &i = *impl_;
  if (!i.wtxn)
    i.begin_batch();
  uint64_t off, len;
  if (i.lookup(hash, off, len, nullptr))
    return hash;
  auto m = i.current();
  size_t woff = 0;
  while (woff < data.size()) {
    ssize_t n = pwrite(m->fd, data.data() + woff, data.size() - woff,
                       static_cast<off_t>(i.end + woff));
    if (n < 0)
      sys_err("pwrite pack");
    woff += static_cast<size_t>(n);
  }
  IndexEntry e{i.end, data.size()};
  MDB_val k = to_val(hash), v{sizeof(e), &e};
  mdb_check(mdb_put(i.wtxn, i.blobs, &k, &v, MDB_NOOVERWRITE), "mdb_put");
  i.end += data.size();
  i.batch_bytes += data.size();
  if (i.batch_bytes >= kAutoSyncBytes)
    sync();
  return hash;
}

// pread: immune to truncation, unlike the mmap
bool PackCas::get(std::string_view hash, std::string &out) {
  auto &i = *impl_;
  for (int attempt = 0; attempt < 8; attempt++) {
    uint64_t off, len, gen;
    if (!i.lookup(hash, off, len, &gen))
      return false;
    auto m = i.mapping_for(gen, i.wtxn != nullptr);
    if (!m)
      continue;
    if (off + len < off)
      throw CorruptError("pack index out of bounds");
    out.resize(len);
    size_t got = 0;
    while (got < len) {
      ssize_t n = pread(m->fd, out.data() + got, len - got,
                        static_cast<off_t>(off + got));
      if (n < 0)
        sys_err("pread pack");
      if (n == 0)
        throw CorruptError("pack truncated");
      got += static_cast<size_t>(n);
    }
    return true;
  }
  throw std::runtime_error("pack generation churn");
}

bool PackCas::get_view(std::string_view hash, std::string_view &out) {
  auto &i = *impl_;
  // retry: a compaction can retire the generation between index and map
  for (int attempt = 0; attempt < 8; attempt++) {
    uint64_t off, len, gen;
    if (!i.lookup(hash, off, len, &gen))
      return false;
    auto m = i.mapping_for(gen, i.wtxn != nullptr);
    if (!m)
      continue;
    if (off + len < off || off + len > kMapSize)
      throw CorruptError("pack index out of bounds");
    uint64_t known = m->size;
    if (off + len > known) {
      struct stat st;
      if (fstat(m->fd, &st) < 0)
        sys_err("fstat pack");
      m->size = known = static_cast<uint64_t>(st.st_size);
      if (off + len > known)
        throw CorruptError("pack index out of bounds");
    }
    out = {m->base + off, len};
    return true;
  }
  throw std::runtime_error("pack generation churn");
}

bool PackCas::has(std::string_view hash) {
  uint64_t off, len;
  return impl_->lookup(hash, off, len, nullptr);
}

void PackCas::sync() {
  if (impl_->wtxn)
    impl_->commit_batch();
}

uint64_t PackCas::pack_size() {
  auto m = impl_->current();
  struct stat st;
  if (fstat(m->fd, &st) < 0)
    sys_err("fstat pack");
  return static_cast<uint64_t>(st.st_size);
}

void PackCas::compact(const std::function<bool(std::string_view)> &live) {
  auto &i = *impl_;
  sync();
  i.lock_writer();
  int nfd = -1;
  std::string npath;
  try {
    WriteTxn txn(i.env, nullptr);
    uint64_t gen = i.read_gen(txn.get());
    auto m = i.mapping_for(gen, true);
    struct stat st;
    if (fstat(m->fd, &st) < 0)
      sys_err("fstat pack");
    uint64_t psize = static_cast<uint64_t>(st.st_size);
    m->size = psize;

    npath = i.pack_path(gen + 1);
    nfd = ::open(npath.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (nfd < 0)
      sys_err("open " + npath);

    MDB_cursor *c;
    mdb_check(mdb_cursor_open(txn.get(), i.blobs, &c), "cursor_open");
    std::vector<std::pair<std::string, IndexEntry>> keep;
    std::string buf;
    buf.reserve(4 << 20);
    uint64_t noff = 0;
    MDB_val k, v;
    int rc;
    while ((rc = mdb_cursor_get(c, &k, &v, MDB_NEXT)) == MDB_SUCCESS) {
      if (v.mv_size != sizeof(IndexEntry))
        throw CorruptError("corrupt index entry");
      IndexEntry e;
      memcpy(&e, v.mv_data, sizeof(e));
      std::string_view hash{static_cast<char *>(k.mv_data), k.mv_size};
      if (!live(hash))
        continue;
      if (e.off + e.len < e.off || e.off + e.len > psize)
        throw CorruptError("pack index out of bounds");
      buf.append(m->base + e.off, e.len);
      keep.emplace_back(std::string(hash), IndexEntry{noff, e.len});
      noff += e.len;
      if (buf.size() >= (4 << 20)) {
        write_all(nfd, buf);
        buf.clear();
      }
    }
    if (rc != MDB_NOTFOUND)
      mdb_check(rc, "cursor_get");
    mdb_cursor_close(c);
    write_all(nfd, buf);
    if (fdatasync(nfd) < 0)
      sys_err("fdatasync new pack");

    mdb_check(mdb_drop(txn.get(), i.blobs, 0), "mdb_drop");
    for (auto &[hash, e] : keep) {
      MDB_val kk = to_val(hash), vv{sizeof(e), &e};
      mdb_check(mdb_put(txn.get(), i.blobs, &kk, &vv, 0), "mdb_put");
    }
    i.write_gen(txn.get(), gen + 1);
    txn.commit();
    mdb_check(mdb_env_sync(i.env, 1), "env_sync");

    close(nfd);
    nfd = -1;
    unlink(i.pack_path(gen).c_str());
    i.mapping_for(gen + 1, true);
    i.committed_end = i.end = noff;
    flock(i.lock_fd, LOCK_UN);
  } catch (...) {
    if (nfd >= 0) {
      close(nfd);
      unlink(npath.c_str());
    }
    flock(i.lock_fd, LOCK_UN);
    throw;
  }
}

void PackCas::meta_put(std::string_view key, std::string_view val) {
  auto &i = *impl_;
  WriteTxn txn(i.env, i.wtxn);
  MDB_val k = to_val(key), v = to_val(val);
  mdb_check(mdb_put(txn.get(), i.meta, &k, &v, 0), "mdb_put meta");
  txn.commit();
}

bool PackCas::meta_get(std::string_view key, std::string &out) {
  auto &i = *impl_;
  ReadTxn txn(i.env, i.wtxn);
  MDB_val k = to_val(key), v;
  int rc = mdb_get(txn.get(), i.meta, &k, &v);
  if (rc == MDB_NOTFOUND)
    return false;
  mdb_check(rc, "mdb_get meta");
  out.assign(static_cast<char *>(v.mv_data), v.mv_size);
  return true;
}

void PackCas::meta_del(std::string_view key) {
  auto &i = *impl_;
  WriteTxn txn(i.env, i.wtxn);
  MDB_val k = to_val(key);
  int rc = mdb_del(txn.get(), i.meta, &k, nullptr);
  if (rc != MDB_NOTFOUND)
    mdb_check(rc, "mdb_del meta");
  txn.commit();
}

void PackCas::meta_scan(
    std::string_view prefix,
    const std::function<bool(std::string_view, std::string_view)> &cb) {
  auto &i = *impl_;
  ReadTxn txn(i.env, i.wtxn);
  MDB_cursor *c;
  mdb_check(mdb_cursor_open(txn.get(), i.meta, &c), "cursor_open");
  MDB_val k = to_val(prefix), v;
  int rc = mdb_cursor_get(c, &k, &v, MDB_SET_RANGE);
  while (rc == MDB_SUCCESS) {
    std::string_view key{static_cast<char *>(k.mv_data), k.mv_size};
    if (!key.starts_with(prefix))
      break;
    if (!cb(key, {static_cast<char *>(v.mv_data), v.mv_size}))
      break;
    rc = mdb_cursor_get(c, &k, &v, MDB_NEXT);
  }
  mdb_cursor_close(c);
  if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND)
    mdb_check(rc, "cursor_get");
}
