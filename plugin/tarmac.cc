// Replaces Nix's builtin "tarball" input scheme with one backed by the
// pack CAS.
#include "ctx.hh"
#include "pack_accessor.hh"
#include "pack_cas.hpp"
#include "tree.hpp"

#include <archive.h>

#include <nix/fetchers/attrs.hh>
#include <nix/fetchers/cache.hh>
#include <nix/fetchers/fetch-settings.hh>
#include <nix/fetchers/fetch-to-store.hh>
#include <nix/fetchers/fetchers.hh>
#include <nix/store/filetransfer.hh>
#include <nix/store/store-api.hh>
#include <nix/util/canon-path.hh>
#include <nix/util/error.hh>
#include <nix/util/file-descriptor.hh>
#include <nix/util/fun.hh>
#include <nix/util/hash.hh>
#include <nix/util/logging.hh>
#include <nix/util/ref.hh>
#include <nix/util/serialise.hh>
#include <nix/util/source-accessor.hh>
#include <nix/util/sync.hh>
#include <nix/util/types.hh>
#include <nix/util/url.hh>
#include <nix/util/users.hh>
#include <nix/util/util.hh>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr size_t kDownloadChunk = 1 << 16;

template <typename Func> auto healing(Func func) {
  try {
    return func();
  } catch (const CorruptError &err) {
    Ctx::get()->poison();
    throw nix::Error("tarmac cache is corrupt (%s); it resets on the next run",
                     err.what());
  }
}

// libarchive read callback over a Nix Source
class SourceReader {
public:
  explicit SourceReader(nix::Source &source) : source_(&source) {}

  static auto read_cb(struct archive * /*unused*/, void *data, const void **out)
      -> la_ssize_t {
    auto *self = static_cast<SourceReader *>(data);
    *out = self->buf_.data();
    try {
      return static_cast<la_ssize_t>(
          self->source_->read(self->buf_.data(), self->buf_.size()));
    } catch (nix::EndOfFile &) {
      return 0;
    } catch (...) {
      return -1;
    }
  }

private:
  nix::Source *source_;
  std::vector<char> buf_ = std::vector<char>(kDownloadChunk);
};

auto has_tarball_extension(const nix::ParsedURL &url) -> bool {
  if (url.path.empty()) {
    return false;
  }
  static constexpr std::array<std::string_view, 6> kSuffixes = {
      ".tar", ".tgz", ".tar.gz", ".tar.xz", ".tar.bz2", ".tar.zst"};
  const std::string_view name = url.path.back();
  return std::ranges::any_of(kSuffixes, [&](std::string_view suffix) -> bool {
    return name.ends_with(suffix);
  });
}

auto open_archive(nix::Source &source, const std::string &url)
    -> std::pair<struct archive *, std::unique_ptr<SourceReader>> {
  auto reader = std::make_unique<SourceReader>(source);
  struct archive *arc = archive_read_new();
  archive_read_support_filter_all(arc);
  archive_read_support_format_all(arc);
  if (archive_read_open(arc, reader.get(), nullptr, SourceReader::read_cb,
                        nullptr) != ARCHIVE_OK) {
    const char *msg = archive_error_string(arc);
    const std::string err = msg != nullptr ? msg : "unknown archive error";
    archive_read_free(arc);
    throw nix::Error("failed to read tarball '%s': %s", url, err);
  }
  return {arc, std::move(reader)};
}

struct FastTarballInputScheme : nix::fetchers::InputScheme {
  using Attrs = nix::fetchers::Attrs;
  using Input = nix::fetchers::Input;
  using Settings = nix::fetchers::Settings;

  [[nodiscard]] auto schemeName() const -> std::string_view override {
    return "tarball";
  }

  [[nodiscard]] auto schemeDescription() const -> std::string override {
    return "Download a tar archive into a local content-addressed cache "
           "and expose it as a lazily accessed source tree.";
  }

  [[nodiscard]] auto allowedAttrs() const
      -> const std::map<std::string, AttributeInfo> & override {
    // superset of the builtin scheme so locked inputs keep working
    static const std::map<std::string, AttributeInfo> kAttrs = {
        {"url", {.type = "String", .required = true, .doc = "Tarball URL."}},
        {"narHash", {}},
        {"name", {}},
        {"unpack", {}},
        {"rev", {}},
        {"revCount", {}},
        {"lastModified", {}},
    };
    return kAttrs;
  }

  // Nix 2.36 dropped the Settings parameter, only one overload overrides
  [[nodiscard]] auto inputFromURL(const nix::fetchers::Settings & /*s*/,
                                  const nix::ParsedURL &url,
                                  bool requireTree) const
      -> std::optional<Input> {
    return inputFromURL(url, requireTree);
  }

  [[nodiscard]] auto inputFromURL(const nix::ParsedURL &orig_url,
                                  bool requireTree) const
      -> std::optional<Input> {
    const auto scheme = nix::parseUrlScheme(orig_url.scheme);
    static const nix::StringSet kTransports{"http", "https", "file"};
    if (!kTransports.contains(std::string(scheme.transport))) {
      return std::nullopt;
    }
    const bool applies = scheme.application
                             ? *scheme.application == "tarball"
                             : (requireTree || has_tarball_extension(orig_url));
    if (!applies) {
      return std::nullopt;
    }
    auto url = orig_url;
    url.scheme = std::string(scheme.transport);
    Input input{};
    for (const auto *attr : {"narHash", "rev", "revCount", "lastModified"}) {
      if (const auto *value = nix::get(url.query, attr)) {
        input.attrs.insert_or_assign(attr, *value);
        url.query.erase(attr);
      }
    }
    input.attrs.insert_or_assign("type", std::string{schemeName()});
    input.attrs.insert_or_assign("url", url.to_string());
    return input;
  }

  [[nodiscard]] auto inputFromAttrs(const nix::fetchers::Settings & /*s*/,
                                    const Attrs &attrs) const
      -> std::optional<Input> {
    return inputFromAttrs(attrs);
  }

  [[nodiscard]] auto inputFromAttrs(const Attrs &attrs) const
      -> std::optional<Input> {
    Input input{};
    input.attrs = attrs;
    return input;
  }

  [[nodiscard]] auto toURL(const Input &input) const
      -> nix::ParsedURL override {
    auto url = nix::parseURL(nix::fetchers::getStrAttr(input.attrs, "url"));
    if (const auto narHash = input.getNarHash()) {
      url.query.insert_or_assign(
          "narHash", narHash->to_string(nix::HashFormat::SRI, true));
    }
    return url;
  }

  [[nodiscard]] auto isLocked(const Settings & /*settings*/,
                              const Input &input) const -> bool override {
    return input.getNarHash().has_value();
  }

  auto getFingerprint(nix::Store & /*store*/, const Input &input) const
      -> std::optional<std::string> override {
    if (const auto narHash = input.getNarHash()) {
      return narHash->to_string(nix::HashFormat::SRI, true);
    }
    return std::nullopt;
  }

  auto getAccessor(const Settings &settings, nix::Store & /*store*/,
                   const Input &orig_input) const
      -> std::pair<nix::ref<nix::SourceAccessor>, Input> override {
    auto input(orig_input);
    const auto url = nix::fetchers::getStrAttr(input.attrs, "url");
    auto &ctx = *Ctx::get();

    const nix::fetchers::Cache::Key key{"tarmac", {{"url", url}}};
    auto cached = settings.getCache()->lookupExpired(key);

    if (cached) {
      try {
        if (!ctx.store.hasTree(
                from_hex(nix::fetchers::getStrAttr(cached->value, "root")))) {
          cached.reset();
        }
      } catch (const CorruptError &) {
        ctx.poison();
        cached.reset();
      }
    }

    Attrs info;
    if (cached && !cached->expired) {
      info = cached->value;
    } else {
      info = download(settings, url, cached);
      settings.getCache()->upsert(key, info);
    }

    input.attrs.insert_or_assign("narHash",
                                 nix::fetchers::getStrAttr(info, "narHash"));

    std::string root = from_hex(nix::fetchers::getStrAttr(info, "root"));
    ctx.store.touchRoot(root);
    auto accessor = nix::make_ref<PackAccessor>(ctx.store, std::move(root),
                                                [] { Ctx::get()->poison(); });
    accessor->setPathDisplay("«" + input.to_string() + "»");
    return {accessor, input};
  }

private:
  using CachedResult = std::optional<nix::fetchers::Cache::Result>;

  static auto download(const Settings &settings, const std::string &url,
                       const CachedResult &cached) -> Attrs {
    auto &ctx = *Ctx::get();
    nix::FileTransferRequest req{nix::VerbatimURL{url}};
    if (cached) {
      req.expectedETag = nix::fetchers::getStrAttr(cached->value, "etag");
    }
    auto transfer = std::make_shared<nix::Sync<nix::FileTransferResult>>();
    auto source = nix::sinkToSource([&](nix::Sink &sink) -> void {
      nix::getFileTransfer()->download(
          std::move(req), sink,
          [transfer](nix::FileTransferResult result) -> void {
            *transfer->lock() = std::move(result);
          });
    });

    const std::scoped_lock lock(ctx.mutex);
    auto [arc, reader] = open_archive(*source, url);
    auto ingest = ingest_archive(ctx.store, arc);
    // like the builtin: a single top-level directory is stripped
    if (auto top = ctx.store.readTree(ingest.root);
        top.size() == 1 && top[0].type == 'd') {
      ingest.root = top[0].id;
      ctx.store.registerRoot(ingest.root);
    }

    auto result = transfer->lock();
    if (result->cached && cached) {
      return cached->value;
    }
    uint64_t narSize = 0;
    const auto narHash = nar_sha256(ctx.store, ingest.root, narSize);
    const auto narHashSri =
        nix::Hash::parseAny(to_hex(narHash), nix::HashAlgorithm::SHA256)
            .to_string(nix::HashFormat::SRI, true);
    // otherwise mountInput re-dumps the tree to recompute the narHash
    settings.getCache()->upsert(nix::makeSourcePathToHashCacheKey(
                                    "tarmac:" + to_hex(ingest.root),
                                    nix::ContentAddressMethod::Raw::NixArchive,
                                    nix::CanonPath::root),
                                {{"hash", narHashSri}});
    return {
        {"etag", result->etag},
        {"root", to_hex(ingest.root)},
        {"narHash", narHashSri},
    };
  }
};

static_assert(!std::is_abstract_v<FastTarballInputScheme>);

[[maybe_unused]] const auto kRegisterScheme = nix::OnStartup([] -> void {
  // evict the builtin so plain https://...tar.gz URLs hit this scheme;
  // Nix only exposes the registry read-only
  const auto &registry = nix::fetchers::getAllInputSchemes();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  auto &schemes = const_cast<nix::fetchers::InputSchemeMap &>(registry);
  if (schemes.erase("tarball") != 1) {
    // no "warning:" prefix: the NixOS nix.conf check fails on warnings
    std::fputs("nix-tarmac: builtin tarball scheme not found; falling back\n",
               stderr);
    return;
  }
  nix::fetchers::registerInputScheme(
      std::make_unique<FastTarballInputScheme>());
});

} // namespace
