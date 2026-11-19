// multi-process safety: concurrent writers must not lose data, and a
// SIGKILLed writer must never corrupt the store or lose synced batches
#include "test_tmp.hpp"

#include "tree.hpp"

#include <fcntl.h>
#include <lmdb.h>
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

// A process killed inside a read txn leaves its reader slot, and LMDB
// reuses no page freed after that snapshot: a cache with one such slot
// filled its 32 GiB index map with 26.7 GiB of free pages it could not
// reuse (MDB_MAP_FULL). After such a death, 5,000 writes must keep the
// index's high-water page low: record writes (their own write txns) and
// batches, each with its own dead reader.
template <typename Each>
size_t pages_after_dead_reader(const std::string &dir, Each each) {
  const std::string index = dir + "/index";
  auto cas = PackCas::open(dir);
  int ready[2];
  assert(pipe(ready) == 0);
  const pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    MDB_env *env = nullptr;
    MDB_txn *txn = nullptr;
    if (mdb_env_create(&env) != 0 ||
        mdb_env_open(env, index.c_str(), MDB_RDONLY, 0644) != 0 ||
        mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn) != 0)
      _exit(1);
    assert(write(ready[1], "x", 1) == 1);
    pause();
  }
  close(ready[1]); // a child that fails closes the pipe: the read fails
  char token = 0;
  assert(read(ready[0], &token, 1) == 1);
  kill(pid, SIGKILL);
  assert(waitpid(pid, nullptr, 0) == pid);
  for (unsigned i = 0; i < 5000; i++)
    each(*cas, std::to_string(i));
  cas.reset(); // LMDB opens an environment once per process
  MDB_env *env = nullptr;
  MDB_envinfo info;
  assert(mdb_env_create(&env) == 0 &&
         mdb_env_open(env, index.c_str(), MDB_RDONLY | MDB_NOLOCK, 0644) == 0 &&
         mdb_env_info(env, &info) == 0);
  mdb_env_close(env);
  return info.me_last_pgno + 1;
}

void dead_reader_cleared(const std::string &root) {
  const size_t records = pages_after_dead_reader(
      root + "/dead-reader-records",
      [](PackCas &cas, const std::string &i) { cas.meta_put("k", i); });
  const size_t batches = pages_after_dead_reader(
      root + "/dead-reader-batches", [](PackCas &cas, const std::string &i) {
        (void)cas.put(i);
        cas.sync();
      });
  printf("dead reader: index high-water %zu pages after records, %zu after "
         "batches\n", records, batches);
  fflush(stdout);
  assert(records < 1024);
  assert(batches < 1024);
}

int main(int argc, char **argv) {
  std::string dir = argc > 1 ? argv[1] : make_test_dir("packcas-crash-test");
  concurrent_writers(dir);
  crash_recovery(dir);
  dead_reader_cleared(dir);
  return 0;
}
