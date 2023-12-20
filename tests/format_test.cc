// Building blocks: coding, crc, skiplist, blocks, bloom filters, tables, the log.
#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <random>
#include <set>
#include <thread>

#include "../src/block.h"
#include "../src/bloom.h"
#include "../src/coding.h"
#include "../src/crc32c.h"
#include "../src/dbformat.h"
#include "../src/log.h"
#include "../src/memtable.h"
#include "../src/skiplist.h"
#include "../src/table.h"

using namespace lsmkv;

namespace {

std::string TempDir() {
  char tmpl[] = "/tmp/lsmkv_fmtXXXXXX";
  return mkdtemp(tmpl);
}

std::string IKey(std::string_view user, SequenceNumber seq, ValueType t = kTypeValue) {
  std::string k;
  AppendInternalKey(&k, user, seq, t);
  return k;
}

}  // namespace

TEST(Coding, VarintRoundTrip) {
  std::string buf;
  const uint64_t values[] = {0, 1, 127, 128, 300, 1u << 21, (1ull << 35) + 7, ~0ull};
  for (uint64_t v : values) PutVarint64(&buf, v);
  std::string_view in(buf);
  for (uint64_t v : values) {
    uint64_t got;
    ASSERT_TRUE(GetVarint64(&in, &got));
    EXPECT_EQ(got, v);
  }
  EXPECT_TRUE(in.empty());
  std::string_view truncated("\x80\x80", 2);
  uint64_t v;
  EXPECT_FALSE(GetVarint64(&truncated, &v));
}

TEST(Coding, Crc32cKnownValue) {
  // Standard check value for CRC-32C.
  EXPECT_EQ(crc32c::Value("123456789", 9), 0xE3069283u);
  const uint32_t c = crc32c::Value("hello", 5);
  EXPECT_EQ(crc32c::Unmask(crc32c::Mask(c)), c);
  EXPECT_NE(crc32c::Mask(c), c);
}

TEST(InternalKey, NewerSequenceSortsFirst) {
  EXPECT_LT(CompareInternalKey(IKey("a", 5), IKey("a", 3)), 0);
  EXPECT_LT(CompareInternalKey(IKey("a", 1), IKey("b", 9)), 0);
  EXPECT_LT(CompareInternalKey(IKey("a", 5, kTypeValue), IKey("a", 5, kTypeDeletion)), 0);
  ParsedInternalKey p;
  const std::string k = IKey("key", 42, kTypeDeletion);  // p points into it
  ASSERT_TRUE(ParseInternalKey(k, &p));
  EXPECT_EQ(p.user_key, "key");
  EXPECT_EQ(p.sequence, 42u);
  EXPECT_EQ(p.type, kTypeDeletion);
}

namespace {
struct IntCmp {
  int operator()(uint64_t a, uint64_t b) const { return a < b ? -1 : (a > b ? 1 : 0); }
};
}  // namespace

TEST(SkipList, InsertAndIterateInOrder) {
  Arena arena;
  SkipList<uint64_t, IntCmp> list(IntCmp(), &arena);
  std::set<uint64_t> model;
  std::mt19937_64 rng(1);
  for (int i = 0; i < 5000; ++i) {
    uint64_t k = rng() % 100000;
    if (model.insert(k).second) list.Insert(k);
  }
  SkipList<uint64_t, IntCmp>::Iterator it(&list);
  it.SeekToFirst();
  for (uint64_t k : model) {
    ASSERT_TRUE(it.Valid());
    EXPECT_EQ(it.key(), k);
    it.Next();
  }
  EXPECT_FALSE(it.Valid());
  it.Seek(50000);
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), *model.lower_bound(50000));
}

TEST(SkipList, ReadersSeeConsistentListWhileWriting) {
  Arena arena;
  SkipList<uint64_t, IntCmp> list(IntCmp(), &arena);
  std::atomic<uint64_t> inserted{0};
  std::atomic<bool> done{false};
  std::thread writer([&] {
    for (uint64_t i = 1; i <= 20000; ++i) {
      list.Insert(i * 2);  // strictly increasing, so readers can check for gaps
      inserted.store(i, std::memory_order_release);
    }
    done = true;
  });
  int rounds = 0;
  while (!done || rounds < 3) {
    const uint64_t at_least = inserted.load(std::memory_order_acquire);
    SkipList<uint64_t, IntCmp>::Iterator it(&list);
    uint64_t expect = 2, n = 0;
    for (it.SeekToFirst(); it.Valid(); it.Next(), expect += 2, ++n) ASSERT_EQ(it.key(), expect);
    ASSERT_GE(n, at_least);  // everything inserted before we started is visible
    ++rounds;
  }
  writer.join();
}

TEST(MemTable, NewestVisibleVersionWins) {
  MemTable mem;
  mem.Add(1, kTypeValue, "k", "v1");
  mem.Add(2, kTypeValue, "k", "v2");
  mem.Add(3, kTypeDeletion, "k", "");
  mem.Add(4, kTypeValue, "other", "x");
  std::string v;
  bool deleted;
  ASSERT_TRUE(mem.Get("k", 1, &v, &deleted));
  EXPECT_FALSE(deleted);
  EXPECT_EQ(v, "v1");
  ASSERT_TRUE(mem.Get("k", 2, &v, &deleted));
  EXPECT_EQ(v, "v2");
  ASSERT_TRUE(mem.Get("k", 10, &v, &deleted));
  EXPECT_TRUE(deleted);
  EXPECT_FALSE(mem.Get("missing", 10, &v, &deleted));
  EXPECT_FALSE(mem.Get("other", 3, &v, &deleted));  // written after the snapshot
}

TEST(Block, PrefixCompressedRoundTripAndSeek) {
  BlockBuilder b(4);
  std::vector<std::string> keys;
  for (int i = 0; i < 200; ++i) {
    char buf[32];
    snprintf(buf, sizeof buf, "user%06d", i * 3);
    keys.push_back(IKey(buf, 100));
    b.Add(keys.back(), "val" + std::to_string(i));
  }
  auto contents = std::make_shared<const std::string>(b.Finish());
  // Same entries with a restart at every key = no prefix compression at all.
  BlockBuilder plain(1);
  for (size_t i = 0; i < keys.size(); ++i) plain.Add(keys[i], "val" + std::to_string(i));
  EXPECT_LT(contents->size() * 10, size_t(plain.Finish().size()) * 8) << "expected >20% smaller";

  auto it = NewBlockIterator(contents);
  int i = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++i) {
    EXPECT_EQ(it->key(), keys[size_t(i)]);
    EXPECT_EQ(it->value(), "val" + std::to_string(i));
  }
  EXPECT_EQ(i, 200);
  it->Seek(IKey("user000301", kMaxSequenceNumber));  // between 300 and 303
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(ExtractUserKey(it->key()), "user000303");
  it->Seek(IKey("zzz", kMaxSequenceNumber));
  EXPECT_FALSE(it->Valid());
  it->Seek(IKey("a", kMaxSequenceNumber));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), keys[0]);
}

TEST(Block, CorruptTrailerIsReported) {
  auto it = NewBlockIterator(std::make_shared<const std::string>("\x05\x00\x00\x00", 4));
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
}

TEST(Bloom, NoFalseNegativesAndLowFalsePositives) {
  BloomBuilder b(10);
  for (int i = 0; i < 10000; ++i) b.AddKey("key" + std::to_string(i));
  const std::string f = b.Finish();
  for (int i = 0; i < 10000; ++i) ASSERT_TRUE(BloomMayContain(f, "key" + std::to_string(i)));
  int fp = 0;
  for (int i = 0; i < 10000; ++i) fp += BloomMayContain(f, "missing" + std::to_string(i));
  // Theory says ~0.8% at 10 bits/key.
  EXPECT_LT(fp, 200) << "false positive rate " << fp / 100.0 << "%";
}

class TableTest : public ::testing::Test {
 protected:
  void SetUp() override { dir_ = TempDir(); }
  void TearDown() override {
    std::remove((dir_ + "/t.sst").c_str());
    rmdir(dir_.c_str());
  }
  std::string dir_;
};

TEST_F(TableTest, WriteThenReadEverything) {
  Options opt;
  opt.block_size = 256;  // lots of blocks
  std::unique_ptr<WritableFile> wf;
  ASSERT_TRUE(WritableFile::Open(dir_ + "/t.sst", &wf).ok());
  TableBuilder tb(opt, std::move(wf));
  std::map<std::string, std::string, InternalKeyLess> model;
  for (int i = 0; i < 3000; ++i) {
    char buf[32];
    snprintf(buf, sizeof buf, "k%07d", i * 2);
    model[IKey(buf, uint64_t(i) + 1)] = std::string(size_t(i % 50), 'v');
  }
  for (auto& [k, v] : model) tb.Add(k, v);
  ASSERT_TRUE(tb.Finish().ok());

  std::unique_ptr<RandomAccessFile> rf;
  ASSERT_TRUE(RandomAccessFile::Open(dir_ + "/t.sst", &rf).ok());
  BlockCache cache(1 << 20);
  std::unique_ptr<Table> t;
  ASSERT_TRUE(Table::Open(opt, std::move(rf), 7, &cache, &t).ok());

  auto it = t->NewIterator(ReadOptions());
  auto m = model.begin();
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++m) {
    ASSERT_NE(m, model.end());
    ASSERT_EQ(it->key(), m->first);
    ASSERT_EQ(it->value(), m->second);
  }
  EXPECT_EQ(m, model.end());
  EXPECT_TRUE(it->status().ok());

  // Point lookups: present keys hit, absent ones are mostly stopped by bloom.
  int found = 0;
  for (int i = 0; i < 3000; i += 7) {
    char buf[32];
    snprintf(buf, sizeof buf, "k%07d", i * 2);
    ASSERT_TRUE(t->Get(ReadOptions(), IKey(buf, kMaxSequenceNumber), [&](std::string_view k, std::string_view) {
                   found += ExtractUserKey(k) == buf;
                 }).ok());
  }
  EXPECT_EQ(found, (3000 + 6) / 7);
  for (int i = 0; i < 1000; ++i) {
    char buf[32];
    snprintf(buf, sizeof buf, "k%07d", i * 2 + 1);  // odd: never written
    t->Get(ReadOptions(), IKey(buf, kMaxSequenceNumber), [](std::string_view, std::string_view) {});
  }
  EXPECT_GT(t->bloom_useful(), 950u);
  EXPECT_GT(cache.hits(), 0u);
}

TEST_F(TableTest, FlippedByteIsCaughtByChecksum) {
  Options opt;
  std::unique_ptr<WritableFile> wf;
  ASSERT_TRUE(WritableFile::Open(dir_ + "/t.sst", &wf).ok());
  TableBuilder tb(opt, std::move(wf));
  for (int i = 0; i < 100; ++i) tb.Add(IKey("key" + std::to_string(1000 + i), 1), "value");
  ASSERT_TRUE(tb.Finish().ok());

  std::string data;
  ASSERT_TRUE(ReadFileToString(dir_ + "/t.sst", &data).ok());
  data[10] ^= 0x40;  // inside the first data block
  ASSERT_TRUE(WriteStringToFileSync(dir_ + "/t.sst", data).ok());

  std::unique_ptr<RandomAccessFile> rf;
  ASSERT_TRUE(RandomAccessFile::Open(dir_ + "/t.sst", &rf).ok());
  std::unique_ptr<Table> t;
  ASSERT_TRUE(Table::Open(opt, std::move(rf), 1, nullptr, &t).ok());
  auto it = t->NewIterator(ReadOptions());
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
}

TEST(Log, TornTailIsDroppedQuietly) {
  const std::string dir = TempDir();
  const std::string path = dir + "/x.log";
  {
    std::unique_ptr<WritableFile> f;
    ASSERT_TRUE(WritableFile::Open(path, &f).ok());
    LogWriter w(std::move(f));
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(w.AddRecord("record-" + std::to_string(i)).ok());
    ASSERT_TRUE(w.Close().ok());
  }
  std::string data;
  ASSERT_TRUE(ReadFileToString(path, &data).ok());

  // Chop the file mid-record, as a crash during write would.
  LogReader torn(data.substr(0, data.size() - 3));
  std::string_view rec;
  int n = 0;
  while (torn.ReadRecord(&rec)) EXPECT_EQ(rec, "record-" + std::to_string(n++));
  EXPECT_EQ(n, 9);
  EXPECT_GT(torn.dropped_bytes(), 0u);
  EXPECT_FALSE(torn.hit_bad_checksum());

  // A flipped bit in the middle stops the log there and is flagged.
  std::string bad = data;
  bad[data.size() / 2] ^= 1;
  LogReader corrupt(bad);
  n = 0;
  while (corrupt.ReadRecord(&rec)) ++n;
  EXPECT_LT(n, 10);
  EXPECT_TRUE(corrupt.hit_bad_checksum());

  std::remove(path.c_str());
  rmdir(dir.c_str());
}
