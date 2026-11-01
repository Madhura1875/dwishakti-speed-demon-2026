// Track C: join records.csv with regions.csv and aggregate per country.
//
// Every thread parses its own slice of records.csv in chunks. A slice starts at
// a record boundary: a newline followed (after an optional byte order mark) by
// a digit, which quoted text never contains. Names come from a small pool, so
// all threads share one hash table of the distinct names; each name has a bit
// per country, and a country's distinct names are the names with its bit set.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <sched.h>
#include <string>
#include <string_view>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace std;
typedef uint64_t u64;
typedef int64_t i64;

struct Agg {
  u64 count = 0, idSum = 0, nameBytes = 0, noteBytes = 0;
  i64 sum = 0, lo = INT64_MAX, hi = INT64_MIN;
};

static vector<uint16_t> region; // region id -> group index; 0 is _UNMATCHED
static u64 *tab;                // name table: per slot the name's hash, then W words
static size_t cap, W, used;     // slots, bit words per slot, names so far
static bool full;               // the table got too full: redo with a bigger one

// Marks the name with hash h as present in group g, one bit per group.
static void seen(u64 h, size_t g) {
  if (__atomic_load_n(&full, __ATOMIC_RELAXED))
    return;
  h |= 1ull << 63; // 0 means an empty slot
  u64 *s;
  for (size_t i = h & (cap - 1);; i = (i + 1) & (cap - 1)) {
    u64 k = __atomic_load_n(s = tab + i * (W + 1), __ATOMIC_RELAXED);
    if (!k && __atomic_compare_exchange_n(s, &k, h, 0, 0, 0)) {
      if (__atomic_add_fetch(&used, 1, __ATOMIC_RELAXED) * 10 > cap * 9)
        __atomic_store_n(&full, true, __ATOMIC_RELAXED);
      break;
    }
    if (k == h)
      break;
  }
  u64 &w = s[1 + g / 64], b = 1ull << g % 64;
  if (!(__atomic_load_n(&w, __ATOMIC_RELAXED) & b))
    __atomic_fetch_or(&w, b, __ATOMIC_RELAXED);
}

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
    a.count++;
    a.idSum += id;
    a.sum += cents;
    a.lo = min(a.lo, cents);
    a.hi = max(a.hi, cents);
    a.nameBytes += name.size();
    a.noteBytes += note.size() - tesc;
    seen(hash<string_view>()(name), g);
    return p;
  }
};

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
  FILE *rf = fopen((dataDir + "/regions.csv").c_str(), "r");
  size_t id;
  char cc[4];
  fscanf(rf, "%*[^\n]");
  while (fscanf(rf, "%zu,%3s", &id, cc) == 2) {
    if (id >= region.size())
      region.resize(id + 1);
    region[id] = groups.emplace(cc, groups.size()).first->second;
  }
  size_t ng = groups.size();
  W = (ng + 63) / 64;

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

  vector<Worker> ws;
  const size_t CH = 1 << 18,
               MARGIN = 1 << 16; // a record is far shorter than MARGIN
  for (cap = (1 << 17);; cap *= 4) { // a name pool of 100000 fits the first size
    size_t bytes = (cap * (W + 1) * 8 + (2 << 20) - 1) & -(2 << 20);
    tab = (u64 *)mmap(0, bytes + (2 << 20), PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    tab = (u64 *)((uintptr_t(tab) + (2 << 20) - 1) & -(2 << 20)); // 2 MB aligned
    madvise(tab, bytes, MADV_HUGEPAGE); // random access: fewer TLB misses
    used = full = 0;
    ws.assign(n, Worker{vector<Agg>(ng), {}});
    vector<thread> threads;
    auto work = [&](int i) {
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
    };
    for (int i = 1; i < n; i++)
      threads.emplace_back(work, i);
    work(0);
    for (thread &t : threads)
      t.join();
    if (!full)
      break;
    munmap(tab, bytes);
  }
  close(fd);

  // Merge the threads' aggregates; a group's distinct names are the names
  // whose bit for that group is set.
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
  for (size_t i = 0; i < cap; i++)
    for (size_t w = 0; tab[i * (W + 1)] && w < W; w++)
      for (u64 b = tab[i * (W + 1) + 1 + w]; b; b &= b - 1)
        distinct[w * 64 + __builtin_ctzll(b)]++;

  filesystem::create_directories(outDir);
  FILE *out = fopen((outDir + "/aggregates.txt").c_str(), "wb");
  if (!out)
    return fprintf(stderr, "Cannot create aggregates.txt\n"), 1;
  for (auto &[code, g] : groups) {
    const Agg &a = total[g];
    if (a.count)
      fprintf(out, "%s %lu %lu %s %s %s %lu %lu %lu\n", code.c_str(), a.count,
              a.idSum, money(a.sum).c_str(), money(a.lo).c_str(),
              money(a.hi).c_str(), distinct[g], a.nameBytes, a.noteBytes);
  }
  fclose(out);
}