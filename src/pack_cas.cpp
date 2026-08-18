#include "pack_cas.hpp"

#include <blake3.h>
#include <fcntl.h>
#include <lmdb.h>
#include <lz4.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

auto blake3_hash(std::string_view data) -> std::string {
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, data.data(), data.size());
  std::array<uint8_t, BLAKE3_OUT_LEN> digest{};
  blake3_hasher_finalize(&hasher, digest.data(), digest.size());
  return {digest.begin(), digest.end()};
}

namespace {

constexpr size_t kMapSize = 1UL << 40;
constexpr size_t kIndexMapSize = 32UL << 30;
constexpr uint64_t kAutoSyncBytes = 256UL << 20;
constexpr size_t kCompactChunk = 4UL << 20;
constexpr size_t kHashLen = 32;
constexpr unsigned kNumDbs = 2;
constexpr mode_t kFileMode = 0644;
constexpr mode_t kDirMode = 0755;
constexpr auto kLockTimeout = std::chrono::minutes(10);
constexpr auto kLockPoll = std::chrono::milliseconds(50);
// a compaction can retire a generation between index lookup and mapping
constexpr int kMaxGenerationRetries = 8;
constexpr size_t kMinCompressSize = 64;
constexpr std::string_view kPackPrefix = "pack.";
// mismatch wipes the cache; bump when IndexEntry or the pack encoding changes
constexpr std::string_view kFormat = "2";

[[noreturn]] void sys_err(const std::string &what) {
  throw std::system_error(errno, std::generic_category(), what);
}

void mdb_check(int res, const char *what) {
  if (res != MDB_SUCCESS) {
    throw std::runtime_error(std::string(what) + ": " + mdb_strerror(res));
  }
}

struct IndexEntry {
  uint64_t off;
  uint64_t len;
  uint64_t raw_len; // == len means stored raw

  [[nodiscard]] auto compressed() const -> bool { return raw_len != len; }
};
static_assert(sizeof(IndexEntry) == 3 * sizeof(uint64_t));

auto decode_entry(const MDB_val &val) -> IndexEntry {
  if (val.mv_size != sizeof(IndexEntry)) {
    throw CorruptError("corrupt index entry");
  }
  IndexEntry entry{};
  memcpy(&entry, val.mv_data, sizeof(entry));
  if (entry.off + entry.len < entry.off) {
    throw CorruptError("pack index out of bounds");
  }
  return entry;
}

void decompress(std::string_view stored, uint64_t raw_len, std::string &out) {
  if (raw_len > static_cast<uint64_t>(LZ4_MAX_INPUT_SIZE) ||
      stored.size() > static_cast<size_t>(INT32_MAX)) {
    throw CorruptError("corrupt index entry: blob too large");
  }
  out.resize(raw_len);
  const int produced = LZ4_decompress_safe(stored.data(), out.data(),
                                           static_cast<int>(stored.size()),
                                           static_cast<int>(raw_len));
  if (produced < 0 || std::cmp_not_equal(produced, raw_len)) {
    throw CorruptError("corrupt compressed blob");
  }
}

// aborts the txn unless it is the caller-owned write txn
class ReadTxn {
public:
  ReadTxn(MDB_env *env, MDB_txn *wtxn) : owned_(wtxn == nullptr), txn_(wtxn) {
    if (owned_) {
      mdb_check(mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn_), "txn_begin");
    }
  }
  ~ReadTxn() {
    if (owned_) {
      mdb_txn_abort(txn_);
    }
  }
  ReadTxn(const ReadTxn &) = delete;
  ReadTxn(ReadTxn &&) = delete;
  auto operator=(const ReadTxn &) -> ReadTxn & = delete;
  auto operator=(ReadTxn &&) -> ReadTxn & = delete;
  [[nodiscard]] auto get() const -> MDB_txn * { return txn_; }

private:
  bool owned_;
  MDB_txn *txn_;
};

// commits or aborts an owned write txn; joins an open batch txn instead
class WriteTxn {
public:
  WriteTxn(MDB_env *env, MDB_txn *wtxn) : owned_(wtxn == nullptr), txn_(wtxn) {
    if (owned_) {
      mdb_check(mdb_txn_begin(env, nullptr, 0, &txn_), "txn_begin");
    }
  }
  ~WriteTxn() {
    if (owned_ && txn_ != nullptr) {
      mdb_txn_abort(txn_);
    }
  }
  WriteTxn(const WriteTxn &) = delete;
  WriteTxn(WriteTxn &&) = delete;
  auto operator=(const WriteTxn &) -> WriteTxn & = delete;
  auto operator=(WriteTxn &&) -> WriteTxn & = delete;
  [[nodiscard]] auto get() const -> MDB_txn * { return txn_; }
  void commit() {
    if (owned_) {
      MDB_txn *txn = txn_;
      txn_ = nullptr;
      mdb_check(mdb_txn_commit(txn), "txn_commit");
    }
  }

private:
  bool owned_;
  MDB_txn *txn_;
};

} // namespace

namespace pack_cas_detail {

struct Mapping {
  char *base = static_cast<char *>(MAP_FAILED);
  int pack_fd = -1;
  uint64_t gen = 0;
  std::atomic<uint64_t> size{0};

  Mapping() = default;
  Mapping(const Mapping &) = delete;
  Mapping(Mapping &&) = delete;
  auto operator=(const Mapping &) -> Mapping & = delete;
  auto operator=(Mapping &&) -> Mapping & = delete;
  ~Mapping() {
    if (base != MAP_FAILED) {
      munmap(base, kMapSize);
    }
    if (pack_fd >= 0) {
      close(pack_fd);
    }
  }

  // bytes known to be committed; refreshes from the file once if needed
  auto view(uint64_t off, uint64_t len) -> std::string_view {
    if (off + len > kMapSize) {
      throw CorruptError("pack index out of bounds");
    }
    uint64_t known = size;
    if (off + len > known) {
      struct stat info{};
      if (fstat(pack_fd, &info) < 0) {
        sys_err("fstat pack");
      }
      known = static_cast<uint64_t>(info.st_size);
      size = known;
      if (off + len > known) {
        throw CorruptError("pack index out of bounds");
      }
    }
    return std::string_view(base, known).substr(off, len);
  }

  [[nodiscard]] auto file_size() const -> uint64_t {
    struct stat info{};
    if (fstat(pack_fd, &info) < 0) {
      sys_err("fstat pack");
    }
    return static_cast<uint64_t>(info.st_size);
  }
};

} // namespace pack_cas_detail

namespace {

using pack_cas_detail::Mapping;

void write_all(int out_fd, std::string_view data) {
  while (!data.empty()) {
    const ssize_t written = write(out_fd, data.data(), data.size());
    if (written < 0) {
      sys_err("write pack");
    }
    data.remove_prefix(static_cast<size_t>(written));
  }
}

void pwrite_all(int out_fd, std::string_view data, uint64_t off) {
  while (!data.empty()) {
    const ssize_t written =
        pwrite(out_fd, data.data(), data.size(), static_cast<off_t>(off));
    if (written < 0) {
      sys_err("pwrite pack");
    }
    data.remove_prefix(static_cast<size_t>(written));
    off += static_cast<uint64_t>(written);
  }
}

void pread_all(int in_fd, std::span<char> dst, uint64_t off) {
  while (!dst.empty()) {
    const ssize_t got =
        pread(in_fd, dst.data(), dst.size(), static_cast<off_t>(off));
    if (got < 0) {
      sys_err("pread pack");
    }
    if (got == 0) {
      throw CorruptError("pack truncated");
    }
    dst = dst.subspan(static_cast<size_t>(got));
    off += static_cast<uint64_t>(got);
  }
}

// LMDB takes non-const pointers even for reads
auto to_val(std::string_view str) -> MDB_val {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  return {str.size(), const_cast<char *>(str.data())};
}

auto to_view(const MDB_val &val) -> std::string_view {
  return {static_cast<const char *>(val.mv_data), val.mv_size};
}

auto open_file(const std::string &path, int flags) -> int {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open(2) is variadic
  return ::open(path.c_str(), flags, kFileMode);
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

  std::string compress_buf;

  Impl() = default;
  Impl(const Impl &) = delete;
  Impl(Impl &&) = delete;
  auto operator=(const Impl &) -> Impl & = delete;
  auto operator=(Impl &&) -> Impl & = delete;
  ~Impl() {
    if (wtxn != nullptr) {
      abort_batch();
    }
    retired.clear();
    cur.reset();
    if (lock_fd >= 0) {
      close(lock_fd);
    }
    if (env != nullptr) {
      mdb_env_close(env);
    }
  }

  [[nodiscard]] auto pack_path(uint64_t gen) const -> std::string {
    return dir + "/" + std::string(kPackPrefix) + std::to_string(gen);
  }

  [[nodiscard]] auto read_gen(MDB_txn *txn) const -> uint64_t {
    MDB_val key = to_val("gen");
    MDB_val val;
    const int res = mdb_get(txn, meta, &key, &val);
    if (res == MDB_NOTFOUND) {
      return 0;
    }
    mdb_check(res, "mdb_get gen");
    uint64_t gen = 0;
    if (val.mv_size != sizeof(gen)) {
      throw CorruptError("corrupt generation record");
    }
    memcpy(&gen, val.mv_data, sizeof(gen));
    return gen;
  }

  void write_gen(MDB_txn *txn, uint64_t gen) const {
    MDB_val key = to_val("gen");
    MDB_val val{sizeof(gen), &gen};
    mdb_check(mdb_put(txn, meta, &key, &val, 0), "mdb_put gen");
  }

  [[nodiscard]] auto map_pack(uint64_t gen, bool create) const
      -> std::shared_ptr<Mapping> {
    auto mapping = std::make_shared<Mapping>();
    mapping->gen = gen;
    const int flags = O_RDWR | O_CLOEXEC | (create ? O_CREAT : 0);
    mapping->pack_fd = open_file(pack_path(gen), flags);
    if (mapping->pack_fd < 0) {
      if (!create && errno == ENOENT) {
        return nullptr;
      }
      sys_err("open " + pack_path(gen));
    }
    mapping->size = mapping->file_size();
    void *base =
        mmap(nullptr, kMapSize, PROT_READ, MAP_SHARED, mapping->pack_fd, 0);
    if (base == MAP_FAILED) {
      sys_err("mmap pack");
    }
    mapping->base = static_cast<char *>(base);
    return mapping;
  }

  // never unmaps: in-flight views into old generations must stay valid
  auto mapping_for(uint64_t gen, bool create) -> std::shared_ptr<Mapping> {
    const std::scoped_lock lock(map_mu);
    if (cur && cur->gen == gen) {
      return cur;
    }
    for (auto &old : retired) {
      if (old->gen == gen) {
        return old;
      }
    }
    auto mapping = map_pack(gen, create);
    if (!mapping) {
      return nullptr;
    }
    if (!cur || gen > cur->gen) {
      if (cur) {
        retired.push_back(cur);
      }
      cur = mapping;
    } else {
      retired.push_back(mapping);
    }
    return mapping;
  }

  auto current() -> std::shared_ptr<Mapping> {
    const std::scoped_lock lock(map_mu);
    return cur;
  }

  void lock_writer() const {
    for (std::chrono::milliseconds waited{0};; waited += kLockPoll) {
      if (flock(lock_fd, LOCK_EX | LOCK_NB) == 0) {
        return;
      }
      if (errno != EWOULDBLOCK) {
        sys_err("flock");
      }
      if (waited >= kLockTimeout) {
        throw std::runtime_error("timed out waiting for the pack writer "
                                 "lock; is another process stuck?");
      }
      std::this_thread::sleep_for(kLockPoll);
    }
  }

  void unlock_writer() const { flock(lock_fd, LOCK_UN); }

  void begin_batch() {
    lock_writer();
    try {
      mdb_check(mdb_txn_begin(env, nullptr, 0, &wtxn), "txn_begin");
      auto mapping = mapping_for(read_gen(wtxn), true);
      end = committed_end = mapping->file_size();
      mapping->size = end;
      batch_bytes = 0;
    } catch (...) {
      if (wtxn != nullptr) {
        mdb_txn_abort(wtxn);
        wtxn = nullptr;
      }
      unlock_writer();
      throw;
    }
  }

  void commit_batch() {
    auto mapping = current();
    if (fdatasync(mapping->pack_fd) < 0) {
      sys_err("fdatasync pack");
    }
    mdb_check(mdb_txn_commit(wtxn), "txn_commit");
    wtxn = nullptr;
    mdb_check(mdb_env_sync(env, 1), "env_sync");
    committed_end = end;
    mapping->size = end;
    unlock_writer();
  }

  void abort_batch() {
    mdb_txn_abort(wtxn);
    wtxn = nullptr;
    // drop appended-but-uncommitted bytes; we still hold the flock
    auto mapping = current();
    if (ftruncate(mapping->pack_fd, static_cast<off_t>(committed_end)) == 0) {
      end = committed_end;
    }
    mapping->size = end;
    unlock_writer();
  }

  auto lookup(std::string_view hash, IndexEntry &entry, uint64_t *gen_out) const
      -> bool {
    if (hash.size() != kHashLen) {
      throw std::invalid_argument("bad hash length");
    }
    const ReadTxn txn(env, wtxn);
    if (gen_out != nullptr) {
      *gen_out = read_gen(txn.get());
    }
    MDB_val key = to_val(hash);
    MDB_val val;
    const int res = mdb_get(txn.get(), blobs, &key, &val);
    if (res == MDB_NOTFOUND) {
      return false;
    }
    mdb_check(res, "mdb_get");
    entry = decode_entry(val);
    return true;
  }

  // entry and its mapping from a consistent generation
  auto locate(std::string_view hash, IndexEntry &entry,
              std::shared_ptr<Mapping> &mapping) -> bool {
    for (int attempt = 0; attempt < kMaxGenerationRetries; attempt++) {
      uint64_t gen = 0;
      if (!lookup(hash, entry, &gen)) {
        return false;
      }
      mapping = mapping_for(gen, wtxn != nullptr);
      if (mapping) {
        return true;
      }
    }
    throw std::runtime_error("pack generation churn");
  }

  void check_format(MDB_txn *txn) const {
    MDB_val key = to_val("format");
    MDB_val val;
    const int res = mdb_get(txn, meta, &key, &val);
    if (res == MDB_SUCCESS) {
      if (to_view(val) != kFormat) {
        throw CorruptError("unsupported cache format");
      }
      return;
    }
    if (res != MDB_NOTFOUND) {
      mdb_check(res, "mdb_get format");
    }
    MDB_stat stats;
    mdb_check(mdb_stat(txn, blobs, &stats), "mdb_stat");
    if (stats.ms_entries != 0) {
      throw CorruptError("cache predates format versioning");
    }
    val = to_val(kFormat);
    mdb_check(mdb_put(txn, meta, &key, &val, 0), "mdb_put format");
  }

  // only safe with the writer lock
  void cleanup_orphans(uint64_t live_gen) const {
    namespace fs = std::filesystem;
    for (const auto &entry : fs::directory_iterator(dir)) {
      const auto name = entry.path().filename().string();
      if (!name.starts_with(kPackPrefix)) {
        continue;
      }
      const auto num = std::string_view(name).substr(kPackPrefix.size());
      uint64_t gen = 0;
      const char *num_end = std::to_address(num.end());
      auto [last, err] =
          std::from_chars(std::to_address(num.begin()), num_end, gen);
      if (err != std::errc{} || last != num_end) {
        continue;
      }
      if (gen != live_gen) {
        unlink(entry.path().c_str());
      }
    }
  }
};

PackCas::PackCas(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

PackCas::~PackCas() = default;

auto PackCas::open(const std::string &dir) -> std::unique_ptr<PackCas> {
  try {
    return open_once(dir);
  } catch (const std::exception &) {
    std::error_code err;
    std::filesystem::remove_all(dir, err);
    return open_once(dir);
  }
}

auto PackCas::open_once(const std::string &dir) -> std::unique_ptr<PackCas> {
  auto impl = std::make_unique<Impl>();
  impl->dir = dir;
  std::filesystem::create_directories(dir);
  const std::string idx_dir = dir + "/index";
  if (mkdir(idx_dir.c_str(), kDirMode) < 0 && errno != EEXIST) {
    sys_err("mkdir " + idx_dir);
  }

  mdb_check(mdb_env_create(&impl->env), "env_create");
  mdb_check(mdb_env_set_mapsize(impl->env, kIndexMapSize), "set_mapsize");
  mdb_check(mdb_env_set_maxdbs(impl->env, kNumDbs), "set_maxdbs");
  mdb_check(mdb_env_open(impl->env, idx_dir.c_str(),
                         MDB_NOSYNC | MDB_WRITEMAP | MDB_NOTLS, kFileMode),
            "env_open");
  MDB_txn *txn = nullptr;
  mdb_check(mdb_txn_begin(impl->env, nullptr, 0, &txn), "txn_begin");
  uint64_t gen = 0;
  try {
    mdb_check(mdb_dbi_open(txn, "blobs", MDB_CREATE, &impl->blobs), "dbi_open");
    mdb_check(mdb_dbi_open(txn, "meta", MDB_CREATE, &impl->meta), "dbi_open");
    impl->check_format(txn);
    gen = impl->read_gen(txn);
  } catch (...) {
    mdb_txn_abort(txn);
    throw;
  }
  mdb_check(mdb_txn_commit(txn), "txn_commit");

  const std::string lock = dir + "/lock";
  impl->lock_fd = open_file(lock, O_RDWR | O_CREAT | O_CLOEXEC);
  if (impl->lock_fd < 0) {
    sys_err("open " + lock);
  }

  if (flock(impl->lock_fd, LOCK_EX | LOCK_NB) == 0) {
    impl->cleanup_orphans(gen);
    impl->unlock_writer();
  }
  if (!impl->mapping_for(gen, true)) {
    throw std::runtime_error("cannot map pack");
  }

  return std::unique_ptr<PackCas>(new PackCas(std::move(impl)));
}

auto PackCas::put(std::string_view data) -> std::string {
  std::string hash = blake3_hash(data);
  auto &impl = *impl_;
  if (impl.wtxn == nullptr) {
    impl.begin_batch();
  }
  IndexEntry entry{};
  if (impl.lookup(hash, entry, nullptr)) {
    return hash;
  }
  std::string_view stored = data;
  std::string &cbuf = impl.compress_buf;
  if (data.size() >= kMinCompressSize &&
      data.size() <= static_cast<size_t>(LZ4_MAX_INPUT_SIZE)) {
    // capacity < input: LZ4 fails instead of producing a larger blob
    cbuf.resize(data.size() - 1);
    const int packed = LZ4_compress_default(data.data(), cbuf.data(),
                                            static_cast<int>(data.size()),
                                            static_cast<int>(cbuf.size()));
    if (packed > 0) {
      stored = {cbuf.data(), static_cast<size_t>(packed)};
    }
  }
  pwrite_all(impl.current()->pack_fd, stored, impl.end);
  entry = {impl.end, stored.size(), data.size()};
  MDB_val key = to_val(hash);
  MDB_val val{sizeof(entry), &entry};
  mdb_check(mdb_put(impl.wtxn, impl.blobs, &key, &val, MDB_NOOVERWRITE),
            "mdb_put");
  impl.end += stored.size();
  impl.batch_bytes += stored.size();
  if (impl.batch_bytes >= kAutoSyncBytes) {
    sync();
  }
  return hash;
}

// pread: immune to truncation, unlike the mmap
auto PackCas::get(std::string_view hash, std::string &out) -> bool {
  IndexEntry entry{};
  std::shared_ptr<Mapping> mapping;
  if (!impl_->locate(hash, entry, mapping)) {
    return false;
  }
  if (!entry.compressed()) {
    out.resize(entry.len);
    pread_all(mapping->pack_fd, out, entry.off);
    return true;
  }
  std::string stored(entry.len, '\0');
  pread_all(mapping->pack_fd, stored, entry.off);
  decompress(stored, entry.raw_len, out);
  return true;
}

auto PackCas::get_view(std::string_view hash, std::string &scratch,
                       std::string_view &out) -> bool {
  IndexEntry entry{};
  std::shared_ptr<Mapping> mapping;
  if (!impl_->locate(hash, entry, mapping)) {
    return false;
  }
  const std::string_view stored = mapping->view(entry.off, entry.len);
  if (!entry.compressed()) {
    out = stored;
    return true;
  }
  decompress(stored, entry.raw_len, scratch);
  out = scratch;
  return true;
}

auto PackCas::size(std::string_view hash, uint64_t &out) -> bool {
  IndexEntry entry{};
  if (!impl_->lookup(hash, entry, nullptr)) {
    return false;
  }
  out = entry.raw_len;
  return true;
}

auto PackCas::has(std::string_view hash) -> bool {
  IndexEntry entry{};
  return impl_->lookup(hash, entry, nullptr);
}

void PackCas::sync() {
  if (impl_->wtxn != nullptr) {
    impl_->commit_batch();
  }
}

auto PackCas::pack_size() -> uint64_t { return impl_->current()->file_size(); }

void PackCas::compact(const std::function<bool(std::string_view)> &live) {
  auto &impl = *impl_;
  sync();
  impl.lock_writer();
  int new_fd = -1;
  std::string new_path;
  try {
    WriteTxn txn(impl.env, nullptr);
    const uint64_t gen = impl.read_gen(txn.get());
    auto mapping = impl.mapping_for(gen, true);
    mapping->size = mapping->file_size();

    new_path = impl.pack_path(gen + 1);
    new_fd = open_file(new_path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC);
    if (new_fd < 0) {
      sys_err("open " + new_path);
    }

    MDB_cursor *cursor = nullptr;
    mdb_check(mdb_cursor_open(txn.get(), impl.blobs, &cursor), "cursor_open");
    std::vector<std::pair<std::string, IndexEntry>> keep;
    std::string buf;
    buf.reserve(kCompactChunk);
    uint64_t new_off = 0;
    MDB_val key;
    MDB_val val;
    int res = 0;
    while ((res = mdb_cursor_get(cursor, &key, &val, MDB_NEXT)) ==
           MDB_SUCCESS) {
      const IndexEntry entry = decode_entry(val);
      const std::string_view hash = to_view(key);
      if (!live(hash)) {
        continue;
      }
      buf += mapping->view(entry.off, entry.len);
      keep.emplace_back(hash, IndexEntry{new_off, entry.len, entry.raw_len});
      new_off += entry.len;
      if (buf.size() >= kCompactChunk) {
        write_all(new_fd, buf);
        buf.clear();
      }
    }
    if (res != MDB_NOTFOUND) {
      mdb_check(res, "cursor_get");
    }
    mdb_cursor_close(cursor);
    write_all(new_fd, buf);
    if (fdatasync(new_fd) < 0) {
      sys_err("fdatasync new pack");
    }

    mdb_check(mdb_drop(txn.get(), impl.blobs, 0), "mdb_drop");
    for (auto &[hash, entry] : keep) {
      MDB_val new_key = to_val(hash);
      MDB_val new_val{sizeof(entry), &entry};
      mdb_check(mdb_put(txn.get(), impl.blobs, &new_key, &new_val, 0),
                "mdb_put");
    }
    impl.write_gen(txn.get(), gen + 1);
    txn.commit();
    mdb_check(mdb_env_sync(impl.env, 1), "env_sync");

    close(new_fd);
    new_fd = -1;
    unlink(impl.pack_path(gen).c_str());
    impl.mapping_for(gen + 1, true);
    impl.committed_end = impl.end = new_off;
    impl.unlock_writer();
  } catch (...) {
    if (new_fd >= 0) {
      close(new_fd);
      unlink(new_path.c_str());
    }
    impl.unlock_writer();
    throw;
  }
}

void PackCas::meta_put(std::string_view key, std::string_view val) {
  auto &impl = *impl_;
  WriteTxn txn(impl.env, impl.wtxn);
  MDB_val mkey = to_val(key);
  MDB_val mval = to_val(val);
  mdb_check(mdb_put(txn.get(), impl.meta, &mkey, &mval, 0), "mdb_put meta");
  txn.commit();
}

auto PackCas::meta_get(std::string_view key, std::string &out) -> bool {
  auto &impl = *impl_;
  const ReadTxn txn(impl.env, impl.wtxn);
  MDB_val mkey = to_val(key);
  MDB_val mval;
  const int res = mdb_get(txn.get(), impl.meta, &mkey, &mval);
  if (res == MDB_NOTFOUND) {
    return false;
  }
  mdb_check(res, "mdb_get meta");
  out = to_view(mval);
  return true;
}

void PackCas::meta_del(std::string_view key) {
  auto &impl = *impl_;
  WriteTxn txn(impl.env, impl.wtxn);
  MDB_val mkey = to_val(key);
  const int res = mdb_del(txn.get(), impl.meta, &mkey, nullptr);
  if (res != MDB_NOTFOUND) {
    mdb_check(res, "mdb_del meta");
  }
  txn.commit();
}

void PackCas::meta_scan(
    std::string_view prefix,
    const std::function<bool(std::string_view, std::string_view)> &callback) {
  auto &impl = *impl_;
  const ReadTxn txn(impl.env, impl.wtxn);
  MDB_cursor *cursor = nullptr;
  mdb_check(mdb_cursor_open(txn.get(), impl.meta, &cursor), "cursor_open");
  MDB_val mkey = to_val(prefix);
  MDB_val mval;
  int res = mdb_cursor_get(cursor, &mkey, &mval, MDB_SET_RANGE);
  while (res == MDB_SUCCESS) {
    const std::string_view key = to_view(mkey);
    if (!key.starts_with(prefix) || !callback(key, to_view(mval))) {
      break;
    }
    res = mdb_cursor_get(cursor, &mkey, &mval, MDB_NEXT);
  }
  mdb_cursor_close(cursor);
  if (res != MDB_SUCCESS && res != MDB_NOTFOUND) {
    mdb_check(res, "cursor_get");
  }
}
