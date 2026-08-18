// randomized model test: PackCas vs in-memory map, with reopen cycles
#include "pack_cas.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

int main(int argc, char **argv) {
  std::string dir = argc > 1 ? argv[1] : "/tmp/packcas-model-test";
  uint64_t seed = argc > 2 ? std::stoull(argv[2]) : 1;
  std::string cmd = "rm -rf " + dir;
  if (system(cmd.c_str()) != 0)
    return 1;

  std::mt19937_64 rng(seed);
  std::unordered_map<std::string, std::string> model;
  std::vector<std::string> keys;      // sampling; may contain erased keys
  std::unordered_set<std::string> unsynced; // only these can be lost on crash
  auto cas = PackCas::open(dir);

  auto sample_key = [&]() -> const std::string * {
    for (int tries = 0; tries < 8 && !keys.empty(); tries++) {
      auto &k = keys[rng() % keys.size()];
      if (model.count(k))
        return &k;
    }
    return nullptr;
  };

  auto random_blob = [&] {
    size_t len = rng() % 3 ? rng() % 512 : rng() % 100000;
    std::string b(len, '\0');
    bool text = rng() % 2;
    for (size_t i = 0; i < len; i += 8) {
      uint64_t r = text ? rng() % 4 : rng();
      memcpy(b.data() + i, &r, std::min<size_t>(8, len - i));
    }
    return b;
  };

  for (int op = 0; op < 200000; op++) {
    switch (rng() % 10) {
    case 0:
    case 1:
    case 2: {
      std::string data = random_blob();
      std::string hash = cas->put(data);
      assert(hash == blake3_hash(data));
      if (model.emplace(hash, data).second)
        keys.push_back(hash);
      unsynced.insert(hash);
      break;
    }
    case 3:
    case 4:
    case 5: {
      auto *k = sample_key();
      if (!k)
        break;
      std::string out;
      assert(cas->get(*k, out));
      assert(out == model[*k]);
      std::string scratch;
      std::string_view v;
      assert(cas->get_view(*k, scratch, v));
      assert(v == model[*k]);
      uint64_t n;
      assert(cas->size(*k, n) && n == model[*k].size());
      break;
    }
    case 6: { // miss
      std::string fake(32, '\0');
      uint64_t r[4] = {rng(), rng(), rng(), rng()};
      memcpy(fake.data(), r, 32);
      std::string out;
      assert(!cas->get(fake, out));
      assert(!cas->has(fake));
      break;
    }
    case 7:
      cas->sync();
      unsynced.clear();
      break;
    case 8: { // reopen: synced data must survive
      cas->sync();
      unsynced.clear();
      cas.reset();
      cas = PackCas::open(dir);
      break;
    }
    case 9: { // drop without sync: batch may be lost, must not corrupt
      cas.reset();
      cas = PackCas::open(dir);
      for (auto &k : unsynced) {
        std::string out;
        if (!cas->get(k, out))
          model.erase(k);
        else
          assert(out == model[k]);
      }
      unsynced.clear();
      break;
    }
    }
  }
  // final: everything in the model must be intact
  cas->sync();
  for (auto &[hash, data] : model) {
    std::string out;
    assert(cas->get(hash, out) && out == data);
  }
  printf("model test ok: %zu blobs\n", model.size());
  return 0;
}
