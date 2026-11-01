// Track D: shortest-path distances. The graph is streamed from the text file
// into forward and reverse CSR arrays in two passes (count, then fill) without
// ever holding the edge list; queries then run in parallel as bidirectional
// Dijkstra on a radix heap.
//
// Both passes parse a block of the file into a small edge array and then touch
// the big arrays in batches: all cache lines of a batch are prefetched first,
// so the random accesses overlap instead of waiting one by one behind the
// locked increments.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
using namespace std;

typedef uint32_t u32;
typedef uint64_t u64;
static constexpr u64 INF = UINT64_MAX;
static constexpr size_t BATCH = 32;
struct Arc {
  u32 to, w;
};
struct Edge {
  u32 u, v, w;
};
struct Item {
  u64 d;
  u32 v;
};

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
  const u32 *head[2];
  const Arc *arcs[2];
  vector<array<u64, 2>> dd; // dd[v][0] forward, dd[v][1] backward distance: one
                            // cache line serves both
  vector<u32> touched[2];
  Heap heap[2];
  Searcher(u32 n, const u32 *fh, const Arc *fa, const u32 *rh, const Arc *ra)
      : head{fh, rh}, arcs{fa, ra}, dd(n, {INF, INF}) {}

  void clean(int s) {
    Heap &h = heap[s];
    while (h.n && h.top().d > dd[h.top().v][s])
      h.pop();
  }
  void step(int s, u64 &mu) {
    Heap &h = heap[s];
    Item it = h.top();
    h.pop();
    const u32 lo = head[s][it.v], hi = head[s][it.v + 1];
    for (u32 i = lo; i < hi; ++i)
      __builtin_prefetch(&dd[arcs[s][i].to]);
    for (u32 i = lo; i < hi; ++i) {
      u64 nd = it.d + arcs[s][i].w;
      u32 v = arcs[s][i].to;
      array<u64, 2> &x = dd[v];
      if (nd < mu && nd < x[s]) {
        if (x[s] == INF)
          touched[s].push_back(v);
        x[s] = nd;
        h.push({nd, v});
        if (x[s ^ 1] != INF && nd + x[s ^ 1] < mu)
          mu = nd + x[s ^ 1];
      }
    }
  }
  u64 run(u32 s, u32 t) {
    if (s == t)
      return 0;
    u64 mu = INF;
    dd[s][0] = 0;
    touched[0].push_back(s);
    heap[0].push({0, s});
    dd[t][1] = 0;
    touched[1].push_back(t);
    heap[1].push({0, t});
    for (;;) {
      clean(0);
      clean(1);
      if (!heap[0].n || !heap[1].n || heap[0].top().d + heap[1].top().d >= mu)
        break;
      step(heap[0].n <= heap[1].n ? 0 : 1, mu);
    }
    for (int d = 0; d < 2; ++d) {
      for (u32 v : touched[d])
        dd[v][d] = INF;
      touched[d].clear();
      heap[d].clear();
    }
    return mu;
  }
};

// The whole file with the number sentinel appended.
static string slurp(const char *path) {
  int fd = open(path, O_RDONLY);
  string s(fd < 0 ? 0 : lseek(fd, 0, SEEK_END), 0);
  size_t got = 0;
  for (ssize_t r;
       got < s.size() && (r = pread(fd, &s[got], s.size() - got, got)) > 0;)
    got += r;
  close(fd);
  s.resize(got);
  return s + "\n0\n";
}

int main(int argc, char **argv) {
  const char *gp = argc > 3 ? argv[1] : "/data/graph.txt";
  const char *qp = argc > 3 ? argv[2] : "/data/queries.txt";
  const char *op = argc > 3 ? argv[3] : "/out/distances.txt";
  const int T = (int)min(8u, max(1u, thread::hardware_concurrency()));

  int fd = open(gp, O_RDONLY);
  if (fd < 0)
    return 1;
  const off_t size = lseek(fd, 0, SEEK_END);
  char hdr[72] = {};
  ssize_t hl = max<ssize_t>(pread(fd, hdr, 64, 0), 0);
  memcpy(hdr + hl, "\n0\n0\n", 5);
  const char *hp = hdr;
  u32 n = num(hp);
  num(hp);
  off_t body = (const char *)memchr(hp, '\n', hl + 5 - (hp - hdr)) - hdr + 1;

  // line-aligned chunk boundaries
  vector<off_t> cb(T + 1, size);
  cb[0] = min<off_t>(body, size);
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

  vector<u32> fh(n + 1, 0), rh(n + 1, 0);
  unique_ptr<Arc[]> fa, ra;
  auto sweep = [&](bool fill) { // counts every edge, or (fill) also stores its
                                // two arcs
    par(T, [&](int t) {
      stream_edges(fd, cb[t], cb[t + 1], [&](const Edge *e, size_t k) {
        for (size_t i = 0; i < k; i += BATCH) {
          const Edge *q = e + i;
          size_t c = min(BATCH, k - i);
          u32 pf[BATCH], pr[BATCH];
          for (size_t j = 0; j < c; ++j) {
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
  fa = make_unique_for_overwrite<Arc[]>(
      fs + 1); // not zero-filled: every slot is written by the second sweep
  ra = make_unique_for_overwrite<Arc[]>(rs + 1);
  sweep(true);
  close(fd);
  for (u32 u = n; u > 0; --u) {
    fh[u] = fh[u - 1];
    rh[u] = rh[u - 1];
  } // cursors -> starts
  fh[0] = rh[0] = 0;

  string qt = slurp(qp);
  vector<pair<u32, u32>> qs;
  for (const char *p = qt.data(), *e = p + qt.size() - 3;;) {
    while (p < e && (unsigned)(*p - '0') > 9)
      ++p;
    if (p >= e)
      break;
    u32 s = num(p), t = num(p);
    if (p > e)
      break;
    qs.push_back({s, t});
  }

  vector<u64> ans(qs.size());
  atomic<size_t> nxt{0};
  const int QT = (int)max<size_t>(1, min<size_t>(min(T, 4), qs.size()));
  par(QT, [&](int) {
    Searcher S(n, fh.data(), fa.get(), rh.data(), ra.get());
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