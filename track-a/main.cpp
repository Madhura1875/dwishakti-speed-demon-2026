// Track A: group 3.1M log lines by (hour, endpoint) and report count / p50 /
// p95 / p99 / 5xx.
//
// Every timestamp lies in the same 72 hours (spec section 2), so an hour is a
// number 0..71 and a group is simply hour * E + rank, where rank is the
// endpoint's position in byte order. Group ids therefore come out already
// sorted: no per-group table, no sort of groups, only one sort of the ~1.8k
// distinct endpoints. Each thread reads its own slice of the file, counts per
// (hour, local endpoint) and keeps (key, latency) records; the records are then
// scattered into one array grouped by group, and the percentiles come from
// there.
//
// Parsing works on 8 bytes at a time: the end of the method and of the endpoint
// is found with a zero-byte test, and the endpoint is hashed in the same pass.
// Everything after the parse (merge, scatter, percentiles, output) is parallel
// as well.
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <sched.h>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>
using namespace std;
typedef uint32_t u32;
typedef uint64_t u64;
constexpr int HOURS = 72;

static int quotaCpus() { // cgroup v2 "max 100000" or "200000 100000"
  FILE *f = fopen("/sys/fs/cgroup/cpu.max", "r");
  if (!f)
    return 1 << 30;
  char q[32] = {0};
  long per = 0;
  int ok = fscanf(f, "%31s %ld", q, &per);
  fclose(f);
  if (ok != 2 || q[0] == 'm' || per <= 0)
    return 1 << 30;
  return (int)max(1L, (atol(q) + per - 1) / per);
}

// 0x80 in the lowest byte of w that holds a space (higher bytes may be flagged
// wrongly, the lowest never is).
static inline u64 spaces(u64 w) {
  u64 x = w ^ 0x2020202020202020ULL;
  return (x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL;
}

// One slice of the file: endpoint dictionary, per-(endpoint, hour) counters and
// the records.
struct Part {
  vector<u32> slots =
      vector<u32>(1 << 12); // open addressing, endpoint id + 1, 0 = empty
  vector<u32> off, len;     // endpoint id -> bytes inside `names`
  vector<u64> hash;         // endpoint id -> hash
  string names;
  vector<u64> ce;   // key = endpoint id * HOURS + hour: count low, errors high
  vector<u64> recs; // key << 32 | latency

  u32 idOf(const char *e, u32 n, u64 h) {
    u32 mask = slots.size() - 1, s = h & mask;
    for (; slots[s]; s = (s + 1) & mask) {
      u32 id = slots[s] - 1;
      if (len[id] == n && !memcmp(&names[off[id]], e, n))
        return id;
    }
    u32 id = off.size();
    off.push_back(names.size());
    len.push_back(n);
    hash.push_back(h);
    names.append(e, n);
    ce.resize(ce.size() + HOURS);
    slots[s] = id + 1;
    if (off.size() * 2 > slots.size()) { // grow and rehash
      vector<u32> bigger(slots.size() * 2);
      bigger.swap(slots);
      for (u32 k = 0; k < off.size(); k++) {
        u32 t = hash[k] & (slots.size() - 1);
        while (slots[t])
          t = (t + 1) & (slots.size() - 1);
        slots[t] = k + 1;
      }
    }
    return id;
  }

  // Whole lines "ts method endpoint status latency\n" in [p, end); at least 8
  // readable bytes follow end.
  void ingest(const char *p, const char *end) {
    while (p < end) {
      // hour 0 = 2026-10-30T18; October has 31 days, so November day d is day
      // 31 + d.
      u32 day = (p[6] == '1' ? 31 : 0) + (p[8] - '0') * 10 + p[9] - '0';
      u32 hour = (day * 24 + (p[11] - '0') * 10 + p[12] - '0') - (30 * 24 + 18);
      u64 w;
      memcpy(&w, p + 28, 8); // method: 3 to 7 bytes, then a space
      p += 28 + (__builtin_ctzll(spaces(w)) >> 3) + 1;
      const char *ep = p;
      u64 h = 0x9E3779B97F4A7C15ull;
      for (;;
           p += 8,
           h ^= h >> 29) { // endpoint: hashed 8 bytes at a time up to the space
        memcpy(&w, p, 8);
        u64 m = spaces(w);
        if (m) {
          u32 k = __builtin_ctzll(m) >> 3;
          h = (h ^ (w & ((1ULL << (8 * k)) - 1))) * 0x9E3779B97F4A7C15ull;
          p += k;
          break;
        }
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
      }
      u32 n = p - ep;
      h = (h ^ n) * 0xC2B2AE3D27D4EB4Full;
      u32 key = idOf(ep, n, h ^ h >> 32) * HOURS + hour;
      ce[key] += 1 + ((u64)(p[1] == '5') << 32);
      u32 lat = 0;
      for (p += 5; *p != '\n'; p++)
        lat = lat * 10 + (*p - '0');
      p++;
      recs.push_back((u64)key << 32 | lat);
    }
  }

  void scan(int fd, off_t pos, off_t end) {
    vector<char> buf((1 << 18) + 16);
    size_t have = 0;
    while (pos < end) {
      ssize_t got = pread(fd, buf.data() + have,
                          min<off_t>((1 << 18) - have, end - pos), pos);
      if (got <= 0)
        break;
      pos += got;
      have += got;
      char *last = (char *)memrchr(buf.data(), '\n', have);
      if (!last)
        continue;
      ingest(buf.data(), last + 1);
      have -= last + 1 - buf.data();
      memmove(buf.data(), last + 1, have);
    }
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

// Writes the values at ranks r50 <= r95 <= r99 of b[0..c) to q.
static inline void pick(u32 *b, u64 c, u64 r50, u64 r95, u64 r99, u32 *q) {
  if (c <= 48) { // insertion sort beats three selection passes here
    for (u64 i = 1; i < c; i++) {
      u32 v = b[i];
      u64 j = i;
      while (j && b[j - 1] > v) {
        b[j] = b[j - 1];
        j--;
      }
      b[j] = v;
    }
  } else {
    // Median first: each later pass only looks at what is not below the
    // previous rank (about 1.55 c elements in total instead of 2.9 c). A rank
    // is read as soon as it is placed, because the next pass may move it.
    nth_element(b, b + r50, b + c);
    q[0] = b[r50];
    nth_element(b + r50, b + r95, b + c);
    q[1] = b[r95];
    nth_element(b + r95, b + r99, b + c);
    q[2] = b[r99];
    return;
  }
  q[0] = b[r50];
  q[1] = b[r95];
  q[2] = b[r99];
}

int main(int argc, char *argv[]) {
  string dataDir = argc > 1 ? argv[1] : "/data",
         outDir = argc > 2 ? argv[2] : "/out";
  int fd = open((dataDir + "/requests.log").c_str(), O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "Cannot open requests.log\n");
    return 1;
  }
  off_t size = lseek(fd, 0, SEEK_END);
  cpu_set_t cpus;
  sched_getaffinity(0, sizeof cpus, &cpus);
#ifdef NTHREADS
  int n = NTHREADS;
#else
  int n = clamp(min(CPU_COUNT(&cpus), quotaCpus()), 1, 16);
#endif

  // Slice i holds the lines that start in [cut[i], cut[i + 1]); each cut is
  // moved to a line start.
  vector<off_t> cut(n + 1, size);
  cut[0] = 0;
  for (int i = 1; i < n; i++) {
    char b[256];
    off_t lo = size * i / n;
    ssize_t got = max<ssize_t>(pread(fd, b, sizeof b, lo - 1), 0);
    if (auto nl = (const char *)memchr(b, '\n', got))
      cut[i] = lo + (nl - b);
  }
  vector<Part> parts(n);
  parallel(n, [&](int i) {
    parts[i].recs.reserve((cut[i + 1] - cut[i]) / 40 +
                          1); // a line has at least 40 bytes
    parts[i].scan(fd, cut[i], cut[i + 1]);
  });
  close(fd);

  // Global endpoint list in byte order; loc[i][r] is slice i's local id of the
  // r-th endpoint, or -1.
  vector<string_view> eps;
  for (Part &p : parts)
    for (size_t e = 0; e < p.off.size(); e++)
      eps.emplace_back(&p.names[p.off[e]], p.len[e]);
  sort(eps.begin(), eps.end());
  eps.erase(unique(eps.begin(), eps.end()), eps.end());
  size_t E = eps.size(), G = E * HOURS;
  vector<vector<int>> loc(n);
  vector<vector<u32>> cursor(
      n); // cursor[i][key]: where slice i writes its next latency of that group
  parallel(n, [&](int i) {
    Part &p = parts[i];
    loc[i].assign(E, -1);
    cursor[i].resize(p.ce.size());
    for (size_t e = 0; e < p.off.size(); e++)
      loc[i][lower_bound(eps.begin(), eps.end(),
                         string_view(&p.names[p.off[e]], p.len[e])) -
             eps.begin()] = e;
  });
  // Ranks [lo, hi) of the endpoints belong to one thread; groups of different
  // ranks never share a slot.
  auto ranks = [&](int t) {
    return pair<size_t, size_t>(E * t / n, E * (t + 1) / n);
  };
  vector<u32> total(G + 1), errors(G);
  parallel(n, [&](int t) {
    for (size_t r = ranks(t).first; r < ranks(t).second; r++)
      for (int i = 0; i < n; i++)
        if (loc[i][r] >= 0)
          for (u32 h = 0; h < HOURS; h++) {
            u64 c = parts[i].ce[(size_t)loc[i][r] * HOURS + h];
            total[h * E + r + 1] += (u32)c;
            errors[h * E + r] += c >> 32;
          }
  });
  for (size_t g = 0; g < G; g++)
    total[g + 1] += total[g]; // total[g] = first slot of group g
  parallel(n, [&](int t) {
    for (size_t r = ranks(t).first; r < ranks(t).second; r++)
      for (u32 h = 0; h < HOURS; h++) {
        u32 at = total[h * E + r];
        for (int i = 0; i < n; i++)
          if (loc[i][r] >= 0) {
            size_t k = (size_t)loc[i][r] * HOURS + h;
            cursor[i][k] = at;
            at += (u32)parts[i].ce[k];
          }
      }
  });
  auto vals = make_unique_for_overwrite<u32[]>(total[G]);
  parallel(n, [&](int i) {
    for (u64 r : parts[i].recs)
      vals[cursor[i][r >> 32]++] = (u32)r;
    vector<u64>().swap(parts[i].recs);
  });

  // Split the groups so that each thread owns about the same number of
  // latencies.
  vector<size_t> split(n + 1, G);
  for (int i = 0; i < n; i++)
    split[i] = upper_bound(total.begin(), total.end(), (u64)total[G] * i / n) -
               total.begin() - 1;
  split[0] = 0;
  static const char *days[] = {"2026-10-30", "2026-10-31", "2026-11-01",
                               "2026-11-02"};
  string hdr[HOURS]; // "2026-10-30T18:00:00Z " per hour index
  for (int h = 0; h < HOURS; h++) {
    char hd[40];
    u32 t = h + 18;
    snprintf(hd, sizeof hd, "%sT%02u:00:00Z ", days[t / 24], t % 24);
    hdr[h] = hd;
  }
  vector<string> outs(n);
  parallel(n, [&](int i) {
    string &s = outs[i];
    s.reserve((split[i + 1] - split[i]) * 14 + 64);
    auto num = [&](u64 v, char sep) {
      char b[24];
      s.append(b, to_chars(b, b + 24, v).ptr);
      s += sep;
    };
    for (size_t g = split[i]; g < split[i + 1]; g++) {
      u64 c = total[g + 1] - total[g];
      if (!c)
        continue;
      u32 *b = vals.get() + total[g];
      u64 r50 = (50 * c + 99) / 100 - 1, r95 = (95 * c + 99) / 100 - 1,
          r99 = (99 * c + 99) / 100 - 1;
      u32 q[3];
      pick(b, c, r50, r95, r99, q);
      s += hdr[g / E];
      s += eps[g % E];
      s += ' ';
      num(c, ' ');
      num(q[0], ' ');
      num(q[1], ' ');
      num(q[2], ' ');
      num(errors[g], ' ');
      num(errors[g] * 1000000ULL / c, '\n');
    }
  });

  filesystem::create_directories(outDir);
  int ofd = open((outDir + "/summary.txt").c_str(),
                 O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (ofd < 0) {
    fprintf(stderr, "Cannot create summary.txt\n");
    return 1;
  }
  vector<u64> at(n + 1); // each thread writes its own part of the file
  for (int i = 0; i < n; i++)
    at[i + 1] = at[i] + outs[i].size();
  parallel(n, [&](int i) {
    const char *p = outs[i].data();
    for (size_t left = outs[i].size(), done = 0; left;) {
      ssize_t w = pwrite(ofd, p + done, left, at[i] + done);
      if (w <= 0)
        break;
      done += w;
      left -= w;
    }
  });
  close(ofd);
  _exit(0); // skip destructors / freeing ~100 MB of vectors
}