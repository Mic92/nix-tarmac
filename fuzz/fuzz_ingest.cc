// libFuzzer target: arbitrary bytes as tarball -> ingest must never crash,
// and everything ingested must be readable back.
#include "tree.hpp"

#include <archive.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

TreeStore &store() {
  static std::unique_ptr<TreeStore> s = [] {
    std::string dir = "/tmp/tarmac-fuzz-" + std::to_string(getpid());
    std::string cmd = "rm -rf " + dir;
    if (system(cmd.c_str()) != 0)
      abort();
    return std::make_unique<TreeStore>(PackCas::open(dir));
  }();
  return *s;
}

void walk(TreeStore &s, TreeWalker &w, const std::string &root,
          const std::string &id, const std::string &path, int depth) {
  if (depth > 600)
    abort(); // deeper than the ingest limit: sanitization failed
  for (auto &e : s.readTree(id)) {
    std::string child = path.empty() ? e.name : path + "/" + e.name;
    TreeEntry found;
    if (!w.lookup(root, child, found) || found.id != e.id)
      abort(); // every ingested path must resolve to itself
    if (e.type == 'd')
      walk(s, w, root, e.id, child, depth + 1);
    else {
      std::string_view v;
      if (!s.readBlobView(e.id, v))
        abort(); // ingested tree references a missing blob
    }
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  auto &s = store();
  archive *a = archive_read_new();
  archive_read_support_filter_all(a);
  archive_read_support_format_all(a);
  if (archive_read_open_memory(a, data, size) != ARCHIVE_OK) {
    archive_read_free(a);
    return 0;
  }
  try {
    auto res = ingest_archive(s, a);
    TreeWalker w{s};
    walk(s, w, res.root, res.root, "", 0);
    uint64_t nar_size;
    nar_sha256(s, res.root, nar_size);
  } catch (const std::exception &) {
    // hostile archives may be rejected, but never crash
  }
  return 0;
}
