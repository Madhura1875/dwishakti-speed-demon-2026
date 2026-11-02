// Track D: streaming two-pass CSR build (no edge list, no file-sized buffer),
// bidirectional Dijkstra per query, queries run in parallel.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sched.h>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>
using namespace std;

typedef uint32_t u32;
typedef uint64_t u64;
static constexpr u64 INF = UINT64_MAX;
struct Arc {
  u32 to, w;
};
struct Item {
  u64 d;
  u32 v;
};
struct Edge {
  u32 u, v, w;
};
static constexpr size_t BATCH = 32;

// Next number at or after p. The text must end in the sentinel "\n0\n" so that
// neither loop can run off it.
static inline u32 num(const char *&p) {
  while ((unsigned)(*p - '0') > 9)
    ++p;
  u32 x = 0;
  while ((unsigned)(*p - '0') <= 9)
    x = x * 10 + (u32)(*p++ - '0');
  return x;
}

static inline bool next_num(const char *&p, const char *e, u32 &x) {
  while (p < e && (unsigned)(*p - '0') > 9)
    ++p;
  if (p >= e)
    return false;
  x = 0;
  while (p < e && (unsigned)(*p - '0') <= 9)
    x = x * 10 + (u32)(*p++ - '0');
  return true;
}

// Large random-access arrays: ask for 2 MB pages to cut TLB misses (harmless if
// the kernel declines).
template <class T> static T *huge(size_t n) {
  size_t bytes = (n * sizeof(T) + (2u << 20) - 1) & ~(size_t)((2u << 20) - 1);
  void *p = aligned_alloc(2u << 20, bytes ? bytes : 2u << 20);
  madvise(p, bytes, MADV_HUGEPAGE);
  return (T *)p;
}

template <class F> static void par(int T, F &&fn) {
  vector<thread> th;
  for (int t = 1; t < T; ++t)
    th.emplace_back([&fn, t] { fn(t); });
  fn(0);
  for (auto &x : th)
    x.join();
}

// Streams byte range [a,b) (whole lines) through 256 KB blocks; f(edges, count)
// gets each block's edges.
template <class F> static void stream_edges(int fd, off_t a, off_t b, F &&f) {
  const size_t B = 1 << 18;
  vector<char> buf(B + 4);
  vector<Edge> es;
  size_t have = 0;
  off_t off = a;
  for (;;) {
    ssize_t r = off < b ? pread(fd, buf.data() + have,
                                min<off_t>(B - have, b - off), off)
                        : 0;
    if (r > 0) {
      off += r;
      have += r;
    }
    bool last = r <= 0 || off >= b;
    size_t use = have;
    if (!last) {
      while (use && buf[use - 1] != '\n')
        --use;
    }
    char *e = buf.data() + use, saved[3];
    memcpy(saved, e, 3);
    memcpy(e, "\n0\n", 3);
    es.clear();
    for (const char *p = buf.data();;) {
      while (p < e && (unsigned)(*p - '0') > 9)
        ++p;
      if (p >= e)
        break;
      u32 u = num(p), v = num(p), w = num(p);
      if (p > e)
        break; // the third number was the sentinel: truncated line
      if (u != v)
        es.push_back({u, v, w});
    }
    memcpy(e, saved, 3);
    f(es.data(), es.size());
    memmove(buf.data(), buf.data() + use, have - use);
    have -= use;
    if (last)
      break;
  }
}

// Small open-addressing map node -> tentative distance. A query touches only a
// few thousand nodes, so this stays in cache, unlike a full n-sized array per
// thread (also cuts ~28 MB of memory per thread).
struct Dist {
  static constexpr u32 EMPTY = UINT32_MAX;
  vector<u32> key;
  vector<u64> val;
  vector<u32> used;
  int shift = 0;
  Dist() { init(14); }
  void init(int bits) {
    key.assign(1u << bits, EMPTY);
    val.resize(1u << bits);
    used.clear();
    shift = 32 - bits;
  }
  u32 slot(u32 k) const {
    u32 mask = key.size() - 1, s = (k * 0x9E3779B1u) >> shift;
    while (key[s] != EMPTY && key[s] != k)
      s = (s + 1) & mask;
    return s;
  }
  u64 get(u32 k) const {
    u32 s = slot(k);
    return key[s] == k ? val[s] : INF;
  }
  void set(u32 k, u64 v) {
    u32 s = slot(k);
    if (key[s] == EMPTY) {
      key[s] = k;
      used.push_back(s);
      if (used.size() * 2 > key.size()) { // grow
        vector<u32> ok = key;
        vector<u64> ov = val;
        vector<u32> ou = used;
        init(33 - shift);
        for (u32 u : ou) {
          u32 t = slot(ok[u]);
          key[t] = ok[u];
          val[t] = ov[u];
          used.push_back(t);
        }
        s = slot(k);
      }
    }
    val[s] = v;
  }
  void clear() {
    if (key.size() > (1u << 16)) {
      init(14);
      return;
    }
    for (u32 s : used)
      key[s] = EMPTY;
    used.clear();
  }
};

// Radix heap: a priority queue for keys that never drop below the last popped
// key (true for Dijkstra).
struct Heap {
  vector<Item> b[65];
  u64 last = 0, n = 0;
  static int at(u64 x, u64 l) {
    return x == l ? 0 : 64 - __builtin_clzll(x ^ l);
  }
  void push(Item it) {
    b[at(it.d, last)].push_back(it);
    ++n;
  }
  Item &top() {
    if (b[0].empty()) {
      int i = 1;
      while (b[i].empty())
        ++i;
      last = min_element(b[i].begin(), b[i].end(), [](Item &x, Item &y) {
               return x.d < y.d;
             })->d;
      for (Item &it : b[i])
        b[at(it.d, last)].push_back(it);
      b[i].clear();
    }
    return b[0].back();
  }
  void pop() {
    top();
    b[0].pop_back();
    --n;
  }
  void clear() {
    for (auto &v : b)
      v.clear();
    n = last = 0;
  }
};

struct Searcher {
  const u32 *fh, *rh;
  const Arc *fa, *ra;
  Dist df, db;
  Heap hf, hb;
  Searcher(const u32 *a, const Arc *b, const u32 *c, const Arc *d)
      : fh(a), rh(c), fa(b), ra(d) {}

  static void clean(Heap &h, const Dist &dist) {
    while (h.n && h.top().d > dist.get(h.top().v))
      h.pop();
  }
  static void step(Heap &h, Dist &dist, const Dist &other, const u32 *head,
                   const Arc *arcs, u64 &mu) {
    Item it = h.top();
    h.pop();
    for (u32 i = head[it.v], e = head[it.v + 1]; i < e; ++i) {
      u64 nd = it.d + arcs[i].w;
      u32 v = arcs[i].to;
      if (nd < mu && nd < dist.get(v)) {
        dist.set(v, nd);
        h.push({nd, v});
        u64 o = other.get(v);
        if (o != INF && nd + o < mu)
          mu = nd + o;
      }
    }
  }
  u64 run(u32 s, u32 t) {
    if (s == t)
      return 0;
    u64 mu = INF;
    df.set(s, 0);
    hf.push({0, s});
    db.set(t, 0);
    hb.push({0, t});
    for (;;) {
      clean(hf, df);
      clean(hb, db);
      if (!hf.n || !hb.n || hf.top().d + hb.top().d >= mu)
        break;
      if (hf.n <= hb.n)
        step(hf, df, db, fh, fa, mu);
      else
        step(hb, db, df, rh, ra, mu);
    }
    df.clear();
    db.clear();
    hf.clear();
    hb.clear();
    return mu;
  }
};

int main(int argc, char **argv) {
  const char *gp = argc > 3 ? argv[1] : "/data/graph.txt";
  const char *qp = argc > 3 ? argv[2] : "/data/queries.txt";
  const char *op = argc > 3 ? argv[3] : "/out/distances.txt";
  cpu_set_t cs;
  sched_getaffinity(0, sizeof cs, &cs);
  const int T =
      clamp(CPU_COUNT(&cs), 1, 64); // every core we are allowed to use

  int fd = open(gp, O_RDONLY);
  if (fd < 0)
    return 1;
  const off_t size = lseek(fd, 0, SEEK_END);
  char hdr[64];
  ssize_t hl = pread(fd, hdr, sizeof hdr, 0);
  const char *hp = hdr;
  u32 n = 0, m = 0;
  next_num(hp, hdr + hl, n);
  next_num(hp, hdr + hl, m);
  off_t body = hp - hdr;
  while (body < hl && hdr[body] != '\n')
    ++body;
  ++body;

  // line-aligned chunk boundaries
  vector<off_t> cb(T + 1, size);
  cb[0] = body;
  for (int t = 1; t < T; ++t) {
    off_t r = body + (size - body) * t / T;
    char w[64];
    ssize_t k = pread(fd, w, sizeof w, r - 1);
    off_t bnd = size;
    for (ssize_t i = 0; i < k; ++i)
      if (w[i] == '\n') {
        bnd = r + i;
        break;
      }
    cb[t] = bnd;
  }

  u32 *fh = huge<u32>(n + 1), *rh = huge<u32>(n + 1);
  memset(fh, 0, (n + 1) * 4);
  memset(rh, 0, (n + 1) * 4);
  Arc *fa = nullptr, *ra = nullptr;
  auto sweep = [&](bool fill) { // counts every edge, or (fill) also stores its
                                // two arcs
    par(T, [&](int t) {
      stream_edges(fd, cb[t], cb[t + 1], [&](const Edge *e, size_t k) {
        for (size_t i = 0; i < k; i += BATCH) {
          const Edge *q = e + i;
          size_t c = min(BATCH, k - i);
          u32 pf[BATCH], pr[BATCH];
          for (size_t j = 0; j < c; ++j) { // overlap the random accesses
            __builtin_prefetch(&fh[q[j].u], 1);
            __builtin_prefetch(&rh[q[j].v], 1);
          }
          for (size_t j = 0; j < c; ++j) {
            pf[j] =
                atomic_ref<u32>(fh[q[j].u]).fetch_add(1, memory_order_relaxed);
            pr[j] =
                atomic_ref<u32>(rh[q[j].v]).fetch_add(1, memory_order_relaxed);
            if (fill) {
              __builtin_prefetch(&fa[pf[j]], 1);
              __builtin_prefetch(&ra[pr[j]], 1);
            }
          }
          for (size_t j = 0; fill && j < c; ++j) {
            fa[pf[j]] = {q[j].v, q[j].w};
            ra[pr[j]] = {q[j].u, q[j].w};
          }
        }
      });
    });
  };
  sweep(false);
  u32 fs = 0, rs = 0;
  for (u32 u = 0; u < n; ++u) {
    u32 a = fh[u];
    fh[u] = fs;
    fs += a;
    u32 b = rh[u];
    rh[u] = rs;
    rs += b;
  }
  fa = huge<Arc>(fs + 1);
  ra = huge<Arc>(rs + 1);
  sweep(true);
  close(fd);
  for (u32 u = n; u > 0; --u) {
    fh[u] = fh[u - 1];
    rh[u] = rh[u - 1];
  } // cursors -> starts
  fh[0] = rh[0] = 0;

  vector<pair<u32, u32>> qs;
  {
    int qf = open(qp, O_RDONLY);
    if (qf < 0)
      return 1;
    string b(lseek(qf, 0, SEEK_END), 0); // the whole file, however large
    size_t got = 0;
    for (ssize_t r;
         got < b.size() && (r = pread(qf, &b[got], b.size() - got, got)) > 0;)
      got += r;
    close(qf);
    const char *p = b.data(), *e = p + got;
    u32 s, t;
    while (next_num(p, e, s) && next_num(p, e, t))
      qs.push_back({s, t});
  }

  vector<u64> ans(qs.size());
  atomic<size_t> nxt{0};
  const int QT = (int)max<size_t>(1, min<size_t>(T, qs.size()));
  par(QT, [&](int) {
    Searcher S(fh, fa, rh, ra);
    for (size_t i; (i = nxt.fetch_add(1)) < qs.size();)
      ans[i] = S.run(qs[i].first, qs[i].second);
  });

  string out;
  for (u64 x : ans)
    out += (x == INF ? string("-1") : to_string(x)) + '\n';
  FILE *f = fopen(op, "wb");
  if (!f)
    return 1;
  fwrite(out.data(), 1, out.size(), f);
  fclose(f);
}