// Shims over Nix C++ API churn so the rest of the plugin has no version
// conditionals. NIX_COMPAT_VERSION_* come from meson.
#pragma once

#include <nix/store/derivations.hh>
#include <nix/store/path.hh>
#include <nix/store/store-dir-config.hh>
#include <nix/util/archive.hh>
#include <nix/util/canon-path.hh>
#include <nix/util/file-content-address.hh>
#include <nix/util/memory-source-accessor.hh>
#include <nix/util/serialise.hh>

#include <string>
#include <string_view>
#include <utility>

#define TARMAC_NIX_AT_LEAST(major, minor)                                      \
  (NIX_COMPAT_VERSION_MAJOR > (major) ||                                       \
   (NIX_COMPAT_VERSION_MAJOR == (major) &&                                     \
    NIX_COMPAT_VERSION_MINOR >= (minor)))

// Store / SourceAccessor grew an anchor() key function and the Store API
// settled enough for TarmacStore.
#define TARMAC_HAVE_STORE TARMAC_NIX_AT_LEAST(2, 35)

#if TARMAC_NIX_AT_LEAST(2, 35)
#define TARMAC_ANCHOR_OVERRIDE                                                 \
  void anchor() override {}
#else
#define TARMAC_ANCHOR_OVERRIDE
#endif

// 2.36 split Derivation into a template over its inputs, moved the ATerm
// (de)serialiser into nix::derivation and renamed the registerDrvOutput
// hook to registerDrvOutputUnchecked.
#if TARMAC_NIX_AT_LEAST(2, 36)
#include <nix/store/derivation/aterm.hh>
#define TARMAC_REGISTER_DRV_OUTPUT registerDrvOutputUnchecked
#else
#define TARMAC_REGISTER_DRV_OUTPUT registerDrvOutput
#endif

namespace nix_compat {

#if TARMAC_HAVE_STORE
inline auto parseDrv(const nix::StoreDirConfig &store, std::string &&text,
                     std::string_view name) -> nix::Derivation {
#if TARMAC_NIX_AT_LEAST(2, 36)
  return nix::derivation::parse(store, std::move(text), name);
#else
  return nix::parseDerivation(store, std::move(text), name);
#endif
}

inline auto unparseDrv(const nix::StoreDirConfig &store,
                       const nix::Derivation &drv) -> std::string {
#if TARMAC_NIX_AT_LEAST(2, 36)
  return nix::derivation::unparse(drv, store, false);
#else
  return drv.unparse(store, false);
#endif
}

// Restores a Flat or NAR serialisation into the empty accessor. NixOS/nix
// #15392 made a FileSystemObjectSink create only its own root, with no path
// argument, and added restorePath(sink, source, method) to do this dispatch.
// The overload is detected rather than a version compared, because the merge
// commit itself still reports 2.36.
// Sink is a template parameter only so that the branch not taken is dependent
// and never instantiated.
template <typename Sink = nix::MemorySink>
void restoreToMemory(nix::MemorySourceAccessor &accessor, nix::Source &source,
                     nix::FileSerialisationMethod method) {
  Sink sink{accessor};
  if constexpr (requires(Sink &s, nix::Source &src,
                         nix::FileSerialisationMethod m) {
                  nix::restorePath(s, src, m);
                }) {
    nix::restorePath(sink, source, method);
  } else {
    switch (method) {
    case nix::FileSerialisationMethod::NixArchive:
      nix::parseDump(sink, source);
      break;
    case nix::FileSerialisationMethod::Flat:
      accessor.root = nix::MemorySourceAccessor::File::Regular{};
      sink.createRegularFile(nix::CanonPath::root, [&](auto &fileSink) {
        source.drainInto(fileSink);
      });
      break;
    }
  }
}

// store paths a .drv refers to (input sources and input derivations)
inline auto drvReferences(const nix::Derivation &drv) -> nix::StorePathSet {
#if TARMAC_NIX_AT_LEAST(2, 36)
  nix::StorePathSet refs;
  for (const auto &input : drv.inputs) {
    refs.insert(input.getBaseStorePath());
  }
  return refs;
#else
  auto refs = drv.inputSrcs;
  for (const auto &[inputDrv, node] : drv.inputDrvs.map) {
    refs.insert(inputDrv);
  }
  return refs;
#endif
}
#endif

} // namespace nix_compat
