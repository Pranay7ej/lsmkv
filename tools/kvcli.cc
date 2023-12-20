// Tiny command line client, mostly for poking at a DB by hand.
//
//   kvcli <dir> put <key> <value>
//   kvcli <dir> get <key>
//   kvcli <dir> del <key>
//   kvcli <dir> scan [start] [limit]
//   kvcli <dir> compact
//   kvcli <dir> stats
#include <cstdio>
#include <cstdlib>
#include <string>

#include "lsmkv/db.h"

using namespace lsmkv;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <dir> put|get|del|scan|compact|stats ...\n", argv[0]);
    return 2;
  }
  std::unique_ptr<DB> db;
  Status s = DB::Open(Options(), argv[1], &db);
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.ToString().c_str());
    return 1;
  }
  const std::string cmd = argv[2];
  if (cmd == "put" && argc == 5) {
    s = db->Put(argv[3], argv[4]);
  } else if (cmd == "get" && argc == 4) {
    std::string v;
    s = db->Get(argv[3], &v);
    if (s.ok()) std::printf("%s\n", v.c_str());
  } else if (cmd == "del" && argc == 4) {
    s = db->Delete(argv[3]);
  } else if (cmd == "scan") {
    auto it = db->NewIterator(ReadOptions());
    int limit = argc > 4 ? std::atoi(argv[4]) : 100;
    if (argc > 3) it->Seek(argv[3]);
    else it->SeekToFirst();
    for (; it->Valid() && limit-- > 0; it->Next())
      std::printf("%.*s = %.*s\n", int(it->key().size()), it->key().data(), int(it->value().size()),
                  it->value().data());
    s = it->status();
  } else if (cmd == "compact") {
    s = db->CompactAll();
  } else if (cmd == "stats") {
    std::string out;
    db->GetProperty("lsmkv.stats", &out);
    std::printf("%s", out.c_str());
  } else {
    std::fprintf(stderr, "bad command\n");
    return 2;
  }
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.ToString().c_str());
    return 1;
  }
  return 0;
}
