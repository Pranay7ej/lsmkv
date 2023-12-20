#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <map>
#include <random>
#include <thread>

#include "../src/file.h"
#include "lsmkv/db.h"

using namespace lsmkv;

namespace {

std::string Key(int i) {
  char buf[32];
  snprintf(buf, sizeof buf, "key%08d", i);
  return buf;
}

class DBTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/lsmkv_dbXXXXXX";
    dir_ = mkdtemp(tmpl);
    // Tiny buffers and files so tests exercise flushes and multi-level compaction.
    opt_.write_buffer_size = 64 * 1024;
    opt_.target_file_size = 32 * 1024;
    opt_.max_bytes_for_level_base = 128 * 1024;
    opt_.block_size = 1024;
    Reopen();
  }
  void TearDown() override {
    db_.reset();
    DestroyDB(dir_);
  }
  void Reopen() {
    db_.reset();
    Status s = DB::Open(opt_, dir_, &db_);
    ASSERT_TRUE(s.ok()) << s.ToString();
  }
  std::string Get(std::string_view k, const Snapshot* snap = nullptr) {
    ReadOptions ro;
    ro.snapshot = snap;
    std::string v;
    Status s = db_->Get(ro, k, &v);
    if (s.IsNotFound()) return "NOT_FOUND";
    if (!s.ok()) return "ERROR: " + s.ToString();
    return v;
  }
  std::map<std::string, std::string> Scan(const Snapshot* snap = nullptr) {
    ReadOptions ro;
    ro.snapshot = snap;
    std::map<std::string, std::string> out;
    auto it = db_->NewIterator(ro);
    std::string prev;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      EXPECT_TRUE(out.empty() || std::string(it->key()) > prev) << "iterator out of order";
      prev = std::string(it->key());
      out[prev] = std::string(it->value());
    }
    EXPECT_TRUE(it->status().ok()) << it->status().ToString();
    return out;
  }
  int FilesAtLevel(int l) {
    std::string v;
    db_->GetProperty("lsmkv.num-files-at-level" + std::to_string(l), &v);
    return std::stoi(v);
  }
  int TotalTableFiles() {
    int n = 0;
    for (int l = 0; l < 7; ++l) n += FilesAtLevel(l);
    return n;
  }

  std::string dir_;
  Options opt_;
  std::unique_ptr<DB> db_;
};

}  // namespace

TEST_F(DBTest, PutGetDelete) {
  EXPECT_EQ(Get("a"), "NOT_FOUND");
  ASSERT_TRUE(db_->Put("a", "1").ok());
  ASSERT_TRUE(db_->Put("b", "2").ok());
  EXPECT_EQ(Get("a"), "1");
  ASSERT_TRUE(db_->Put("a", "3").ok());
  EXPECT_EQ(Get("a"), "3");
  ASSERT_TRUE(db_->Delete("a").ok());
  EXPECT_EQ(Get("a"), "NOT_FOUND");
  EXPECT_EQ(Get("b"), "2");
  ASSERT_TRUE(db_->Put("", "empty key works").ok());
  EXPECT_EQ(Get(""), "empty key works");
}

TEST_F(DBTest, SurvivesReopenFromWalOnly) {
  for (int i = 0; i < 100; ++i) ASSERT_TRUE(db_->Put(Key(i), "v" + std::to_string(i)).ok());
  ASSERT_TRUE(db_->Delete(Key(50)).ok());
  Reopen();  // never flushed: everything comes back from the log
  EXPECT_EQ(Get(Key(0)), "v0");
  EXPECT_EQ(Get(Key(99)), "v99");
  EXPECT_EQ(Get(Key(50)), "NOT_FOUND");
  Reopen();
  EXPECT_EQ(Scan().size(), 99u);
}

TEST_F(DBTest, WriteBatchIsAtomicAndOrdered) {
  WriteBatch b;
  b.Put("x", "1");
  b.Put("y", "2");
  b.Delete("x");
  b.Put("z", "3");
  ASSERT_TRUE(db_->Write(&b).ok());
  EXPECT_EQ(Get("x"), "NOT_FOUND");  // later ops in a batch win
  EXPECT_EQ(Get("y"), "2");
  Reopen();
  EXPECT_EQ(Get("z"), "3");
  EXPECT_EQ(Get("x"), "NOT_FOUND");
}

TEST_F(DBTest, FlushesAndCompactsIntoLevels) {
  const std::string value(200, 'x');
  for (int i = 0; i < 5000; ++i) ASSERT_TRUE(db_->Put(Key(i % 3000), value + std::to_string(i)).ok());
  ASSERT_TRUE(db_->CompactAll().ok());
  EXPECT_EQ(FilesAtLevel(0), 0);
  EXPECT_GT(TotalTableFiles(), 1);
  for (int i = 0; i < 3000; ++i) {
    const int last = i < 2000 ? i + 3000 : i;  // keys 0..1999 were overwritten once
    ASSERT_EQ(Get(Key(i)), value + std::to_string(last)) << i;
  }
  std::string stats;
  ASSERT_TRUE(db_->GetProperty("lsmkv.stats", &stats));
  EXPECT_NE(stats.find("write amplification"), std::string::npos);
}

TEST_F(DBTest, CompactionDropsDeletedData) {
  const std::string value(500, 'y');
  for (int i = 0; i < 2000; ++i) ASSERT_TRUE(db_->Put(Key(i), value).ok());
  ASSERT_TRUE(db_->CompactAll().ok());
  uint64_t before = 0;
  std::vector<std::string> names;
  GetChildren(dir_, &names);
  for (auto& n : names) {
    uint64_t sz;
    if (n.find(".sst") != std::string::npos && GetFileSize(dir_ + "/" + n, &sz).ok()) before += sz;
  }
  for (int i = 0; i < 2000; ++i) ASSERT_TRUE(db_->Delete(Key(i)).ok());
  ASSERT_TRUE(db_->CompactAll().ok());
  EXPECT_TRUE(Scan().empty());
  EXPECT_EQ(TotalTableFiles(), 0) << "tombstones at the bottom level should be dropped";
  EXPECT_GT(before, 500u * 1000);
}

TEST_F(DBTest, SnapshotsSeeTheirPointInTime) {
  ASSERT_TRUE(db_->Put("k", "old").ok());
  ASSERT_TRUE(db_->Put("gone", "here").ok());
  const Snapshot* snap = db_->GetSnapshot();
  ASSERT_TRUE(db_->Put("k", "new").ok());
  ASSERT_TRUE(db_->Delete("gone").ok());
  ASSERT_TRUE(db_->Put("added", "later").ok());

  // Push everything through flush and compaction; the snapshot must keep the old versions alive.
  for (int i = 0; i < 3000; ++i) ASSERT_TRUE(db_->Put(Key(i), std::string(100, 'z')).ok());
  ASSERT_TRUE(db_->CompactAll().ok());

  EXPECT_EQ(Get("k", snap), "old");
  EXPECT_EQ(Get("gone", snap), "here");
  EXPECT_EQ(Get("added", snap), "NOT_FOUND");
  EXPECT_EQ(Get("k"), "new");
  EXPECT_EQ(Get("gone"), "NOT_FOUND");
  auto at_snap = Scan(snap);
  EXPECT_EQ(at_snap.size(), 2u);
  EXPECT_EQ(at_snap["k"], "old");

  db_->ReleaseSnapshot(snap);
  ASSERT_TRUE(db_->CompactAll().ok());
  EXPECT_EQ(Get("k"), "new");
}

TEST_F(DBTest, IteratorSeekAndMergeAcrossLevels) {
  // Spread versions of the same keys across memtable, L0 and deeper levels.
  for (int i = 0; i < 1000; i += 2) ASSERT_TRUE(db_->Put(Key(i), "a").ok());
  ASSERT_TRUE(db_->CompactAll().ok());
  for (int i = 1; i < 1000; i += 2) ASSERT_TRUE(db_->Put(Key(i), "b").ok());
  ASSERT_TRUE(db_->Flush().ok());
  for (int i = 0; i < 1000; i += 10) ASSERT_TRUE(db_->Put(Key(i), "c").ok());
  for (int i = 5; i < 1000; i += 10) ASSERT_TRUE(db_->Delete(Key(i)).ok());

  auto all = Scan();
  EXPECT_EQ(all.size(), 900u);
  EXPECT_EQ(all[Key(0)], "c");
  EXPECT_EQ(all[Key(2)], "a");
  EXPECT_EQ(all[Key(3)], "b");
  EXPECT_EQ(all.count(Key(5)), 0u);

  auto it = db_->NewIterator(ReadOptions());
  it->Seek(Key(500));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), Key(500));
  it->Seek("key00000505");  // deleted
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), Key(506));
  it->Seek("zzz");
  EXPECT_FALSE(it->Valid());
}

TEST_F(DBTest, IteratorIsStableWhileDbChanges) {
  for (int i = 0; i < 500; ++i) ASSERT_TRUE(db_->Put(Key(i), "v1").ok());
  auto it = db_->NewIterator(ReadOptions());
  for (int i = 0; i < 500; ++i) ASSERT_TRUE(db_->Put(Key(i), "v2").ok());
  for (int i = 0; i < 3000; ++i) ASSERT_TRUE(db_->Put(Key(1000 + i), std::string(100, 'q')).ok());
  ASSERT_TRUE(db_->CompactAll().ok());  // deletes the files the iterator started on... unless pinned
  int n = 0;
  for (it->SeekToFirst(); it->Valid() && it->key() < Key(500); it->Next(), ++n) ASSERT_EQ(it->value(), "v1");
  EXPECT_EQ(n, 500);
  EXPECT_TRUE(it->status().ok());
}

TEST_F(DBTest, RandomOpsMatchAModel) {
  std::mt19937 rng(42);
  std::map<std::string, std::string> model;
  for (int step = 0; step < 30000; ++step) {
    const std::string k = Key(int(rng() % 2000));
    const int op = int(rng() % 10);
    if (op < 6) {
      std::string v = std::to_string(step) + std::string(rng() % 200, 'v');
      ASSERT_TRUE(db_->Put(k, v).ok());
      model[k] = v;
    } else if (op < 8) {
      ASSERT_TRUE(db_->Delete(k).ok());
      model.erase(k);
    } else if (op < 9) {
      auto m = model.find(k);
      ASSERT_EQ(Get(k), m == model.end() ? "NOT_FOUND" : m->second) << "step " << step;
    } else if (rng() % 500 == 0) {
      Reopen();
    }
  }
  EXPECT_EQ(Scan(), model);
  Reopen();
  EXPECT_EQ(Scan(), model);
  ASSERT_TRUE(db_->CompactAll().ok());
  EXPECT_EQ(Scan(), model);
}

TEST_F(DBTest, ConcurrentReadersDuringWrites) {
  for (int i = 0; i < 1000; ++i) ASSERT_TRUE(db_->Put(Key(i), "0").ok());
  std::atomic<bool> stop{false};
  std::atomic<int> errors{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 3; ++t) {
    readers.emplace_back([&, t] {
      std::mt19937 rng(t);
      while (!stop) {
        std::string v;
        Status s = db_->Get(Key(int(rng() % 1000)), &v);
        // Values only ever go up; every key always exists.
        if (!s.ok() || v.empty()) errors++;
      }
    });
  }
  for (int round = 1; round <= 20; ++round)
    for (int i = 0; i < 1000; ++i) ASSERT_TRUE(db_->Put(Key(i), std::to_string(round) + std::string(64, '.')).ok());
  stop = true;
  for (auto& r : readers) r.join();
  EXPECT_EQ(errors.load(), 0);
}

TEST_F(DBTest, SecondOpenOfSameDirectoryFails) {
  std::unique_ptr<DB> other;
  Status s = DB::Open(opt_, dir_, &other);
  EXPECT_FALSE(s.ok());
  EXPECT_NE(s.ToString().find("locked"), std::string::npos);
}

TEST_F(DBTest, CrashInTheMiddleOfTheLogKeepsAPrefix) {
  // Write batches, then chop the WAL at every possible length: recovery must
  // always give exactly the state after some prefix of the batches.
  std::vector<std::map<std::string, std::string>> states(1);
  for (int b = 0; b < 20; ++b) {
    WriteBatch batch;
    auto next = states.back();
    for (int j = 0; j < 3; ++j) {
      const std::string k = Key((b * 7 + j) % 25);
      if ((b + j) % 4 == 0) {
        batch.Delete(k);
        next.erase(k);
      } else {
        batch.Put(k, "b" + std::to_string(b));
        next[k] = "b" + std::to_string(b);
      }
    }
    ASSERT_TRUE(db_->Write(&batch).ok());
    states.push_back(next);
  }
  db_.reset();

  std::vector<std::string> names;
  GetChildren(dir_, &names);
  std::string log;
  for (auto& n : names)
    if (n.size() > 4 && n.substr(n.size() - 4) == ".log") {
      std::string data;
      ReadFileToString(dir_ + "/" + n, &data);
      if (data.size() > log.size()) log = n;
    }
  ASSERT_FALSE(log.empty());
  std::string full;
  ASSERT_TRUE(ReadFileToString(dir_ + "/" + log, &full).ok());

  const std::string pristine = dir_ + "_pristine";
  CreateDirIfMissing(pristine);
  for (auto& n : names) {
    std::string d;
    ReadFileToString(dir_ + "/" + n, &d);
    WriteStringToFileSync(pristine + "/" + n, d);
  }

  for (size_t cut = 0; cut <= full.size(); cut += 7) {
    db_.reset();
    DestroyDB(dir_);
    CreateDirIfMissing(dir_);
    for (auto& n : names) {
      std::string d;
      ReadFileToString(pristine + "/" + n, &d);
      WriteStringToFileSync(dir_ + "/" + n, n == log ? d.substr(0, cut) : d);
    }
    Reopen();
    auto got = Scan();
    bool matches_prefix = false;
    for (auto& st : states) matches_prefix |= (got == st);
    ASSERT_TRUE(matches_prefix) << "cut at " << cut;
  }
  DestroyDB(pristine);
}
