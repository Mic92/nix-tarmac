// cases mirror nix/src/libfetchers-tests/merkle-tar-adapter.cc
#include "tree.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>

namespace {

struct Tar {
  archive *a = archive_write_new();
  std::string buf = std::string(1 << 20, '\0');
  size_t used = 0;

  Tar() {
    archive_write_set_format_pax_restricted(a);
    archive_write_open_memory(a, buf.data(), buf.size(), &used);
  }

  Tar &entry(const char *path, mode_t type, mode_t perm, const char *data,
             const char *symlink, const char *hardlink) {
    archive_entry *e = archive_entry_new();
    archive_entry_set_pathname(e, path);
    archive_entry_set_filetype(e, type);
    archive_entry_set_perm(e, perm);
    if (symlink)
      archive_entry_set_symlink(e, symlink);
    if (hardlink)
      archive_entry_set_hardlink(e, hardlink);
    size_t len = data ? strlen(data) : 0;
    archive_entry_set_size(e, static_cast<int64_t>(len));
    assert(archive_write_header(a, e) == ARCHIVE_OK);
    if (data)
      assert(archive_write_data(a, data, len) == static_cast<ssize_t>(len));
    archive_entry_free(e);
    return *this;
  }
  Tar &file(const char *path, const char *data, mode_t perm = 0644) {
    return entry(path, AE_IFREG, perm, data, nullptr, nullptr);
  }
  Tar &symlink(const char *path, const char *target) {
    return entry(path, AE_IFLNK, 0777, nullptr, target, nullptr);
  }
  Tar &hardlink(const char *path, const char *target) {
    return entry(path, AE_IFREG, 0644, nullptr, nullptr, target);
  }
  Tar &dir(const char *path) {
    return entry(path, AE_IFDIR, 0755, nullptr, nullptr, nullptr);
  }
  Tar &fifo(const char *path) {
    return entry(path, AE_IFIFO, 0644, nullptr, nullptr, nullptr);
  }

  std::string finish() {
    assert(archive_write_close(a) == ARCHIVE_OK);
    archive_write_free(a);
    buf.resize(used);
    return std::move(buf);
  }
};

struct Fixture {
  TreeStore &store;
  std::string root;
  TreeWalker walker;

  Fixture(TreeStore &s, Tar &tar) : store(s), walker(s) {
    std::string data = tar.finish();
    archive *a = archive_read_new();
    archive_read_support_format_all(a);
    assert(archive_read_open_memory(a, data.data(), data.size()) == ARCHIVE_OK);
    root = ingest_archive(store, a).root;
  }

  TreeEntry get(const char *path) {
    TreeEntry e;
    if (!walker.lookup(root, path, e)) {
      fprintf(stderr, "missing: %s\n", path);
      abort();
    }
    return e;
  }
  void contents(const char *path, const std::string &want, char type = 'r') {
    auto e = get(path);
    auto got = store.readBlob(e.id);
    if (e.type != type || got != want) {
      fprintf(stderr, "%s: got %c '%s', want %c '%s'\n", path, e.type,
              got.c_str(), type, want.c_str());
      abort();
    }
  }
  void is_symlink(const char *path, const std::string &target) {
    contents(path, target, 's');
  }
  void is_dir(const char *path) { assert(get(path).type == 'd'); }
  void absent(const char *path) {
    TreeEntry e;
    assert(!walker.lookup(root, path, e));
  }
};

void must_fail(TreeStore &store, Tar &tar, const char *what) {
  std::string data = tar.finish();
  archive *a = archive_read_new();
  archive_read_support_format_all(a);
  assert(archive_read_open_memory(a, data.data(), data.size()) == ARCHIVE_OK);
  try {
    ingest_archive(store, a);
  } catch (const std::runtime_error &e) {
    if (strstr(e.what(), what) == nullptr) {
      fprintf(stderr, "wrong error: '%s', want '%s'\n", e.what(), what);
      abort();
    }
    return;
  }
  fprintf(stderr, "expected failure: %s\n", what);
  abort();
}

} // namespace

int main(int argc, char **argv) {
  std::string dir = argc > 1 ? argv[1] : "/tmp/packcas-tar-semantics-test";
  std::string cmd = "rm -rf " + dir;
  if (system(cmd.c_str()) != 0)
    return 1;
  TreeStore store(PackCas::open(dir));

  {
    Tar t;
    t.file("dir/target", "shared", 0755)
        .hardlink("dir/link", "dir/target")
        .symlink("s", "tgt")
        .hardlink("h", "s");
    Fixture f(store, t);
    f.contents("dir/target", "shared", 'x');
    f.contents("dir/link", "shared", 'x');
    f.is_symlink("h", "tgt");
  }
  {
    Tar t;
    t.file("a", "x").hardlink("b", "a").hardlink("c", "b").hardlink("d", "a");
    Fixture f(store, t);
    for (const char *p : {"a", "b", "c", "d"})
      f.contents(p, "x");
  }
  {
    Tar t;
    t.file("target", "old").hardlink("link", "target").file("target", "new");
    Fixture f(store, t);
    f.contents("target", "new");
    f.contents("link", "old");
  }
  {
    Tar t;
    t.file("target", "original")
        .hardlink("link", "target")
        .file("link", "replacement");
    Fixture f(store, t);
    f.contents("target", "original");
    f.contents("link", "replacement");
  }
  {
    Tar t;
    t.symlink("s", "some-target").hardlink("h", "s").file("s", "file now");
    Fixture f(store, t);
    f.contents("s", "file now");
    f.is_symlink("h", "some-target");
  }
  {
    Tar t;
    t.file("./x/../a", "A").hardlink("./b", "./a");
    Fixture f(store, t);
    f.contents("a", "A");
    f.contents("b", "A");
    f.absent("x");
  }
  {
    Tar t;
    t.hardlink("link", "missing");
    must_fail(store, t, "does not exist");
  }
  {
    Tar t;
    t.dir("d").hardlink("link", "d");
    must_fail(store, t, "is a directory");
  }
  {
    Tar t;
    t.file("f", "").hardlink("link", "/");
    must_fail(store, t, "is a directory");
  }

  {
    Tar t;
    t.file("a", "1")
        .file("a", "2")
        .symlink("b", "x")
        .file("b", "3")
        .file("c", "4")
        .symlink("c", "y")
        .file("e", "5")
        .file("e", "5", 0755)
        .file("sub/n", "old")
        .file("sub/n", "new");
    Fixture f(store, t);
    f.contents("a", "2");
    f.contents("b", "3");
    f.is_symlink("c", "y");
    f.contents("e", "5", 'x');
    f.contents("sub/n", "new");
  }
  {
    Tar t;
    t.file("foo", "content").dir("foo").file("foo/bar", "child");
    Fixture f(store, t);
    f.contents("foo/bar", "child");
  }
  {
    Tar t;
    t.dir("e").file("e", "now a file").file("d/x", "1").dir("d");
    Fixture f(store, t);
    f.contents("e", "now a file");
    f.contents("d/x", "1");
  }
  {
    Tar t;
    t.file("d/x", "1").file("d", "nope");
    must_fail(store, t, "non-empty directory");
  }
  {
    Tar t;
    t.file("d/x", "1").symlink("d", "nope");
    must_fail(store, t, "non-empty directory");
  }
  {
    Tar t;
    t.file("f", "").file("f/c", "");
    must_fail(store, t, "not a directory");
  }
  {
    Tar t;
    t.symlink("s", "x").dir("s/c");
    must_fail(store, t, "not a directory");
  }
  {
    Tar t;
    t.file("a", "").hardlink("h", "a").file("h/c", "");
    must_fail(store, t, "not a directory");
  }
  {
    Tar t;
    t.fifo("p");
    must_fail(store, t, "unsupported file type");
  }

  puts("tar semantics test ok");
  return 0;
}
