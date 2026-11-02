// Track C: join records.csv with regions.csv and aggregate per country.
//
// Every thread parses its own slice of records.csv in chunks. A slice starts at
// a record boundary: a newline followed (after an optional byte order mark) by
// a digit, which quoted text never contains. Per country a thread keeps count,
// id sum, money sum/min/max and byte totals. Distinct names are counted without
// keeping a hash per record: a shared lock-free table turns a name's 64-bit hash
// into a small dense id, each thread lists the ids it saw per country (4 bytes
// each), and at the end one small bitset per country (it fits in L1) counts them.
// Each record is finished a few steps after it is parsed, so that the table slot
// and the region entry it needs can be prefetched in the meantime.
#include <algorithm>
#include <atomic>
#include <sys/mman.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

static inline u64 hashBytes(const char *p, size_t n) {
  u64 h = 0x9E3779B97F4A7C15ULL ^ n;
  while (n >= 8) {
    u64 w;
    memcpy(&w, p, 8);
    h = (h ^ w) * 0xff51afd7ed558ccdULL;
    h ^= h >> 32;
    p += 8, n -= 8;
  }
  u64 w = 0;
  memcpy(&w, p, n);
  h = (h ^ w) * 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 29;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 32;
  return h;
}

// Distinct names: a shared lock-free table maps a name's 64-bit hash to a small dense id (two different names
// would need the same 64 bits, which is not going to happen with about 100,000 names). Each thread keeps, per
// country, the ids of its records' names; the ids are 4 bytes instead of an 8-byte hash.
constexpr size_t TAB = 1 << 18;  // table slots
constexpr unsigned ID_BITS = 18;
static atomic<u64> *tab;
static atomic<uint32_t> nextId{1};

static inline uint32_t idOf(u64 h) {
  const u64 tag = h >> 24;  // 40 bits; the slot uses the low bits, so the two are independent
  size_t i = h & (TAB - 1);
  for (;;) {
    u64 e = tab[i].load(memory_order_relaxed);
    if (!e) {
      uint32_t fresh = nextId.fetch_add(1, memory_order_relaxed);
      if (tab[i].compare_exchange_strong(e, tag << 24 | fresh, memory_order_relaxed)) return fresh;
    }
    if (e >> 24 == tag) return e & ((1u << ID_BITS) - 1);
    i = (i + 1) & (TAB - 1);
  }
}

struct Agg {
  u64 count = 0, idSum = 0, nameBytes = 0, noteBytes = 0;
  uint32_t *last = nullptr;
  i64 sum = 0, lo = INT64_MAX, hi = INT64_MIN;

  // Ids go into a linked list of 1 KB blocks (the first 8 bytes link to the previous block, then 254 ids), so
  // growing never copies memory. Blocks are kept until exit. Adding also counts a record.
  static constexpr u64 PER = 254;
  void add(uint32_t id) {
    if (count % PER == 0) {
      uint32_t *b = (uint32_t *)malloc(256 * 4);
      *(uint32_t **)b = last, last = b;
    }
    last[2 + count++ % PER] = id;
  }
  template <class F> void each(F f) const {  // every block but the last is full
    u64 left = count;
    for (uint32_t *b = last; b; b = *(uint32_t **)b) {
      u64 inBlock = (left - 1) % PER + 1;
      for (u64 k = 0; k < inBlock; k++) f(b[2 + k]);
      left -= inBlock;
    }
  }
};

static vector<uint16_t> region;

struct Pend { u64 id, h; i64 cents; uint32_t nameLen, noteLen, rid; };

struct alignas(64) Worker {
  vector<Agg> agg;
  string tmp;
  // Records are finished a few steps after they are parsed, so that the table slot and the region entry they
  // need can be prefetched meanwhile (they are cache misses otherwise).
  static constexpr unsigned DEPTH = 8;
  Pend ring[DEPTH];
  unsigned head = 0, count = 0;

  void finish(const Pend &r) {
    size_t g = r.rid < region.size() ? region[r.rid] : 0;
    Agg &a = agg[g];
    a.idSum += r.id;
    a.sum += r.cents;
    a.lo = min(a.lo, r.cents);
    a.hi = max(a.hi, r.cents);
    a.nameBytes += r.nameLen;
    a.noteBytes += r.noteLen;
    a.add(idOf(r.h));
  }
  void push(const Pend &r) {
    if (r.rid < region.size()) __builtin_prefetch(&region[r.rid]);
    __builtin_prefetch(&tab[r.h & (TAB - 1)]);
    if (count == DEPTH) {
      finish(ring[head]);
      ring[head] = r;
      head = (head + 1) % DEPTH;
    } else
      ring[(head + count++) % DEPTH] = r;
  }
  void flush() {
    for (; count; count--, head = (head + 1) % DEPTH) finish(ring[head]);
  }
  static const char *field(const char *p, const char *e, string_view &v, size_t &esc) {
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
    if (nesc) {
      tmp.clear();
      for (size_t i = 0; i < name.size(); i++)
        tmp += name[i], i += name[i] == '"';
      name = tmp;
    }
    push({id, hashBytes(name.data(), name.size()), cents, (uint32_t)name.size(), (uint32_t)(note.size() - tesc), (uint32_t)rid});
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
  snprintf(b, sizeof b, "%s%lld.%02lld", v < 0 ? "-" : "", (long long)llabs(v) / 100, (long long)llabs(v) % 100);
  return b;
}

int main(int argc, char *argv[]) {
  string dataDir = argc > 1 ? argv[1] : "/data", outDir = argc > 2 ? argv[2] : "/out";
  map<string, size_t> groups{{"_UNMATCHED", 0}};
  {
    int rf = open((dataDir + "/regions.csv").c_str(), O_RDONLY);
    if (rf < 0) return fprintf(stderr, "Cannot open regions.csv\n"), 1;
    off_t rs = lseek(rf, 0, SEEK_END);
    vector<char> b(rs + 8, 0);
    for (off_t got = 0; got < rs;) {
      ssize_t r = pread(rf, &b[got], rs - got, got);
      if (r <= 0) break;
      got += r;
    }
    close(rf);
    vector<uint16_t> idx(26 * 26 * 26, 0);
    vector<string> codes;
    const char *p = (const char *)memchr(b.data(), '\n', rs);
    const char *e = b.data() + rs;
    p = p ? p + 1 : e;
    while (p < e) {
      size_t id = 0;
      while (*p != ',') id = id * 10 + (*p++ - '0');
      ++p;
      unsigned k = ((p[0] - 'A') * 26 + (p[1] - 'A')) * 26 + (p[2] - 'A');
      if (!idx[k]) {
        codes.emplace_back(p, 3);
        idx[k] = codes.size();
      }
      if (id >= region.size()) region.resize(max(id + 1, region.size() * 2));
      region[id] = idx[k];
      p = (const char *)memchr(p, '\n', e - p);
      p = p ? p + 1 : e;
    }
    for (size_t i = 0; i < codes.size(); i++) groups.emplace(codes[i], i + 1);
  }
  size_t ng = groups.size();
  tab = (atomic<u64> *)mmap(nullptr, TAB * 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  int fd = open((dataDir + "/records.csv").c_str(), O_RDONLY);
  if (fd < 0) return fprintf(stderr, "Cannot open records.csv\n"), 1;
  off_t size = lseek(fd, 0, SEEK_END);
  cpu_set_t cpus;
  sched_getaffinity(0, sizeof cpus, &cpus);
#ifdef NTHREADS
  int n = NTHREADS;
#else
  int n = clamp(CPU_COUNT(&cpus), 1, 64);
#endif
  n = (int)min<off_t>(n, max<off_t>(1, size >> 20));
  auto boundary = [&](off_t pos) -> off_t {
    char b[1 << 16];
    ssize_t got = pread(fd, b, sizeof b, pos);
    for (ssize_t k = 0; k + 4 < got; k++)
      if (b[k] == '\n' && (uint8_t)(b[k + 1 + 3 * !memcmp(b + k + 1, "\xEF\xBB\xBF", 3)] - '0') < 10)
        return pos + k + 1;
    return size;
  };
  vector<off_t> bounds(n + 1, size);
  for (int i = 0; i < n; i++)
    bounds[i] = max<off_t>(boundary(i ? size * i / n - 1 : 0), i ? bounds[i - 1] : 0);
  vector<Worker> ws(n, Worker{vector<Agg>(ng), {}});
  const size_t CH = 1 << 18, MARGIN = 1 << 16;
  parallel(n, [&](int i) {
    int tfd = open((dataDir + "/records.csv").c_str(), O_RDONLY);
    if (tfd < 0) tfd = fd;
    vector<char> buf(CH + 64);
    off_t pos = bounds[i], end = bounds[i + 1];
    size_t have = 0;
    for (bool last = false; !last;) {
      ssize_t got = max<ssize_t>(pread(tfd, &buf[have], min<off_t>(CH - have, end - pos), pos), 0);
      pos += got;
      have += got;
      last = !got || pos >= end;
      memset(&buf[have], 0, 8);
      const char *p = buf.data(), *e = p + have, *limit = last ? e : have > MARGIN ? e - MARGIN : p;
      while (p < limit) p = ws[i].record(p, e);
      have = e - p;
      memmove(buf.data(), p, have);
    }
    ws[i].flush();
    if (tfd != fd) close(tfd);
  });
  close(fd);
  vector<Agg> total(ng);
  for (Worker &w : ws)
    for (size_t g = 0; g < ng; g++) {
      Agg &t = total[g], &a = w.agg[g];
      t.count += a.count; t.idSum += a.idSum; t.sum += a.sum;
      t.lo = min(t.lo, a.lo); t.hi = max(t.hi, a.hi);
      t.nameBytes += a.nameBytes; t.noteBytes += a.noteBytes;
    }
  // A country's distinct names: mark every id in one small bitset (it fits in L1), then count the bits.
  vector<u64> distinct(ng);
  vector<size_t> order(ng);
  for (size_t g = 0; g < ng; g++) order[g] = g;
  sort(order.begin(), order.end(), [&](size_t x, size_t y) { return total[x].count > total[y].count; });
  atomic<size_t> nextGroup{0};
  const size_t words = (nextId.load() >> 6) + 1;
  parallel(n, [&](int) {
    vector<u64> bits(words);
    for (size_t k; (k = nextGroup.fetch_add(1)) < ng;) {
      size_t g = order[k];
      for (Worker &w : ws) w.agg[g].each([&](uint32_t id) { bits[id >> 6] |= 1ull << (id & 63); });
      for (u64 &x : bits) distinct[g] += __builtin_popcountll(x), x = 0;
    }
  });

  filesystem::create_directories(outDir);
  ofstream out(outDir + "/aggregates.txt", ios::binary);
  if (!out) return fprintf(stderr, "Cannot create aggregates.txt\n"), 1;
  for (auto &[code, g] : groups) {
    const Agg &a = total[g];
    if (a.count)
      out << code << ' ' << a.count << ' ' << a.idSum << ' ' << money(a.sum) << ' ' << money(a.lo) << ' ' << money(a.hi) << ' ' << distinct[g] << ' ' << a.nameBytes << ' ' << a.noteBytes << '\n';
  }
}