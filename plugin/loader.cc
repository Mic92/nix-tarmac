// dlopen()s the plugin build whose Nix libraries are the ones already
// mapped into this process. Must not use any Nix API.
//
// Each build lives in nix-tarmac-versions/<soversion>/ and links against
// libnixstore with exactly that SONAME. Loading a build for any other
// SONAME would pull a second copy of the Nix libraries into the process,
// which appears to work and then double-frees their globals at exit.
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

#ifdef __APPLE__
auto nix_store_soname(const std::string &soversion) -> std::string {
  return "libnixstore." + soversion + ".dylib";
}
#else
auto nix_store_soname(const std::string &soversion) -> std::string {
  return "libnixstore.so." + soversion;
}
#endif

void warn(const std::string &message) {
  const std::string line =
      "nix-tarmac: " + message + ". falling back to the builtin fetcher\n";
  std::fputs(line.c_str(), stderr);
}

auto host_has(const std::string &soname) -> bool {
  void *handle = dlopen(soname.c_str(), RTLD_LAZY | RTLD_NOLOAD);
  if (handle == nullptr) {
    return false;
  }
  dlclose(handle);
  return true;
}

auto versions_dir() -> std::filesystem::path {
  // any object inside this shared object locates the file it was loaded from
  static const int kAnchor = 0;
  Dl_info info{};
  if (dladdr(&kAnchor, &info) == 0 || info.dli_fname == nullptr) {
    return {};
  }
  return std::filesystem::path(info.dli_fname).parent_path().parent_path() /
         "nix-tarmac-versions";
}

auto plugin_for_running_nix() -> std::filesystem::path {
  const auto dir = versions_dir();
  std::error_code ignored;
  for (const auto &entry : std::filesystem::directory_iterator(dir, ignored)) {
    if (host_has(nix_store_soname(entry.path().filename().string()))) {
      return entry.path() / ("nix-tarmac." TARMAC_MODULE_SUFFIX);
    }
  }
  return {};
}

} // namespace

// runs once while Nix loads plugins, before it starts any threads
extern "C" [[gnu::visibility("default")]] void nix_plugin_entry() {
  std::filesystem::path plugin;
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  if (const char *override = std::getenv("NIX_TARMAC_PLUGIN_OVERRIDE")) {
    plugin = override;
  } else {
    plugin = plugin_for_running_nix();
  }
  if (plugin.empty() || !std::filesystem::exists(plugin)) {
    warn("no plugin build matching the loaded libnixstore in '" +
         versions_dir().string() + "'");
    return;
  }
  if (dlopen(plugin.c_str(), RTLD_NOW | RTLD_GLOBAL) == nullptr) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    warn("failed to load '" + plugin.string() + "': " + dlerror());
  }
}
