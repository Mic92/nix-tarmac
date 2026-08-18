// corruption test: truncated/flipped pack must error or misread, never crash
#include "pack_cas.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <random>
#include <vector>

int main(int argc, char **argv) {
  std::string dir = argc > 1 ? argv[1] : "/tmp/packcas-corrupt-test";
  std::string cmd = "rm -rf " + dir;
  if (system(cmd.c_str()) != 0)
    return 1;

  std::mt19937_64 rng(7);
  std::vector<std::string> hashes;
  {
    auto cas = PackCas::open(dir);
    for (int i = 0; i < 1000; i++) {
      std::string data(rng() % 4096, char('a' + i % 26));
      hashes.push_back(cas->put(data));
    }
    cas->sync();
  }

  // flip random bytes in the pack: gets must not crash
  {
    int fd = open((dir + "/pack.0").c_str(), O_RDWR);
    assert(fd >= 0);
    off_t size = lseek(fd, 0, SEEK_END);
    for (int i = 0; i < 100; i++) {
      char c = rng();
      if (pwrite(fd, &c, 1, rng() % size) != 1)
        return 1;
    }
    close(fd);
    auto cas = PackCas::open(dir);
    std::string out;
    size_t rejected = 0;
    for (auto &h : hashes) {
      try {
        (void) cas->get(h, out);
      } catch (const CorruptError &) {
        rejected++;
      }
    }
    printf("bit flips: %zu rejected\n", rejected);
  }

  // truncate the pack: out-of-bounds entries must throw, not SIGBUS
  {
    if (truncate((dir + "/pack.0").c_str(), 100) != 0)
      return 1;
    auto cas = PackCas::open(dir);
    std::string out;
    size_t errors = 0, ok = 0;
    for (auto &h : hashes) {
      try {
        if (cas->get(h, out))
          ok++;
      } catch (std::exception &) {
        errors++;
      }
    }
    assert(errors > 0);
    printf("corrupt test ok: %zu bounds errors, %zu reads\n", errors, ok);
  }

  // truncation while the store is open: get() must throw, never SIGBUS
  {
    std::string dir2 = dir + "-live";
    std::string cmd2 = "rm -rf " + dir2;
    if (system(cmd2.c_str()) != 0)
      return 1;
    auto cas = PackCas::open(dir2);
    std::vector<std::string> hs;
    for (int i = 0; i < 100; i++)
      hs.push_back(cas->put(std::string(3000, char('a' + i % 26))));
    cas->sync();
    if (truncate((dir2 + "/pack.0").c_str(), 50) != 0)
      return 1;
    std::string out;
    size_t errors = 0;
    for (auto &h : hs) {
      try {
        (void) cas->get(h, out);
      } catch (std::exception &) {
        errors++;
      }
    }
    assert(errors > 0);
    printf("live truncation ok: %zu errors\n", errors);
  }

  // trashed index: open() must wipe and start fresh
  {
    std::string dir3 = dir + "-heal";
    std::string cmd3 = "rm -rf " + dir3;
    if (system(cmd3.c_str()) != 0)
      return 1;
    {
      auto cas = PackCas::open(dir3);
      (void) cas->put("hello");
      cas->sync();
    }
    int fd = open((dir3 + "/index/data.mdb").c_str(), O_WRONLY);
    assert(fd >= 0);
    std::string junk(4096, char(0x5a));
    assert(pwrite(fd, junk.data(), junk.size(), 0) > 0);
    close(fd);
    auto cas = PackCas::open(dir3);
    std::string h = cas->put("fresh start");
    std::string out;
    assert(cas->get(h, out) && out == "fresh start");
    printf("self-heal ok\n");
  }
  return 0;
}
