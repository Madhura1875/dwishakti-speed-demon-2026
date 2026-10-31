// Track C: join records.csv with regions.csv and aggregate per country.
//
// Every thread parses its own slice of records.csv in chunks. A slice starts at
// a record boundary: a newline followed (after an optional byte order mark) by
// a digit, which quoted text never contains. Per country a thread keeps count,
// id sum, money sum/min/max, byte totals and a hash of every name; the distinct
// names are counted at the end by inserting those hashes into a small set.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <sched.h>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>
using namespace std;
typedef uint64_t u64;
typedef int64_t i64;

struct Agg {
  u64 count = 0, idSum = 0, nameBytes = 0, noteBytes = 0, *last = nullptr;
  i64 sum = 0, lo = INT64_MAX, hi = INT64_MIN;

  // The hash of every record's name goes into a linked list of blocks of 127
  // (slot 0 links to the previous block), so growing never copies or
  // fragments memory. Blocks are kept until exit. Adding also counts a record.
  void add(u64 h) {
    if (count % 127 == 0) {
      u64 *b = (u64 *)malloc(128 * 8);
      *b = (u64)last, last = b;
    }
    last[1 + count++ % 127] = h;
  }
  template <class F> void each(F f) const { // every block but the last is full
    for (u64 *b = last, left = count; b;
         left -= (left - 1) % 127 + 1, b = (u64 *)*b)
      for (u64 k = 0; k <= (left - 1) % 127; k++)
        f(b[1 + k]);
  }
};

static vector<uint16_t> region; // region id -> group index; 0 is _UNMATCHED

struct Worker {
  vector<Agg> agg;
  string tmp;

  // Reads the text field at p, quoted or not, and returns the comma after it.
  // esc counts the "" pairs inside.
  static const char *field(const char *p, const char *e, string_view &v,
                           size_t &esc) {
    bool quoted = *p == '"';
    const char *s = p += quoted;
    esc = 0;
    if (!quoted) {
      while (*p != ',')
        p++;
    } else {
      while ((p = (const char *)memchr(p, '"', e - p))[1] == '"')
        p += 2, esc++;
    }
    v = {s, size_t(p - s)};
    return p + quoted;
  }

  // Parses the record at p (a complete record ends before e) and returns where
  // the next one starts.
  const char *record(const char *p, const char *e) {
    if (!memcmp(p, "\xEF\xBB\xBF", 3))
      p += 3;
    u64 id = 0, rid = 0;
    while (*p != ',')
      id = id * 10 + (*p++ - '0');
    string_view name, note;
    size_t nesc, tesc;
    p = field(p + 1, e, name, nesc);
    p = field(p + 1, e, note, tesc);
    for (p++; *p != ','; p++)
      rid = rid * 10 + (*p - '0');
    bool neg = p[1] == '-';
    i64 cents = 0;
    for (p += 1 + neg; *p != '.'; p++)
      cents = cents * 10 + (*p - '0');
    cents = (cents * 100 + (p[1] - '0') * 10 + (p[2] - '0')) * (neg ? -1 : 1);
    p += 3;
    p += *p == '\r';
    p += p < e && *p == '\n';

    if (nesc) { // unescape "" so that a name hashes the same however it was
                // written
      tmp.clear();
      for (size_t i = 0; i < name.size(); i++)
        tmp += name[i], i += name[i] == '"';
      name = tmp;
    }
    size_t g = rid < region.size() ? region[rid] : 0;
    Agg &a = agg[g];
    a.idSum += id;
    a.sum += cents;
    a.lo = min(a.lo, cents);
    a.hi = max(a.hi, cents);
    a.nameBytes += name.size();
    a.noteBytes += note.size() - tesc;
    a.add(hash<string_view>()(name));
    return p;
  }
};

template <class F> static void parallel(int n, F f) {
  vector<thread> workers;
  for (int i = 1; i < n; i++)
    workers.emplace_back(f, i);
  f(0);
  for (thread &w : workers)
    w.join();
}

static string money(i64 v) {
  char b[32];
  snprintf(b, sizeof b, "%s%lld.%02lld", v < 0 ? "-" : "",
           (long long)llabs(v) / 100, (long long)llabs(v) % 100);
  return b;
}

int main(int argc, char *argv[]) {
  string dataDir = argc > 1 ? argv[1] : "/data",
         outDir = argc > 2 ? argv[2] : "/out";

  // regions.csv: every country code is a group; "_UNMATCHED" is group 0 and
  // sorts after the codes.
  map<string, size_t> groups{{"_UNMATCHED", 0}};
  ifstream regions(dataDir + "/regions.csv");
  string line;
  getline(regions, line);
  while (getline(regions, line)) {
    size_t id = stoul(line);
    if (id >= region.size())
      region.resize(id + 1);
    region[id] =
        groups.emplace(line.substr(line.find(',') + 1, 3), groups.size())
            .first->second;
  }
  size_t ng = groups.size();

  int fd = open((dataDir + "/records.csv").c_str(), O_RDONLY);
  if (fd < 0)
    return fprintf(stderr, "Cannot open records.csv\n"), 1;
  off_t size = lseek(fd, 0, SEEK_END);
  cpu_set_t cpus;
  sched_getaffinity(0, sizeof cpus, &cpus);
  int n = clamp(CPU_COUNT(&cpus), 1, 16);
  n = (int)min<off_t>(n, max<off_t>(1, size >> 20));

  // First record start at or after pos.
  auto boundary = [&](off_t pos) -> off_t {
    char b[1 << 16];
    ssize_t got = pread(fd, b, sizeof b, pos);
    for (ssize_t k = 0; k + 4 < got; k++)
      if (b[k] == '\n' &&
          (uint8_t)(b[k + 1 + 3 * !memcmp(b + k + 1, "\xEF\xBB\xBF", 3)] -
                    '0') < 10)
        return pos + k + 1;
    return size;
  };
  // Thread i parses the records starting in [bounds[i], bounds[i + 1]);
  // bounds[0] is just after the header.
  vector<off_t> bounds(n + 1, size);
  for (int i = 0; i < n; i++)
    bounds[i] =
        max<off_t>(boundary(i ? size * i / n - 1 : 0), i ? bounds[i - 1] : 0);

  vector<Worker> ws(n, Worker{vector<Agg>(ng), {}});
  const size_t CH = 1 << 18,
               MARGIN = 1 << 16; // a record is far shorter than MARGIN
  parallel(n, [&](int i) {
    vector<char> buf(CH + 64);
    off_t pos = bounds[i], end = bounds[i + 1];
    size_t have = 0;
    for (bool last = false; !last;) {
      ssize_t got = max<ssize_t>(
          pread(fd, &buf[have], min<off_t>(CH - have, end - pos), pos), 0);
      pos += got;
      have += got;
      last = !got || pos >= end;
      memset(&buf[have], 0, 8);
      const char *p = buf.data(), *e = p + have,
                 *limit = last            ? e
                          : have > MARGIN ? e - MARGIN
                                          : p;
      while (p < limit)
        p = ws[i].record(p, e);
      have = e - p;
      memmove(buf.data(), p, have);
    }
  });
  close(fd);

  // Merge the threads' aggregates and count each group's distinct names.
  vector<Agg> total(ng);
  for (Worker &w : ws)
    for (size_t g = 0; g < ng; g++) {
      Agg &t = total[g], &a = w.agg[g];
      t.count += a.count;
      t.idSum += a.idSum;
      t.sum += a.sum;
      t.lo = min(t.lo, a.lo);
      t.hi = max(t.hi, a.hi);
      t.nameBytes += a.nameBytes;
      t.noteBytes += a.noteBytes;
    }
  vector<u64> distinct(ng);
  parallel(n, [&](int i) {
    vector<u64> set;
    for (size_t g = i; g < ng; g += n) {
      // Open-addressing set of hashes (0 = empty). A big group is split into
      // passes by hash range so that the table stays small.
      size_t parts = total[g].count / 65536 + 1, cap = 16;
      while (cap < total[g].count / parts * 3 / 2)
        cap *= 2;
      for (size_t p = 0; p < parts; p++) {
        set.assign(cap, 0);
        for (Worker &w : ws)
          w.agg[g].each([&](u64 h) {
            if ((h >> 32) * parts >> 32 == p) {
              h |= 1;
              size_t k = (h >> 3) & (cap - 1);
              while (set[k] && set[k] != h)
                k = (k + 1) & (cap - 1);
              distinct[g] += !set[k];
              set[k] = h;
            }
          });
      }
    }
  });

  filesystem::create_directories(outDir);
  ofstream out(outDir + "/aggregates.txt", ios::binary);
  if (!out)
    return fprintf(stderr, "Cannot create aggregates.txt\n"), 1;
  for (auto &[code, g] : groups) {
    const Agg &a = total[g];
    if (a.count)
      out << code << ' ' << a.count << ' ' << a.idSum << ' ' << money(a.sum)
          << ' ' << money(a.lo) << ' ' << money(a.hi) << ' ' << distinct[g]
          << ' ' << a.nameBytes << ' ' << a.noteBytes << '\n';
  }
}