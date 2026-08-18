// TTL eviction over roots plus mark-and-sweep compaction
#include "tree.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <cassert>
#include <cstdio>
#include <unistd.h>
#include <string>
#include <vector>

namespace {

// ~100 KB tarball with content unique to `id`
std::string make_tar(unsigned id) {
  std::string buf(1 << 20, '\0');
  size_t used = 0;
  archive *a = archive_write_new();
  archive_write_set_format_pax_restricted(a);
  archive_write_open_memory(a, buf.data(), buf.size(), &used);
  for (unsigned f = 0; f < 50; f++) {
    std::string data = "tarball-" + std::to_string(id) + "-file-" +
                       std::to_string(f) + "-";
    data.resize(2000, static_cast<char>('a' + id % 26));
    std::string name =
        "root/dir" + std::to_string(f % 5) + "/f" + std::to_string(f);
    archive_entry *e = archive_entry_new();
    archive_entry_set_pathname(e, name.c_str());
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_perm(e, 0644);
    archive_entry_set_size(e, static_cast<int64_t>(data.size()));
    assert(archive_write_header(a, e) == ARCHIVE_OK);
    assert(archive_write_data(a, data.data(), data.size()) ==
           static_cast<ssize_t>(data.size()));
    archive_entry_free(e);
  }
  assert(archive_write_close(a) == ARCHIVE_OK);
  archive_write_free(a);
  buf.resize(used);
  return buf;
}

IngestResult ingest(TreeStore &store, unsigned id) {
  std::string tar = make_tar(id);
  archive *a = archive_read_new();
  archive_read_support_format_all(a);
  assert(archive_read_open_memory(a, tar.data(), tar.size()) == ARCHIVE_OK);
  return ingest_archive(store, a);
}

} // namespace

int main(int argc, char **argv) {
  std::string dir = argc > 1 ? argv[1] : "/tmp/packcas-gc-test";
  std::string cmd = "rm -rf " + dir;
  if (system(cmd.c_str()) != 0)
    return 1;

  constexpr uint64_t kTtl = 200'000'000;
  TreeStore store(PackCas::open(dir), kTtl, 0, 0);

  auto first = ingest(store, 0);
  uint64_t size0;
  std::string nar0 = nar_sha256(store, first.root, size0);

  std::vector<IngestResult> roots;
  for (unsigned id = 1; id <= 5; id++)
    roots.push_back(ingest(store, id));
  uint64_t size_before = store.cas().pack_size();

  // age everything past the TTL, keep tarball 0 alive
  usleep(300'000);
  store.touchRoot(first.root);
  auto last = ingest(store, 6);

  uint64_t size = store.cas().pack_size();
  printf("pack size %lu -> %lu after TTL expiry\n",
         static_cast<unsigned long>(size_before),
         static_cast<unsigned long>(size));
  assert(size < size_before);
  assert(store.hasTree(last.root));

  assert(store.hasTree(first.root));
  uint64_t size1;
  assert(nar_sha256(store, first.root, size1) == nar0 && size1 == size0);

  size_t evicted = 0;
  for (auto &r : roots)
    if (!store.hasTree(r.root))
      evicted++;
  printf("evicted %zu of %zu cold roots\n", evicted, roots.size());
  assert(evicted == roots.size());

  auto again = ingest(store, 1);
  assert(store.hasTree(again.root));

  store.sync();
  {
    TreeStore reopened(PackCas::open(dir), kTtl, 0, 0);
    assert(reopened.hasTree(first.root));
    uint64_t size2;
    assert(nar_sha256(reopened, first.root, size2) == nar0);
  }

  printf("gc test ok\n");
  return 0;
}
