// Track D: streaming two-pass CSR build (no edge list, no file-sized buffer),
// bidirectional Dijkstra per query, queries run in parallel.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
using namespace std;

typedef uint32_t u32;
typedef uint64_t u64;
static constexpr u64 INF = UINT64_MAX;
struct Arc { u32 to, w; };

// Arc array of size k, not zeroed, on huge pages where the system allows it:
// the build writes it in random order, so fewer page-table misses matter.
static Arc* arcs(size_t k) {
    size_t bytes = (k * sizeof(Arc) + (2 << 20) - 1) & ~(size_t)((2 << 20) - 1);
    void* p = aligned_alloc(2 << 20, bytes);
    madvise(p, bytes, MADV_HUGEPAGE);
    return (Arc*)p;
}
struct Item { u64 d; u32 v; };
static bool gt(const Item& a, const Item& b) { return a.d > b.d; }

static inline bool next_num(const char*& p, const char* e, u32& x) {
    while (p < e && (unsigned)(*p - '0') > 9) ++p;
    if (p >= e) return false;
    x = 0;
    while (p < e && (unsigned)(*p - '0') <= 9) x = x * 10 + (u32)(*p++ - '0');
    return true;
}

template <class F> static void par(int T, F&& fn) {
    vector<thread> th;
    for (int t = 1; t < T; ++t) th.emplace_back([&fn, t] { fn(t); });
    fn(0);
    for (auto& x : th) x.join();
}

// Streams byte range [a,b) (whole lines) through 1 MB blocks, calls f(u,v,w) per edge.
template <class F> static void stream_edges(int fd, off_t a, off_t b, F&& f) {
    const size_t B = 1 << 20;
    vector<char> buf(B);
    size_t have = 0;
    off_t off = a;
    for (;;) {
        ssize_t r = off < b ? pread(fd, buf.data() + have, min<off_t>(B - have, b - off), off) : 0;
        if (r > 0) { off += r; have += r; }
        bool last = r <= 0 || off >= b;
        size_t use = have;
        if (!last) { while (use && buf[use - 1] != '\n') --use; }
        const char *p = buf.data(), *e = p + use;
        u32 u, v, w;
        while (next_num(p, e, u) && next_num(p, e, v) && next_num(p, e, w)) if (u != v) f(u, v, w);
        memmove(buf.data(), buf.data() + use, have - use);
        have -= use;
        if (last) break;
    }
}

// Distances found so far in one search direction: an open-addressing table that
// only grows with the vertices actually reached, not with n.
struct Dist {
    vector<u32> key = vector<u32>(1024, ~0u);
    vector<u64> val = vector<u64>(1024);
    size_t used = 0;
    size_t slot(u32 v) const {
        size_t m = key.size() - 1, i = (v * 2654435761u >> 7) & m;
        while (key[i] != ~0u && key[i] != v) i = (i + 1) & m;
        return i;
    }
    u64 get(u32 v) const { size_t i = slot(v); return key[i] == v ? val[i] : INF; }
    void set(u32 v, u64 d) {
        size_t i = slot(v);
        if (key[i] != v) {
            if (++used * 2 > key.size()) {
                vector<u32> k(key.size() * 2, ~0u);
                vector<u64> x(k.size());
                k.swap(key); x.swap(val);
                for (size_t j = 0; j < k.size(); ++j) if (k[j] != ~0u) { size_t t = slot(k[j]); key[t] = k[j]; val[t] = x[j]; }
                i = slot(v);
            }
            key[i] = v;
        }
        val[i] = d;
    }
    void clear() { fill(key.begin(), key.end(), ~0u); used = 0; }
};

struct Searcher {
    const u32 *fh, *rh; const Arc *fa, *ra;
    Dist df, db; vector<Item> hf, hb;
    Searcher(const u32* a, const Arc* b, const u32* c, const Arc* d) : fh(a), rh(c), fa(b), ra(d) {}

    static void clean(vector<Item>& h, const Dist& dist) {
        while (!h.empty() && h.front().d > dist.get(h.front().v)) { pop_heap(h.begin(), h.end(), gt); h.pop_back(); }
    }
    static void step(vector<Item>& h, Dist& dist, const Dist& other, const u32* head, const Arc* arcs, u64& mu) {
        Item it = h.front();
        pop_heap(h.begin(), h.end(), gt); h.pop_back();
        for (u32 i = head[it.v], e = head[it.v + 1]; i < e; ++i) {
            u64 nd = it.d + arcs[i].w; u32 v = arcs[i].to;
            if (nd < mu && nd < dist.get(v)) {
                dist.set(v, nd);
                h.push_back({nd, v}); push_heap(h.begin(), h.end(), gt);
                u64 o = other.get(v);
                if (o != INF && nd + o < mu) mu = nd + o;
            }
        }
    }
    u64 run(u32 s, u32 t) {
        if (s == t) return 0;
        u64 mu = INF;
        df.set(s, 0); hf.push_back({0, s});
        db.set(t, 0); hb.push_back({0, t});
        for (;;) {
            clean(hf, df); clean(hb, db);
            if (hf.empty() || hb.empty() || hf.front().d + hb.front().d >= mu) break;
            if (hf.size() <= hb.size()) step(hf, df, db, fh, fa, mu);
            else step(hb, db, df, rh, ra, mu);
        }
        df.clear(); db.clear(); hf.clear(); hb.clear();
        return mu;
    }
};

int main(int argc, char** argv) {
    const char* gp = argc > 3 ? argv[1] : "/data/graph.txt";
    const char* qp = argc > 3 ? argv[2] : "/data/queries.txt";
    const char* op = argc > 3 ? argv[3] : "/out/distances.txt";
    const int T = (int)min(8u, max(1u, thread::hardware_concurrency()));

    int fd = open(gp, O_RDONLY);
    if (fd < 0) return 1;
    const off_t size = lseek(fd, 0, SEEK_END);
    char hdr[64];
    ssize_t hl = pread(fd, hdr, sizeof hdr, 0);
    const char* hp = hdr;
    u32 n = 0, m = 0;
    next_num(hp, hdr + hl, n); next_num(hp, hdr + hl, m);
    off_t body = hp - hdr;
    while (body < hl && hdr[body] != '\n') ++body;
    ++body;

    vector<u32> fh(n + 1, 0), rh(n + 1, 0);
    // Each thread owns the vertices [lo(t), lo(t+1)) and reads the whole file,
    // keeping only the arcs it owns: no atomics and no cache lines shared.
    auto lo = [&](int t) { return (u32)((u64)n * t / T); };
    par(T, [&](int t) {
        u32 a = lo(t), len = lo(t + 1) - a;
        stream_edges(fd, body, size, [&](u32 u, u32 v, u32) {
            if (u - a < len) fh[u]++;
            if (v - a < len) rh[v]++;
        });
    });
    u32 fs = 0, rs = 0;
    for (u32 u = 0; u < n; ++u) {
        u32 a = fh[u]; fh[u] = fs; fs += a;
        u32 b = rh[u]; rh[u] = rs; rs += b;
    }
    Arc *fa = arcs(fs), *ra = arcs(rs);
    par(T, [&](int t) {
        u32 a = lo(t), len = lo(t + 1) - a;
        stream_edges(fd, body, size, [&](u32 u, u32 v, u32 w) {
            if (u - a < len) fa[fh[u]++] = {v, w};
            if (v - a < len) ra[rh[v]++] = {u, w};
        });
    });
    close(fd);
    for (u32 u = n; u > 0; --u) { fh[u] = fh[u - 1]; rh[u] = rh[u - 1]; }  // cursors -> starts
    fh[0] = rh[0] = 0;

    vector<pair<u32, u32>> qs;
    if (FILE* f = fopen(qp, "rb")) {
        vector<char> b(1 << 20);
        size_t len = fread(b.data(), 1, b.size(), f);
        fclose(f);
        const char *p = b.data(), *e = p + len;
        u32 s, t;
        while (next_num(p, e, s) && next_num(p, e, t)) qs.push_back({s, t});
    } else return 1;

    vector<u64> ans(qs.size());
    atomic<size_t> nxt{0};
    const int QT = (int)max<size_t>(1, min<size_t>(min(T, 4), qs.size()));
    par(QT, [&](int) {
        Searcher S(fh.data(), fa, rh.data(), ra);
        for (size_t i; (i = nxt.fetch_add(1)) < qs.size();) ans[i] = S.run(qs[i].first, qs[i].second);
    });

    string out;
    for (u64 x : ans) out += (x == INF ? string("-1") : to_string(x)) + '\n';
    FILE* f = fopen(op, "wb");
    if (!f) return 1;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
}