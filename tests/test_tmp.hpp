#pragma once

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

// mkdtemp under $TMPDIR: unpredictable, owned 0700, safe in shared /tmp
inline auto make_test_dir(const std::string &name) -> std::string {
  const char *tmp = std::getenv("TMPDIR");
  std::string templ = std::string(tmp != nullptr && *tmp != '\0' ? tmp : "/tmp") +
                      "/" + name + ".XXXXXX";
  if (mkdtemp(templ.data()) == nullptr) {
    perror("mkdtemp");
    std::abort();
  }
  static std::string created;
  created = templ;
  std::atexit([] { std::filesystem::remove_all(created); });
  return templ;
}
