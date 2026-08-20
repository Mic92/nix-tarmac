// multi-process safety: concurrent writers must not lose data, and a
// SIGKILLed writer must never corrupt the store or lose synced batches
#include "tree.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

int data_sync(int fd) {
#ifdef __APPLE__
  return fcntl(fd, F_BARRIERFSYNC);
#else
  return fdatasync(fd);
#endif
}

std::string blob_for(unsigned child, unsigned i) {
  std::string b = "child-" + std::to_string(child) + "-" + std::to_string(i);
  b.resize(100 + (i * 37) % 5000, static_cast<char>('a' + child));
  return b;
}

void concurrent_writers(const std::string &dir) {
  constexpr unsigned kChildren = 4, kBlobs = 500;
  for (unsigned c = 0; c < kChildren; c++) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
      auto cas = PackCas::open(dir);
      for (unsigned i = 0; i < kBlobs; i++) {
        (void) cas->put(blob_for(c, i));
        if (i % 100 == 0)
          cas->sync();
      }
      cas->sync();
      _exit(0);
    }
  }
  for (unsigned c = 0; c < kChildren; c++) {
    int st;
    assert(wait(&st) > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
  }
  auto cas = PackCas::open(dir);
  std::string out;
  for (unsigned c = 0; c < kChildren; c++)
    for (unsigned i = 0; i < kBlobs; i++)
      assert(cas->get(blake3_hash(blob_for(c, i)), out) &&
             out == blob_for(c, i));
  printf("concurrent writers ok\n");
}

// child appends the batch's hashes to the log only after sync() returned,
// so everything in the log must survive a SIGKILL
void killer(const std::string &dir, const std::string &log, uint64_t seed) {
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    FILE *f = fopen(log.c_str(), "a");
    assert(f);
    auto cas = PackCas::open(dir);
    std::mt19937_64 rng(seed);
    std::vector<std::string> batch;
    for (unsigned i = 0;; i++) {
      std::string data = "crash-" + std::to_string(seed) + "-" +
                         std::to_string(i) + "-" +
                         std::to_string(rng());
      data.resize(100 + rng() % 8000, 'x');
      batch.push_back(cas->put(data));
      if (batch.size() >= 25) {
        cas->sync();
        for (auto &h : batch)
          fprintf(f, "%s\n", to_hex(h).c_str());
        fflush(f);
        assert(data_sync(fileno(f)) == 0);
        batch.clear();
      }
    }
  }
  std::mt19937_64 rng(seed ^ 0xdead);
  usleep(2000 + rng() % 60000);
  kill(pid, SIGKILL);
  int st;
  assert(waitpid(pid, &st, 0) == pid && WIFSIGNALED(st));
}

void crash_recovery(const std::string &dir) {
  std::string log = dir + "/../crash-log";
  remove(log.c_str());
  for (uint64_t round = 0; round < 20; round++)
    killer(dir, log, round);

  auto cas = PackCas::open(dir);
  FILE *f = fopen(log.c_str(), "r");
  assert(f);
  char line[128];
  size_t n = 0;
  std::string out;
  while (fgets(line, sizeof(line), f)) {
    assert(strlen(line) == 65);
    assert(cas->get(from_hex({line, 64}), out));
    n++;
  }
  fclose(f);
  assert(n > 0);
  printf("crash recovery: %zu synced hashes survived 20 kills\n", n);
}

} // namespace

int main(int argc, char **argv) {
  std::string dir = argc > 1 ? argv[1] : "/tmp/packcas-crash-test";
  std::string cmd = "rm -rf " + dir;
  if (system(cmd.c_str()) != 0)
    return 1;
  concurrent_writers(dir);
  crash_recovery(dir);
  return 0;
}
