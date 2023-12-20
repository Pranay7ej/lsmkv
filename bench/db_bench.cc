// Rough throughput numbers, in the style of LevelDB's db_bench.
//
//   db_bench [--num N] [--value_size B] [--bloom_bits K] [--sync] [--dir PATH]
//
// Runs: fillseq, fillrandom, overwrite, readrandom, readmissing, readseq,
// then prints the level stats (including write amplification).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "lsmkv/db.h"

using namespace lsmkv;
using Clock = std::chrono::steady_clock;

namespace {

int g_num = 1000000;
int g_value_size = 100;
int g_bloom_bits = 10;
bool g_sync = false;
std::string g_dir = "/tmp/lsmkv_bench";

std::string Key(uint64_t i) {
  char buf[32];
  snprintf(buf, sizeof buf, "%016llu", static_cast<unsigned long long>(i));
  return buf;
}

struct Result {
  const char* name;
  int ops;
  double secs;
  double mb;
};

void Report(const Result& r) {
  std::printf("%-12s : %8.3f micros/op  %9.0f ops/s", r.name, r.secs * 1e6 / r.ops, r.ops / r.secs);
  if (r.mb > 0) std::printf("  %7.1f MB/s", r.mb / r.secs);
  std::printf("\n");
  std::fflush(stdout);
}

std::unique_ptr<DB> Open(bool fresh) {
  if (fresh) DestroyDB(g_dir);
  Options o;
  o.bloom_bits_per_key = g_bloom_bits;
  o.sync = g_sync;
  std::unique_ptr<DB> db;
  Status s = DB::Open(o, g_dir, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "open: %s\n", s.ToString().c_str());
    std::exit(1);
  }
  return db;
}

Result Fill(DB* db, const char* name, bool random, int n) {
  std::mt19937_64 rng(301);
  std::string value(size_t(g_value_size), 'x');
  for (auto& c : value) c = char('a' + rng() % 26);  // not trivially compressible, not that we compress
  auto t0 = Clock::now();
  WriteBatch batch;
  for (int i = 0; i < n; ++i) {
    const uint64_t k = random ? rng() % uint64_t(g_num) : uint64_t(i);
    Status s = db->Put(Key(k), value);
    if (!s.ok()) {
      std::fprintf(stderr, "put: %s\n", s.ToString().c_str());
      std::exit(1);
    }
  }
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  return {name, n, secs, double(n) * (16 + g_value_size) / 1048576.0};
}

Result ReadRandom(DB* db, const char* name, bool missing, int n, int* found_out) {
  std::mt19937_64 rng(7);
  std::string v;
  int found = 0;
  auto t0 = Clock::now();
  for (int i = 0; i < n; ++i) {
    // Missing keys sort in between the real ones, so they hit real files and
    // only the bloom filter can skip the block read.
    std::string k = Key(rng() % uint64_t(g_num));
    if (missing) k += ".";
    found += db->Get(k, &v).ok();
  }
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  *found_out = found;
  return {name, n, secs, 0};
}

Result ReadSeq(DB* db) {
  auto t0 = Clock::now();
  auto it = db->NewIterator(ReadOptions());
  int n = 0;
  size_t bytes = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++n) bytes += it->key().size() + it->value().size();
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  return {"readseq", n, secs, double(bytes) / 1048576.0};
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    auto arg = [&](const char* name) { return std::strcmp(argv[i], name) == 0 && i + 1 < argc; };
    if (arg("--num")) g_num = std::atoi(argv[++i]);
    else if (arg("--value_size")) g_value_size = std::atoi(argv[++i]);
    else if (arg("--bloom_bits")) g_bloom_bits = std::atoi(argv[++i]);
    else if (arg("--dir")) g_dir = argv[++i];
    else if (std::strcmp(argv[i], "--sync") == 0) g_sync = true;
    else {
      std::fprintf(stderr, "usage: %s [--num N] [--value_size B] [--bloom_bits K] [--sync] [--dir PATH]\n", argv[0]);
      return 2;
    }
  }
  std::printf("keys: 16 bytes, values: %d bytes, entries: %d, bloom bits/key: %d, sync: %s\n", g_value_size, g_num,
              g_bloom_bits, g_sync ? "yes" : "no");

  {
    auto db = Open(true);
    Report(Fill(db.get(), "fillseq", false, g_num));
  }
  auto db = Open(true);
  Report(Fill(db.get(), "fillrandom", true, g_num));
  Report(Fill(db.get(), "overwrite", true, g_num));
  db->CompactAll();

  int found = 0;
  const int reads = std::min(g_num, 200000);
  Report(ReadRandom(db.get(), "readrandom", false, reads, &found));
  std::printf("               (%d of %d found; fillrandom leaves ~37%% of the key space empty)\n", found, reads);
  Report(ReadRandom(db.get(), "readmissing", true, reads, &found));
  Report(ReadSeq(db.get()));

  std::string stats;
  db->GetProperty("lsmkv.stats", &stats);
  std::printf("\n%s", stats.c_str());
  db.reset();
  DestroyDB(g_dir);
  return 0;
}
