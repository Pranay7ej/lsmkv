# lsmkv

A small key-value storage engine based on a log-structured merge tree, same basic design as LevelDB / RocksDB. Wrote it to actually understand what's going on under databases I use every day instead of just knowing the words.

```cpp
std::unique_ptr<lsmkv::DB> db;
lsmkv::DB::Open(lsmkv::Options(), "/tmp/mydb", &db);

db->Put("apple", "red");
std::string v;
db->Get("apple", &v);

lsmkv::WriteBatch b;          // atomic, even across crashes
b.Put("a", "1");
b.Delete("apple");
db->Write(&b);

auto it = db->NewIterator(lsmkv::ReadOptions());
for (it->Seek("a"); it->Valid(); it->Next()) { ... }
```

## how it works

writes:
1. append the batch to the WAL (crc32c per record)
2. insert into the memtable (skiplist, lock-free reads)
3. when the memtable is full it gets swapped out and a background thread writes it to an L0 sstable

reads check memtable -> old memtable -> L0 files (newest first) -> L1..L6.

- **sstables**: 4 KB data blocks with prefix compression + restart points, an index block, one bloom filter per file (10 bits/key), checksums on every block, LRU block cache
- **MVCC**: every write gets a sequence number. keys are stored as `user_key + (seq << 8 | type)` so the newest version sorts first. snapshots are just a sequence number, and iterators see a consistent view even while compactions delete the files underneath them (files are refcounted through `shared_ptr<Version>`)
- **compaction**: leveled. L0 compacts when it has 4 files, L1 at 10 MB, each level 10x the one above. picks files round robin, drops overwritten versions nobody can see anymore, and drops tombstones once there's nothing older below them. a file with nothing overlapping below just gets moved down, no rewrite
- **manifest**: every change to the file set is a record in a MANIFEST log, `CURRENT` points at it and is swapped with write-temp-then-rename
- **recovery**: replays WALs newer than the last flush. a torn record at the end of the log (crash mid write) is dropped, so you always get some prefix of your writes, never a half applied batch
- **write stalls**: writers wait if the previous memtable hasn't flushed yet or L0 gets to 12 files

## build

```
cmake -S . -B build
cmake --build build -j
ctest --test-dir build

./build/kvcli /tmp/db put hello world
./build/kvcli /tmp/db scan
```

`-DLSMKV_SANITIZE=address` / `thread` for sanitizer builds, CI runs both.

## tests

the ones I care most about:
- random puts/deletes/gets against a `std::map`, with random reopens, then a full compaction, and the DB has to match the map every time
- chop the WAL at every 7th byte and reopen: the result has to equal the state after *some* prefix of the batches
- snapshots keep old versions alive through flush + compaction, and stop doing so once released
- an iterator opened before a full compaction still reads the old data
- flip one byte in an sstable and the checksum catches it
- readers running while a writer pushes data through flushes and compactions (run under TSan in CI)

plus unit tests for the skiplist (including a reader running during inserts), blocks, bloom false positive rate, tables and the log.

## numbers

`db_bench` with 1M entries, 16 byte keys, 100 byte values, no fsync. from the CI bench job (GitHub's ubuntu-24.04 runner):

```
fillseq      :    7.613 micros/op     131356 ops/s     14.5 MB/s
fillrandom   :    5.282 micros/op     189320 ops/s     20.9 MB/s
overwrite    :    7.939 micros/op     125955 ops/s     13.9 MB/s
readrandom   :    7.683 micros/op     130156 ops/s
readmissing  :    0.384 micros/op    2607474 ops/s
readseq      :    0.366 micros/op    2731219 ops/s    302.1 MB/s

write amplification: 3.55
```

same thing without bloom filters:

```
readrandom   :   11.548 micros/op      86593 ops/s
readmissing  :   11.850 micros/op      84387 ops/s
```

so for keys that don't exist the bloom filter makes lookups ~30x faster, since it skips the block read entirely. readrandom also gets faster (7.7 vs 11.5 us), partly because ~37% of those keys were never written (fillrandom leaves gaps) and partly because it skips levels that don't have the key.

write amp of ~3.5 is low because the data only reached L2. it'd grow with more levels. (writes are slower on the CI runner than on my VM, reads faster. disk vs CPU I guess, didn't dig into it.)

## what's missing
- no compression (would add snappy/zstd per block)
- writes are serialized under one mutex, no group commit, so `sync = true` is slow
- one background thread does both flushes and compactions
- iterators are forward only
- no range deletes, no column families, no merge operator
