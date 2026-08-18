// dlopen()s the plugin build matching the running Nix. Must not use any
// Nix API beyond the version string.
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include <string_view>

namespace nix {
extern std::string nixVersion;
}

namespace {

// "2.35.2" / "2.36pre20260813_8529b7a" -> "2.35" / "2.36"
std::string_view major_minor(std::string_view version) {
  auto first_dot = version.find('.');
  auto end = version.find_first_not_of("0123456789", first_dot + 1);
  return version.substr(0, end);
}

void warn(const std::string &message) {
  static_cast<void>(std::fprintf(
      stderr, "nix-tarmac: %s. falling back to the builtin fetcher\n",
      message.c_str()));
}

std::filesystem::path plugin_for_running_nix() {
  Dl_info info{};
  if (dladdr(reinterpret_cast<void *>(&plugin_for_running_nix), &info) == 0 ||
      info.dli_fname == nullptr)
    return {};
  return std::filesystem::path(info.dli_fname).parent_path().parent_path() /
         "nix-tarmac-versions" /
         major_minor(nix::nixVersion) /
         ("nix-tarmac." TARMAC_MODULE_SUFFIX);
}

} // namespace

extern "C" [[gnu::visibility("default")]] void nix_plugin_entry() {
  std::filesystem::path plugin;
  if (const char *override = std::getenv("NIX_TARMAC_PLUGIN_OVERRIDE"))
    plugin = override;
  else
    plugin = plugin_for_running_nix();
  if (plugin.empty() || !std::filesystem::exists(plugin)) {
    warn("no plugin build for Nix " + nix::nixVersion + " at '" +
         plugin.string() + "'");
    return;
  }
  if (!dlopen(plugin.c_str(), RTLD_NOW | RTLD_GLOBAL))
    warn("failed to load '" + plugin.string() + "': " + dlerror());
}
