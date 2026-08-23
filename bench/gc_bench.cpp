// usage: gc_bench <dir> [roots]
// Cost of the paths the eval store added to GC. A touch storm is what
// a warm run pays when every root is older than the touch interval.
// The compaction pass is what an unlucky process pays at startup.
#include "tree.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using clk = std::chrono::steady_clock;

namespace {
auto now() -> double {
  return std::chrono::duration<double>(clk::now().time_since_epoch()).count();
}
} // namespace

auto main(int argc, char **argv) -> int {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <dir> [roots]\n", argv[0]);
    return 1;
  }
  const size_t nroots = argc > 2 ? strtoul(argv[2], nullptr, 10) : 10000;

  // wide enough that the keep-alive touch loop finishes within it
  constexpr auto kTtl = std::chrono::seconds(2);
  constexpr auto kNoDelay = std::chrono::nanoseconds(0);
  TreeStore store(PackCas::open(argv[1]), kTtl, kNoDelay, kNoDelay);

  std::vector<std::string> roots;
  roots.reserve(nroots);
  double t0 = now();
  for (size_t i = 0; i < nroots; i++) {
    // ~4 KB per blob, in the drv text ballpark
    std::string data(4096, static_cast<char>('a' + i % 26));
    data += std::to_string(i);
    auto id = store.putBlob(data);
    store.registerRoot(id, false);
    roots.push_back(std::move(id));
  }
  store.sync();
  printf("ingest     %8zu roots in %6.2fs\n", nroots, now() - t0);

  // zero touch interval forces the timestamp write on every call
  t0 = now();
  for (const auto &id : roots) {
    store.touchRoot(id, false);
  }
  store.sync();
  double dt = now() - t0;
  printf("touch all  %8zu roots in %6.2fs (%.0f/s)\n", nroots, dt, nroots / dt);

  // age everything past the TTL, keep half alive
  sleep(3);
  for (size_t i = 0; i < nroots / 2; i++) {
    store.touchRoot(roots[i], false);
  }
  store.sync();
  t0 = now();
  store.maybeGc();
  printf("gc sweep   %8zu roots in %6.2fs (half expired)\n", nroots,
         now() - t0);

  std::thread writer([&store] {
    for (int i = 0; i < 500; i++) {
      store.putBlob("filler-" + std::to_string(i));
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    store.sync();
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  double worst = 0;
  for (int i = 0; i < 10; i++) {
    t0 = now();
    store.touchRoot(roots[i], false);
    worst = std::max(worst, now() - t0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  writer.join();
  printf("touch during open write batch: worst %.2f ms\n", worst * 1e3);

  size_t alive = 0;
  for (const auto &id : roots) {
    uint64_t size = 0;
    alive += store.blobSize(id, size) ? 1 : 0;
  }
  printf("alive      %8zu of %zu\n", alive, nroots);
  return alive == nroots / 2 ? 0 : 1;
}
