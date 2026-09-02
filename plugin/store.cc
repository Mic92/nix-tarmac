// Persistent Nix store backed by the pack CAS, registered as the
// "tarmac" URI scheme. Meant as an --eval-store. Unlike dummy:// it
// survives evaluator restarts, so store objects are written only once.
// The store API churned before 2.35, older versions only get the
// fetcher.
#include "nix_compat.hh"
#if TARMAC_HAVE_STORE

#include "ctx.hh"
#include "pack_accessor.hh"
#include "pack_cas.hpp"
#include "tree.hpp"

#include <mutex>

#include <nix/store/content-address.hh>
#include <nix/store/derivations.hh>
#include <nix/store/globals.hh>
#include <nix/store/keys.hh>
#include <nix/store/path-info.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-registration.hh>
#include <nix/util/archive.hh>
#include <nix/util/callback.hh>
#include <nix/util/canon-path.hh>
#include <nix/util/error.hh>
#include <nix/util/hash.hh>
#include <nix/util/memory-source-accessor.hh>
#include <nix/util/ref.hh>
#include <nix/util/serialise.hh>
#include <nix/util/source-accessor.hh>
#include <nix/util/users.hh>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using nix_compat::drvReferences;
using nix_compat::parseDrv;
using nix_compat::unparseDrv;

constexpr std::string_view kPathPrefix = "sp:";
constexpr std::string_view kDrvPrefix = "sd:";

// what queryPathInfo needs, keyed by store path basename in the meta
// table. The file system objects live in the CAS behind root_id.
struct PathRecord {
  // 'd' tree id, 'r'/'x'/'s' blob id
  char root_type = 'd';
  std::string root_id;
  std::string nar_hash;
  uint64_t nar_size = 0;
  std::string ca;
  std::vector<std::string> refs;

  [[nodiscard]] auto encode() const -> std::string {
    return nlohmann::json{
        {"t", std::string(1, root_type)},
        {"root", to_hex(root_id)},
        {"narHash", nar_hash},
        {"narSize", nar_size},
        {"ca", ca},
        {"refs", refs},
    }
        .dump();
  }

  static auto decode(std::string_view data) -> PathRecord {
    const auto json = nlohmann::json::parse(data);
    PathRecord rec;
    rec.root_type = json.at("t").get<std::string>().at(0);
    rec.root_id = from_hex(json.at("root").get<std::string>());
    rec.nar_hash = json.at("narHash").get<std::string>();
    rec.nar_size = json.at("narSize").get<uint64_t>();
    rec.ca = json.at("ca").get<std::string>();
    rec.refs = json.at("refs").get<std::vector<std::string>>();
    return rec;
  }
};

// stores a memory file system object, returning type and id
auto ingest(TreeStore &tree, const nix::MemorySourceAccessor::File &file)
    -> std::pair<char, std::string> {
  using File = nix::MemorySourceAccessor::File;
  using Stored = std::pair<char, std::string>;
  return std::visit(nix::overloaded{
                        [&](const File::Regular &reg) -> Stored {
                          return {reg.executable ? 'x' : 'r',
                                  tree.putBlob(reg.contents)};
                        },
                        [&](const File::Symlink &link) -> Stored {
                          return {'s', tree.putBlob(link.target)};
                        },
                        [&](const File::Directory &dir) -> Stored {
                          std::vector<TreeEntry> entries;
                          entries.reserve(dir.entries.size());
                          for (const auto &[name, child] : dir.entries) {
                            auto [type, id] = ingest(tree, child);
                            entries.push_back({name, type, std::move(id)});
                          }
                          return {'d', tree.putTree(entries)};
                        },
                    },
                    file.raw);
}

// parses a NAR (or flat file) dump into memory
auto parseToMemory(nix::Source &source, nix::FileSerialisationMethod method)
    -> nix::ref<nix::MemorySourceAccessor> {
  auto accessor = nix::make_ref<nix::MemorySourceAccessor>();
  nix::MemorySink sink{*accessor};
  switch (method) {
  case nix::FileSerialisationMethod::NixArchive:
    parseDump(sink, source);
    break;
  case nix::FileSerialisationMethod::Flat:
    accessor->root = nix::MemorySourceAccessor::File::Regular{};
    sink.createRegularFile(nix::CanonPath::root,
                           [&](auto &fileSink) { source.drainInto(fileSink); });
    break;
  }
  return accessor;
}

// a derivation is stored and served as its ATerm text
auto drvAccessor(std::string text) -> nix::ref<nix::MemorySourceAccessor> {
  auto accessor = nix::make_ref<nix::MemorySourceAccessor>();
  accessor->root = nix::MemorySourceAccessor::File::Regular{
      .contents = std::move(text),
  };
  return accessor;
}

// accessor for store objects whose root is a single file or symlink
class BlobAccessor : public nix::SourceAccessor {
public:
  BlobAccessor(TreeStore &tree, char type, std::string id)
      : tree_(&tree), type_(type), id_(std::move(id)) {}

  void anchor() override {}

  void readFile(const nix::CanonPath &path, nix::Sink &sink,
                nix::fun<void(uint64_t)> sizeCallback) override {
    if (!path.isRoot() || type_ == 's') {
      throw nix::Error("path '%s' is not a regular file", path);
    }
    std::string scratch;
    std::string_view contents;
    if (!tree_->readBlobView(id_, scratch, contents)) {
      throw nix::Error("missing blob for '%s'", path);
    }
    sizeCallback(contents.size());
    sink(contents);
  }

  auto pathExists(const nix::CanonPath &path) -> bool override {
    return path.isRoot();
  }

  auto maybeLstat(const nix::CanonPath &path) -> std::optional<Stat> override {
    if (!path.isRoot()) {
      return std::nullopt;
    }
    Stat info;
    info.type = tarmac_entry_type(type_);
    if (info.type == tRegular) {
      info.isExecutable = type_ == 'x';
      uint64_t size = 0;
      if (tree_->blobSize(id_, size)) {
        info.fileSize = size;
      }
    }
    return info;
  }

  auto readDirectory(const nix::CanonPath &path) -> DirEntries override {
    throw nix::Error("path '%s' is not a directory", path);
  }

  auto readLink(const nix::CanonPath &path) -> std::string override {
    if (!path.isRoot() || type_ != 's') {
      throw nix::Error("path '%s' is not a symlink", path);
    }
    return tree_->readBlob(id_);
  }

private:
  TreeStore *tree_;
  char type_;
  std::string id_;
};

struct TarmacStoreConfig : std::enable_shared_from_this<TarmacStoreConfig>,
                           virtual nix::StoreConfig {
  std::string dir;

  nix::Setting<bool> requireSigs{
      this, nix::settings.requireSigs, "require-sigs",
      "Whether store paths copied into this store should have a trusted "
      "signature."};

  explicit TarmacStoreConfig(const Params &params)
      : StoreConfig(params, nix::StoreConfig::FilePathType::Unix),
        dir(Ctx::defaultDir()) {
    // the in-memory cache would keep stale misses for paths that are
    // added after being queried
    pathInfoCacheSize = 0;
  }

  TarmacStoreConfig(std::string_view /*scheme*/, std::string_view authorityPath,
                    const Params &params)
      : TarmacStoreConfig(params) {
    if (!authorityPath.empty()) {
      dir = std::string(authorityPath);
    }
  }

  void anchor() override {}

  static auto name() -> std::string { return "Tarmac Store"; }

  static auto doc() -> std::string {
    return "Persistent evaluation store backed by the nix-tarmac pack CAS.";
  }

  static auto uriSchemes() -> nix::StringSet { return {"tarmac"}; }

  auto openStore() const -> nix::ref<nix::Store> override;

  auto getReference() const -> nix::StoreReference override {
    return {
        .variant =
            nix::StoreReference::Specified{
                .scheme = *uriSchemes().begin(),
                .authority = dir,
            },
        .params = getQueryParams(),
    };
  }
};

struct TarmacStore : nix::Store {
  using Config = TarmacStoreConfig;

  nix::ref<const Config> config;
  std::shared_ptr<Ctx> state;
  std::once_flag publicKeysOnce;
  nix::PublicKeys publicKeysCache;

  explicit TarmacStore(nix::ref<const Config> conf)
      : Store{*conf}, config(conf), state(Ctx::get(conf->dir)) {}

  void anchor() override {}

  auto publicKeys() -> const nix::PublicKeys & {
    std::call_once(publicKeysOnce,
                   [this] { publicKeysCache = nix::getDefaultPublicKeys(); });
    return publicKeysCache;
  }

  auto metaKey(std::string_view prefix, const nix::StorePath &path)
      -> std::string {
    std::string key(prefix);
    key += path.to_string();
    return key;
  }

  auto metaGet(std::string_view prefix, const nix::StorePath &path)
      -> std::optional<std::string> {
    std::string out;
    if (!state->store.cas().meta_get(metaKey(prefix, path), out)) {
      return std::nullopt;
    }
    return out;
  }

  void metaPut(std::string_view prefix, const nix::StorePath &path,
               std::string_view val) {
    state->store.cas().meta_put(metaKey(prefix, path), val);
  }

  // GC may have collected the record's objects, drop it then
  auto pathRecord(const nix::StorePath &path) -> std::optional<PathRecord> {
    auto raw = metaGet(kPathPrefix, path);
    if (!raw) {
      return std::nullopt;
    }
    auto rec = PathRecord::decode(*raw);
    if (!state->store.cas().has(rec.root_id)) {
      state->store.cas().meta_del(metaKey(kPathPrefix, path));
      return std::nullopt;
    }
    state->store.touchRoot(rec.root_id, rec.root_type == 'd');
    return rec;
  }

  auto drvText(const nix::StorePath &path) -> std::optional<std::string> {
    auto id = metaGet(kDrvPrefix, path);
    if (!id) {
      return std::nullopt;
    }
    std::string text;
    if (!state->store.cas().get(*id, text)) {
      state->store.cas().meta_del(metaKey(kDrvPrefix, path));
      return std::nullopt;
    }
    state->store.touchRoot(*id, false);
    return text;
  }

  auto drvPathInfo(const nix::StorePath &path, std::string text)
      -> std::shared_ptr<nix::ValidPathInfo> {
    auto narHash = hashPath({drvAccessor(text), nix::CanonPath::root},
                            nix::FileSerialisationMethod::NixArchive,
                            nix::HashAlgorithm::SHA256);
    auto info = std::make_shared<nix::ValidPathInfo>(
        path, nix::UnkeyedValidPathInfo{*this, narHash.hash});
    info->narSize = narHash.numBytesDigested;
    info->ca = nix::ContentAddress{
        .method = nix::ContentAddressMethod::Raw::Text,
        .hash = hashString(nix::HashAlgorithm::SHA256, text),
    };
    // drv paths hash their references in, report them so a copy to
    // another store keeps the path
    info->references = drvReferences(
        parseDrv(*this, std::move(text), nix::Derivation::nameFromPath(path)));
    return info;
  }

  auto pathInfo(const nix::StorePath &path, const PathRecord &rec)
      -> std::shared_ptr<nix::ValidPathInfo> {
    auto info = std::make_shared<nix::ValidPathInfo>(
        path,
        nix::UnkeyedValidPathInfo{*this, nix::Hash::parseSRI(rec.nar_hash)});
    info->narSize = rec.nar_size;
    if (!rec.ca.empty()) {
      info->ca = nix::ContentAddress::parse(rec.ca);
    }
    for (const auto &ref : rec.refs) {
      info->references.insert(nix::StorePath{ref});
    }
    return info;
  }

  void
  queryPathInfoUncached(const nix::StorePath &path,
                        nix::Callback<std::shared_ptr<const nix::ValidPathInfo>>
                            callback) noexcept override {
    try {
      if (path.isDerivation()) {
        if (auto text = drvText(path)) {
          callback(drvPathInfo(path, std::move(*text)));
          return;
        }
      } else if (auto rec = pathRecord(path)) {
        callback(pathInfo(path, *rec));
        return;
      }
      callback(nullptr);
    } catch (...) {
      callback.rethrow();
    }
  }

  auto isValidPathUncached(const nix::StorePath &path) -> bool override {
    return path.isDerivation() ? drvText(path).has_value()
                               : pathRecord(path).has_value();
  }

  auto isTrustedClient() -> std::optional<nix::TrustedFlag> override {
    return nix::Trusted;
  }

  // the base Store calls every substitute untrusted, which leaves an
  // --eval-store no way to fill itself
  auto pathInfoIsUntrusted(const nix::ValidPathInfo &info) -> bool override {
    return config->requireSigs && !info.checkSignatures(*this, publicKeys());
  }

  auto queryPathFromHashPart(const std::string &hashPart)
      -> std::optional<nix::StorePath> override {
    std::optional<nix::StorePath> res;
    for (const auto prefix : {kPathPrefix, kDrvPrefix}) {
      std::string scan(prefix);
      scan += hashPart;
      state->store.cas().meta_scan(
          scan, [&](std::string_view key, std::string_view /*val*/) {
            res = nix::StorePath{key.substr(prefix.size())};
            return false;
          });
      if (res) {
        break;
      }
    }
    return res;
  }

  void rejectRepair(nix::RepairFlag repair) {
    if (repair) {
      throw nix::Error("repairing is not supported for '%s' store",
                       config->getHumanReadableURI());
    }
  }

  void storeObject(const nix::ValidPathInfo &info,
                   const nix::MemorySourceAccessor::File &file) {
    const std::scoped_lock lock(state->mutex);
    auto [type, id] = ingest(state->store, file);
    PathRecord rec;
    rec.root_type = type;
    rec.root_id = std::move(id);
    rec.nar_hash = info.narHash.to_string(nix::HashFormat::SRI, true);
    rec.nar_size = info.narSize;
    rec.ca = renderContentAddress(info.ca);
    for (const auto &ref : info.references) {
      rec.refs.emplace_back(ref.to_string());
    }
    state->store.registerRoot(rec.root_id, rec.root_type == 'd');
    metaPut(kPathPrefix, info.path, rec.encode());
    // commit promptly, the writer flock blocks other eval workers
    state->store.sync();
  }

  void addToStore(const nix::ValidPathInfo &info, nix::Source &source,
                  nix::RepairFlag repair,
                  nix::CheckSigsFlag /*checkSigs*/) override {
    rejectRepair(repair);
    auto accessor =
        parseToMemory(source, nix::FileSerialisationMethod::NixArchive);
    if (info.path.isDerivation()) {
      writeDerivation(parseDrv(*this, accessor->readFile(nix::CanonPath::root),
                               nix::Derivation::nameFromPath(info.path)),
                      nix::NoRepair);
    } else if (!pathRecord(info.path)) {
      storeObject(info, *accessor->root);
    }
  }

  auto addToStoreFromDump(nix::Source &source, std::string_view name,
                          nix::FileSerialisationMethod dumpMethod,
                          nix::ContentAddressMethod hashMethod,
                          nix::HashAlgorithm hashAlgo,
                          const nix::StorePathSet &references,
                          nix::RepairFlag repair) -> nix::StorePath override {
    if (nix::isDerivation(name)) {
      throw nix::Error(
          "refusing to add derivation '%s' with addToStoreFromDump", name);
    }
    rejectRepair(repair);

    auto accessor = parseToMemory(source, dumpMethod);
    auto hash = hashPath({accessor, nix::CanonPath::root},
                         hashMethod.getFileIngestionMethod(), hashAlgo)
                    .first;
    auto narHash = hashPath({accessor, nix::CanonPath::root},
                            nix::FileIngestionMethod::NixArchive,
                            nix::HashAlgorithm::SHA256);

    auto info = nix::ValidPathInfo::makeFromCA(
        *this, name,
        nix::ContentAddressWithReferences::fromParts(hashMethod,
                                                     std::move(hash),
                                                     {
                                                         .others = references,
                                                         .self = false,
                                                     }),
        std::move(narHash.first));
    info.narSize = narHash.second.value();

    if (!pathRecord(info.path)) {
      storeObject(info, *accessor->root);
    }
    return info.path;
  }

  auto writeDerivation(const nix::Derivation &drv, nix::RepairFlag /*repair*/)
      -> nix::StorePath override {
    auto drvPath = nix::computeStorePath(*this, drv);
    if (!drvText(drvPath)) {
      const std::scoped_lock lock(state->mutex);
      const auto id = state->store.putBlob(unparseDrv(*this, drv));
      state->store.registerRoot(id, false);
      metaPut(kDrvPrefix, drvPath, id);
      state->store.sync();
    }
    return drvPath;
  }

  auto readDerivation(const nix::StorePath &drvPath)
      -> nix::Derivation override {
    auto text = drvText(drvPath);
    if (!text) {
      throw nix::Error("derivation '%s' is not valid", printStorePath(drvPath));
    }
    return parseDrv(*this, std::move(*text),
                    nix::Derivation::nameFromPath(drvPath));
  }

  auto readInvalidDerivation(const nix::StorePath &drvPath)
      -> nix::Derivation override {
    return readDerivation(drvPath);
  }

  void
  TARMAC_REGISTER_DRV_OUTPUT(const nix::Realisation & /*output*/) override {
    unsupported("registerDrvOutput");
  }

  void queryRealisationUncached(
      const nix::DrvOutput & /*drvOutput*/,
      nix::Callback<std::shared_ptr<const nix::UnkeyedRealisation>>
          callback) noexcept override {
    callback(nullptr);
  }

  auto accessorFor(const nix::StorePath &path)
      -> std::shared_ptr<nix::SourceAccessor> {
    if (path.isDerivation()) {
      auto text = drvText(path);
      if (!text) {
        return nullptr;
      }
      return drvAccessor(std::move(*text)).get_ptr();
    }
    auto rec = pathRecord(path);
    if (!rec) {
      return nullptr;
    }
    if (rec->root_type == 'd') {
      return std::make_shared<PackAccessor>(state->store, rec->root_id);
    }
    return std::make_shared<BlobAccessor>(state->store, rec->root_type,
                                          rec->root_id);
  }

  auto getFSAccessor(const nix::StorePath &path, bool /*requireValidPath*/)
      -> std::shared_ptr<nix::SourceAccessor> override {
    return accessorFor(path);
  }

  auto getFSAccessor(bool /*requireValidPath*/)
      -> nix::ref<nix::SourceAccessor> override;
};

// dispatches whole-store paths to the per-object accessors
class StoreViewAccessor : public nix::SourceAccessor {
public:
  explicit StoreViewAccessor(TarmacStore &store) : store_(&store) {}

  void anchor() override {}

  void readFile(const nix::CanonPath &path, nix::Sink &sink,
                nix::fun<void(uint64_t)> sizeCallback) override {
    withAccessor(path, [&](auto &accessor, const auto &rest) {
      accessor.readFile(rest, sink, sizeCallback);
    });
  }

  auto pathExists(const nix::CanonPath &path) -> bool override {
    if (path.isRoot()) {
      return true;
    }
    bool res = false;
    tryWithAccessor(path, [&](auto &accessor, const auto &rest) {
      res = accessor.pathExists(rest);
    });
    return res;
  }

  auto maybeLstat(const nix::CanonPath &path) -> std::optional<Stat> override {
    if (path.isRoot()) {
      Stat info;
      info.type = tDirectory;
      return info;
    }
    std::optional<Stat> res;
    tryWithAccessor(path, [&](auto &accessor, const auto &rest) {
      res = accessor.maybeLstat(rest);
    });
    return res;
  }

  auto readDirectory(const nix::CanonPath &path) -> DirEntries override {
    DirEntries res;
    withAccessor(path, [&](auto &accessor, const auto &rest) {
      res = accessor.readDirectory(rest);
    });
    return res;
  }

  auto readLink(const nix::CanonPath &path) -> std::string override {
    std::string res;
    withAccessor(path, [&](auto &accessor, const auto &rest) {
      res = accessor.readLink(rest);
    });
    return res;
  }

private:
  template <typename Func>
  auto tryWithAccessor(const nix::CanonPath &path, Func func) -> bool {
    if (path.isRoot()) {
      return false;
    }
    const std::string base(*path.begin());
    auto accessor = cached(base);
    if (!accessor) {
      return false;
    }
    func(*accessor, path.removePrefix(nix::CanonPath{base}));
    return true;
  }

  template <typename Func>
  void withAccessor(const nix::CanonPath &path, Func func) {
    if (!tryWithAccessor(path, func)) {
      throw nix::Error("path '%s' does not exist in the tarmac store", path);
    }
  }

  auto cached(const std::string &base) -> std::shared_ptr<nix::SourceAccessor> {
    {
      const std::scoped_lock lock(mutex_);
      auto found = cache_.find(base);
      if (found != cache_.end()) {
        return found->second;
      }
    }
    std::shared_ptr<nix::SourceAccessor> accessor;
    try {
      accessor = store_->accessorFor(nix::StorePath{base});
    } catch (const nix::BadStorePath &) {
      return nullptr;
    }
    if (accessor) {
      const std::scoped_lock lock(mutex_);
      cache_.emplace(base, accessor);
    }
    return accessor;
  }

  TarmacStore *store_;
  std::mutex mutex_;
  std::map<std::string, std::shared_ptr<nix::SourceAccessor>> cache_;
};

auto TarmacStore::getFSAccessor(bool /*requireValidPath*/)
    -> nix::ref<nix::SourceAccessor> {
  auto accessor = nix::make_ref<StoreViewAccessor>(*this);
  accessor->setPathDisplay(config->storeDir);
  return accessor;
}

auto TarmacStoreConfig::openStore() const -> nix::ref<nix::Store> {
  return nix::make_ref<TarmacStore>(nix::ref{shared_from_this()});
}

nix::RegisterStoreImplementation<TarmacStoreConfig> regTarmacStore;

} // namespace

#endif
