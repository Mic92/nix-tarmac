// dlopen()s the plugin build matching the running Nix. Must not use any
// Nix API beyond the version string.
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace nix {
// declared by Nix itself, so it has to stay a mutable std::string
extern std::string
    nixVersion; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
} // namespace nix

namespace {

// "2.35.2" / "2.36pre20260813_8529b7a" -> "2.35" / "2.36"
auto major_minor(std::string_view version) -> std::string_view {
  const auto first_dot = version.find('.');
  const auto end = version.find_first_not_of("0123456789", first_dot + 1);
  return version.substr(0, end);
}

void warn(const std::string &message) {
  const std::string line =
      "nix-tarmac: " + message + ". falling back to the builtin fetcher\n";
  std::fputs(line.c_str(), stderr);
}

auto plugin_for_running_nix() -> std::filesystem::path {
  // any object inside this shared object locates the file it was loaded from
  static const int kAnchor = 0;
  Dl_info info{};
  if (dladdr(&kAnchor, &info) == 0 || info.dli_fname == nullptr) {
    return {};
  }
  return std::filesystem::path(info.dli_fname).parent_path().parent_path() /
         "nix-tarmac-versions" / major_minor(nix::nixVersion) /
         ("nix-tarmac." TARMAC_MODULE_SUFFIX);
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
    warn("no plugin build for Nix " + nix::nixVersion + " at '" +
         plugin.string() + "'");
    return;
  }
  if (dlopen(plugin.c_str(), RTLD_NOW | RTLD_GLOBAL) == nullptr) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    warn("failed to load '" + plugin.string() + "': " + dlerror());
  }
}
