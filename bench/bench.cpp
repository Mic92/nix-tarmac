// usage: bench <dir> <tarball>...
#include "tree.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

using clk = std::chrono::steady_clock;

static double now() {
  return std::chrono::duration<double>(clk::now().time_since_epoch()).count();
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <dir> <tarball>...\n", argv[0]);
    return 1;
  }
  TreeStore store(PackCas::open(argv[1]));

  std::string root;
  for (int i = 2; i < argc; i++) {
    double t0 = now();
    auto res = ingest_tarball_file(store, argv[i]);
    double dt = now() - t0;
    printf("ingest     %8llu files in %6.2fs (%.0f files/s)\n",
           static_cast<unsigned long long>(res.files), dt,
           res.files / dt);
    root = res.root;
  }

  std::vector<std::string> paths;
  {
    std::vector<std::pair<std::string, std::string>> stack{{"", root}};
    while (!stack.empty()) {
      auto [prefix, id] = stack.back();
      stack.pop_back();
      for (auto &e : store.readTree(id)) {
        std::string p = prefix.empty() ? e.name : prefix + "/" + e.name;
        if (e.type == 'd')
          stack.emplace_back(p, e.id);
        else
          paths.push_back(std::move(p));
      }
    }
  }

  TreeWalker walker{store};
  for (int round = 0; round < 2; round++) {
    std::mt19937_64 rng(42);
    auto order = paths;
    std::shuffle(order.begin(), order.end(), rng);
    TreeEntry e;
    double t0 = now();
    for (auto &p : order)
      if (!walker.lookup(root, p, e))
        return fprintf(stderr, "miss: %s\n", p.c_str()), 1;
    double dt = now() - t0;
    printf("lstat-%s %8zu paths in %6.2fs (%.0f lookups/s)\n",
           round == 0 ? "cold" : "warm", order.size(), dt, order.size() / dt);
  }

  {
    std::mt19937_64 rng(7);
    size_t n = std::min<size_t>(20000, paths.size());
    TreeEntry e;
    uint64_t bytes = 0;
    std::string scratch;
    std::string_view v;
    double t0 = now();
    for (size_t i = 0; i < n; i++)
      if (walker.lookup(root, paths[rng() % paths.size()], e) &&
          e.type != 'd' && store.readBlobView(e.id, scratch, v))
        bytes += v.size();
    double dt = now() - t0;
    printf("readFile   %8zu reads in %6.2fs (%.0f reads/s, %.1f MB/s)\n", n,
           dt, n / dt, bytes / dt / 1e6);
  }

  {
    uint64_t nar_size;
    double t0 = now();
    nar_sha256(store, root, nar_size);
    double dt = now() - t0;
    printf("nar-dump   %8.1f MB in %6.2fs (%.0f MB/s)\n", nar_size / 1e6, dt,
           nar_size / 1e6 / dt);
  }
  return 0;
}
