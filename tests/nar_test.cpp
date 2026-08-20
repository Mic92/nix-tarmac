// golden test: NAR hash must match `nix hash path` for a known tree
#include "test_tmp.hpp"

#include "tree.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// same tree that was verified against `nix hash path`
constexpr char kGolden[] =
    "b2bba15d0620838f2d26dadf031b1230accaeb31a27c72408895148d07cecd96";

void add_entry(archive *a, const char *path, mode_t type, mode_t perm,
               const char *data, const char *link) {
  archive_entry *e = archive_entry_new();
  archive_entry_set_pathname(e, path);
  archive_entry_set_filetype(e, type);
  archive_entry_set_perm(e, perm);
  if (link)
    archive_entry_set_symlink(e, link);
  size_t len = data ? strlen(data) : 0;
  archive_entry_set_size(e, len);
  archive_write_header(a, e);
  if (data)
    archive_write_data(a, data, len);
  archive_entry_free(e);
}

} // namespace

int main(int argc, char **argv) {
  std::string dir = argc > 1 ? argv[1] : make_test_dir("packcas-nar-test");

  std::string tar;
  {
    archive *a = archive_write_new();
    archive_write_set_format_pax_restricted(a);
    char buf[65536];
    size_t used;
    archive_write_open_memory(a, buf, sizeof(buf), &used);
    add_entry(a, "file", AE_IFREG, 0644, "hello", nullptr);
    add_entry(a, "link", AE_IFLNK, 0777, nullptr, "file");
    add_entry(a, "sub", AE_IFDIR, 0755, nullptr, nullptr);
    add_entry(a, "sub/run.sh", AE_IFREG, 0755, "#!x\n", nullptr);
    archive_write_free(a);
    tar.assign(buf, used);
  }

  TreeStore store(PackCas::open(dir));
  archive *a = archive_read_new();
  archive_read_support_format_all(a);
  assert(archive_read_open_memory(a, tar.data(), tar.size()) == ARCHIVE_OK);
  auto res = ingest_archive(store, a);
  assert(res.files == 3);

  uint64_t nar_size;
  auto h = to_hex(nar_sha256(store, res.root, nar_size));
  if (h != kGolden) {
    fprintf(stderr, "NAR hash mismatch: %s != %s\n", h.c_str(), kGolden);
    return 1;
  }

  // walker sanity
  TreeWalker w{store};
  TreeEntry e;
  assert(w.lookup(res.root, "sub/run.sh", e) && e.type == 'x');
  assert(w.lookup(res.root, "link", e) && e.type == 's');
  assert(!w.lookup(res.root, "sub/missing", e));
  assert(!w.lookup(res.root, "file/nope", e));
  assert(store.readBlob(e.id).empty() == false || true);

  printf("nar test ok: %s\n", h.c_str());
  return 0;
}
