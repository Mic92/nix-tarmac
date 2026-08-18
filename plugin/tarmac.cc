// Replaces Nix's builtin "tarball" input scheme with one backed by the
// pack CAS.
#include "tree.hpp"

#include <archive.h>

#include "nix/fetchers/cache.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/store/filetransfer.hh"
#include "nix/store/store-api.hh"
#include "nix/util/serialise.hh"
#include "nix/util/users.hh"

#include <filesystem>
#include <fstream>
#include <mutex>

#define NIX_COMPAT_AT_LEAST(major, minor)                                      \
  (NIX_COMPAT_VERSION_MAJOR > (major) ||                                       \
   (NIX_COMPAT_VERSION_MAJOR == (major) &&                                     \
    NIX_COMPAT_VERSION_MINOR >= (minor)))

#if NIX_COMPAT_AT_LEAST(2, 36)
#define INPUT_FROM_SETTINGS_PARAM
#else
#define INPUT_FROM_SETTINGS_PARAM const Settings &,
#endif

using namespace nix;
using namespace nix::fetchers;

namespace {

struct Ctx {
  std::mutex mutex; // single writer; reads are lock-free
  std::string dir;
  TreeStore store;

  Ctx() : dir(getCacheDir() + "/tarmac"), store(openHealed(dir)) {
    try {
      store.maybeGc();
    } catch (...) {
    }
  }

  static std::unique_ptr<PackCas> openHealed(const std::string &dir) {
    std::error_code ec;
    if (std::filesystem::exists(dir + "/poison", ec))
      std::filesystem::remove_all(dir, ec);
    return PackCas::open(dir);
  }

  void poison() {
    std::ofstream(dir + "/poison") << "corrupt\n";
  }

  static Ctx &get() {
    static Ctx ctx;
    return ctx;
  }
};

template <typename F> auto healing(F f) {
  try {
    return f();
  } catch (const CorruptError &e) {
    Ctx::get().poison();
    throw Error("tarmac cache is corrupt (%s); it resets on the next run",
                e.what());
  }
}

struct PackAccessor : SourceAccessor {
  TreeStore &store;
  std::string root;
  TreeWalker walker;
  std::mutex mutex; // TreeWalker cache is not thread-safe

  PackAccessor(TreeStore &store_, std::string root_)
      : store(store_), root(std::move(root_)), walker{store_} {
    fingerprint = "tarmac:" + to_hex(root);
  }

#if NIX_COMPAT_AT_LEAST(2, 35)
  void anchor() override {}
#endif

  std::optional<TreeEntry> find(const CanonPath &path) {
    return healing([&]() -> std::optional<TreeEntry> {
      std::lock_guard l(mutex);
      TreeEntry e;
      if (!walker.lookup(root, path.rel(), e))
        return std::nullopt;
      return e;
    });
  }

  void readFile(const CanonPath &path, Sink &sink,
                fun<void(uint64_t)> sizeCallback) override {
    auto e = find(path);
    if (!e || e->type == 'd' || e->type == 's')
      throw Error("path '%s' is not a regular file", path);
    std::string_view v;
    if (!healing([&] { return store.readBlobView(e->id, v); }))
      throw Error("missing blob for '%s'", path);
    sizeCallback(v.size());
    sink(v);
  }

  bool pathExists(const CanonPath &path) override { return (bool) find(path); }

  std::optional<Stat> maybeLstat(const CanonPath &path) override {
    auto e = find(path);
    if (!e)
      return std::nullopt;
    Stat st;
    switch (e->type) {
    case 'd':
      st.type = tDirectory;
      break;
    case 's':
      st.type = tSymlink;
      break;
    default:
      st.type = tRegular;
      st.isExecutable = e->type == 'x';
      std::string_view v;
      if (store.readBlobView(e->id, v))
        st.fileSize = v.size();
    }
    return st;
  }

  DirEntries readDirectory(const CanonPath &path) override {
    auto e = find(path);
    if (!e || e->type != 'd')
      throw Error("path '%s' is not a directory", path);
    DirEntries res;
    for (auto &child : healing([&] { return store.readTree(e->id); }))
      res.emplace(child.name, child.type == 'd'   ? tDirectory
                              : child.type == 's' ? tSymlink
                                                  : tRegular);
    return res;
  }

  std::string readLink(const CanonPath &path) override {
    auto e = find(path);
    if (!e || e->type != 's')
      throw Error("path '%s' is not a symlink", path);
    return healing([&] { return store.readBlob(e->id); });
  }
};

struct SourceReader {
  Source &source;
  std::vector<char> buf = std::vector<char>(1 << 16);

  static la_ssize_t read_cb(struct archive *, void *data, const void **out) {
    auto *self = static_cast<SourceReader *>(data);
    *out = self->buf.data();
    try {
      return self->source.read(self->buf.data(), self->buf.size());
    } catch (EndOfFile &) {
      return 0;
    } catch (...) {
      return -1;
    }
  }
};

bool has_tarball_extension(const ParsedURL &url) {
  if (url.path.empty())
    return false;
  const auto &path = url.path.back();
  std::string_view p = path;
  for (auto suffix : {".tar", ".tgz", ".tar.gz", ".tar.xz", ".tar.bz2",
                      ".tar.zst"})
    if (p.ends_with(suffix))
      return true;
  return false;
}

struct FastTarballInputScheme : InputScheme {
  std::string_view schemeName() const override { return "tarball"; }

  std::string schemeDescription() const override {
    return "Download a tar archive into a local content-addressed cache "
           "and expose it as a lazily accessed source tree.";
  }

  const std::map<std::string, AttributeInfo> &allowedAttrs() const override {
    // superset of the builtin scheme so locked inputs keep working
    static const std::map<std::string, AttributeInfo> attrs = {
        {"url", {.type = "String", .required = true, .doc = "Tarball URL."}},
        {"narHash", {}},
        {"name", {}},
        {"unpack", {}},
        {"rev", {}},
        {"revCount", {}},
        {"lastModified", {}},
    };
    return attrs;
  }

  std::optional<Input> inputFromURL(INPUT_FROM_SETTINGS_PARAM
                                    const ParsedURL &_url,
                                    bool requireTree) const override {
    auto scheme = parseUrlScheme(_url.scheme);
    static const StringSet transports{"http", "https", "file"};
    if (!transports.count(std::string(scheme.transport)))
      return std::nullopt;
    bool applies = scheme.application
                       ? *scheme.application == "tarball"
                       : (requireTree || has_tarball_extension(_url));
    if (!applies)
      return std::nullopt;
    auto url = _url;
    url.scheme = std::string(scheme.transport);
    Input input{};
    for (auto attr : {"narHash", "rev", "revCount", "lastModified"})
      if (auto v = get(url.query, attr)) {
        input.attrs.insert_or_assign(attr, *v);
        url.query.erase(attr);
      }
    input.attrs.insert_or_assign("type", std::string{schemeName()});
    input.attrs.insert_or_assign("url", url.to_string());
    return input;
  }

  std::optional<Input> inputFromAttrs(INPUT_FROM_SETTINGS_PARAM
                                      const Attrs &attrs) const override {
    Input input{};
    input.attrs = attrs;
    return input;
  }

  ParsedURL toURL(const Input &input) const override {
    auto url = parseURL(getStrAttr(input.attrs, "url"));
    if (auto narHash = input.getNarHash())
      url.query.insert_or_assign("narHash",
                                 narHash->to_string(HashFormat::SRI, true));
    return url;
  }

  bool isLocked(const Settings &, const Input &input) const override {
    return (bool) input.getNarHash();
  }

  std::optional<std::string> getFingerprint(Store &,
                                            const Input &input) const override {
    if (auto narHash = input.getNarHash())
      return narHash->to_string(HashFormat::SRI, true);
    return std::nullopt;
  }

  std::pair<ref<SourceAccessor>, Input>
  getAccessor(const Settings &settings, Store &,
              const Input &_input) const override {
    auto input(_input);
    auto url = getStrAttr(input.attrs, "url");
    auto &ctx = Ctx::get();

    Cache::Key key{"tarmac", {{"url", url}}};
    auto cached = settings.getCache()->lookupExpired(key);

    if (cached) {
      try {
        if (!ctx.store.hasTree(from_hex(getStrAttr(cached->value, "root"))))
          cached.reset();
      } catch (const CorruptError &) {
        ctx.poison();
        cached.reset();
      }
    }

    Attrs info;
    if (cached && !cached->expired) {
      info = cached->value;
    } else {
      FileTransferRequest req(VerbatimURL{url});
      if (cached)
        req.expectedETag = getStrAttr(cached->value, "etag");
      auto res = std::make_shared<Sync<FileTransferResult>>();
      auto source = sinkToSource([&](Sink &sink) {
        getFileTransfer()->download(
            std::move(req), sink,
            [res](FileTransferResult r) { *res->lock() = r; });
      });

      std::lock_guard l(ctx.mutex);
      SourceReader reader{*source};
      struct archive *a = archive_read_new();
      archive_read_support_filter_all(a);
      archive_read_support_format_all(a);
      if (archive_read_open(a, &reader, nullptr, SourceReader::read_cb,
                            nullptr) != ARCHIVE_OK) {
        std::string err = archive_error_string(a);
        archive_read_free(a);
        throw Error("failed to read tarball '%s': %s", url, err);
      }
      auto ingest = ingest_archive(ctx.store, a);
      // like the builtin: a single top-level directory is stripped
      if (auto top = ctx.store.readTree(ingest.root);
          top.size() == 1 && top[0].type == 'd') {
        ingest.root = top[0].id;
        ctx.store.registerRoot(ingest.root);
      }

      auto r = res->lock();
      if (r->cached && cached) {
        info = cached->value;
      } else {
        uint64_t narSize;
        auto narHash = nar_sha256(ctx.store, ingest.root, narSize);
        info.insert_or_assign("etag", r->etag);
        info.insert_or_assign("root", to_hex(ingest.root));
        info.insert_or_assign(
            "narHash", Hash::parseAny(to_hex(narHash), HashAlgorithm::SHA256)
                           .to_string(HashFormat::SRI, true));
      }
      settings.getCache()->upsert(key, info);
    }

    input.attrs.insert_or_assign("narHash", getStrAttr(info, "narHash"));

    std::string root = from_hex(getStrAttr(info, "root"));
    ctx.store.touchRoot(root);
    auto accessor = make_ref<PackAccessor>(ctx.store, std::move(root));
    accessor->setPathDisplay("«" + input.to_string() + "»");
    return {accessor, input};
  }
};

[[maybe_unused]] auto rScheme = OnStartup([] {
  // evict the builtin so plain https://...tar.gz URLs hit this scheme
  auto &schemes = const_cast<InputSchemeMap &>(getAllInputSchemes());
  if (schemes.erase("tarball") != 1) {
    warn("nix-tarmac: builtin tarball scheme not found; "
         "falling back to the builtin fetcher");
    return;
  }
  registerInputScheme(std::make_unique<FastTarballInputScheme>());
});

} // namespace
