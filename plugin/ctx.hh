// per-directory store state, shared between the fetcher and the eval
// store. LMDB forbids opening the same environment twice in one
// process, so all users of a directory must go through get().
#ifndef NIX_TARMAC_CTX_HH
#define NIX_TARMAC_CTX_HH

#include "pack_cas.hpp"
#include "tree.hpp"

#include <nix/util/logging.hh>
#include <nix/util/users.hh>

#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

struct Ctx {
  std::mutex mutex; // single writer; reads are lock-free
  std::string dir;
  TreeStore store;

  explicit Ctx(std::string directory)
      : dir(std::move(directory)), store(openHealed(dir)) {
    try {
      store.maybeGc();
    } catch (const std::exception &err) {
      nix::warn("nix-tarmac: garbage collection failed: %s", err.what());
    }
  }

  static auto openHealed(const std::string &dir) -> std::unique_ptr<PackCas> {
    std::error_code err;
    if (std::filesystem::exists(dir + "/poison", err)) {
      std::filesystem::remove_all(dir, err);
    }
    std::filesystem::create_directories(dir, err);
    return PackCas::open(dir);
  }

  void poison() const { std::ofstream(dir + "/poison") << "corrupt\n"; }

  static auto defaultDir() -> std::string {
    return (nix::getCacheDir() / "tarmac").string();
  }

  static auto get(const std::string &dir = defaultDir())
      -> std::shared_ptr<Ctx> {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<Ctx>> instances;
    const std::scoped_lock lock(mutex);
    auto &ctx = instances[dir];
    if (!ctx) {
      ctx = std::make_shared<Ctx>(dir);
    }
    return ctx;
  }
};

#endif
