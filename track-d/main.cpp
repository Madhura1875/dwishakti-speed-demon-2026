// Track D (Traverse): multithreaded load + CSR build, then bidirectional
// Dijkstra per query with the correct stopping rule, queries run in parallel.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

using namespace std;

static constexpr uint64_t INF = UINT64_MAX;

struct Arc  { uint32_t to, w; };
struct Edge { uint32_t u, v, w; };
struct Item { uint64_t d; uint32_t v; };

static inline bool item_gt(const Item& a, const Item& b) { return a.d > b.d; }

static inline bool is_digit(char c) { return (unsigned)(c - '0') <= 9u; }

// Skips anything that is not a digit (spaces, \n, \r), then reads one number.
static inline bool next_num(const char*& p, const char* end, uint32_t& out) {
    while (p < end && !is_digit(*p)) ++p;
    if (p >= end) return false;
    uint32_t x = 0;
    while (p < end && is_digit(*p)) { x = x * 10 + (uint32_t)(*p - '0'); ++p; }
    out = x;
    return true;
}

static char* slurp(const char* path, size_t& size) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = (char*)malloc(size + 1);
    size = fread(b, 1, size, f);
    fclose(f);
    b[size] = 0;
    return b;
}

template <class F>
static void run_threads(int T, F&& fn) {
    vector<thread> th;
    for (int t = 1; t < T; ++t) th.emplace_back([&fn, t] { fn(t); });
    fn(0);
    for (auto& x : th) x.join();
}

struct Searcher {
    uint32_t n;
    const uint32_t *fh, *rh;
    const Arc *fa, *ra;
    vector<uint64_t> df, db;
    vector<uint32_t> tf, tb;
    vector<Item> hf, hb;

    Searcher(uint32_t n_, const uint32_t* fh_, const Arc* fa_, const uint32_t* rh_, const Arc* ra_)
        : n(n_), fh(fh_), rh(rh_), fa(fa_), ra(ra_), df(n_, INF), db(n_, INF) {
        hf.reserve(1 << 16); hb.reserve(1 << 16);
    }

    static inline void clean(vector<Item>& h, const vector<uint64_t>& dist) {
        while (!h.empty() && h.front().d > dist[h.front().v]) {
            pop_heap(h.begin(), h.end(), item_gt);
            h.pop_back();
        }
    }

    static inline void step(vector<Item>& h, vector<uint64_t>& dist, vector<uint32_t>& touched,
                            const vector<uint64_t>& other, const uint32_t* head, const Arc* arcs,
                            uint64_t& mu) {
        Item it = h.front();
        pop_heap(h.begin(), h.end(), item_gt);
        h.pop_back();
        const uint64_t d = it.d;
        const uint32_t u = it.v;
        for (uint32_t i = head[u], e = head[u + 1]; i < e; ++i) {
            const Arc a = arcs[i];
            const uint64_t nd = d + a.w;
            if (nd < mu && nd < dist[a.to]) {
                if (dist[a.to] == INF) touched.push_back(a.to);
                dist[a.to] = nd;
                h.push_back({nd, a.to});
                push_heap(h.begin(), h.end(), item_gt);
                const uint64_t o = other[a.to];
                if (o != INF && nd + o < mu) mu = nd + o;
            }
        }
    }

    // returns INF if unreachable
    uint64_t run(uint32_t s, uint32_t t) {
        if (s == t) return 0;
        uint64_t mu = INF;
        df[s] = 0; tf.push_back(s); hf.push_back({0, s});
        db[t] = 0; tb.push_back(t); hb.push_back({0, t});
        for (;;) {
            clean(hf, df);
            clean(hb, db);
            if (hf.empty() || hb.empty()) break;           // one side exhausted: mu is final
            if (hf.front().d + hb.front().d >= mu) break;  // standard stopping rule
            if (hf.size() <= hb.size()) step(hf, df, tf, db, fh, fa, mu);
            else                        step(hb, db, tb, df, rh, ra, mu);
        }
        for (uint32_t v : tf) df[v] = INF;
        for (uint32_t v : tb) db[v] = INF;
        tf.clear(); tb.clear(); hf.clear(); hb.clear();
        return mu;
    }
};

int main(int argc, char** argv) {
    const char* graph_path = argc > 3 ? argv[1] : "/data/graph.txt";
    const char* query_path = argc > 3 ? argv[2] : "/data/queries.txt";
    const char* out_path   = argc > 3 ? argv[3] : "/out/distances.txt";

    unsigned hw = thread::hardware_concurrency();
    const int T = (int)min(8u, max(1u, hw));

    // ---------- load graph ----------
    size_t gsize = 0;
    char* gbuf = slurp(graph_path, gsize);
    if (!gbuf) return 1;
    const char* gend = gbuf + gsize;
    const char* p = gbuf;
    uint32_t n = 0, m = 0;
    next_num(p, gend, n);
    next_num(p, gend, m);
    while (p < gend && *p != '\n') ++p;
    if (p < gend) ++p;
    const char* body = p;

    // split body into T chunks that start at line starts
    vector<const char*> cb(T + 1);
    const size_t blen = (size_t)(gend - body);
    for (int t = 0; t <= T; ++t) {
        const char* s = body + blen * (size_t)t / (size_t)T;
        if (t == T) s = gend;
        else if (s > body) while (s < gend && s[-1] != '\n') ++s;
        cb[t] = s;
    }

    vector<vector<Edge>> es(T);
    vector<vector<uint32_t>> cf(T), cr(T);

    run_threads(T, [&](int t) {
        const char* q = cb[t];
        const char* e = cb[t + 1];
        auto& ev = es[t];
        ev.reserve((size_t)m / T + (size_t)m / (4 * T) + 16);
        uint32_t u, v, w;
        while (next_num(q, e, u) && next_num(q, e, v) && next_num(q, e, w))
            if (u != v) ev.push_back({u, v, w});     // self-loops never help
        cf[t].assign(n, 0);
        cr[t].assign(n, 0);
        for (const Edge& x : ev) { cf[t][x.u]++; cr[t][x.v]++; }
    });
    free(gbuf);

    // ---------- CSR offsets (per-thread cursors) ----------
    vector<uint32_t> fh(n + 1), rh(n + 1);
    uint32_t fsum = 0, rsum = 0;
    for (uint32_t u = 0; u < n; ++u) {
        fh[u] = fsum; rh[u] = rsum;
        for (int t = 0; t < T; ++t) {
            uint32_t c = cf[t][u]; cf[t][u] = fsum; fsum += c;
            uint32_t d = cr[t][u]; cr[t][u] = rsum; rsum += d;
        }
    }
    fh[n] = fsum; rh[n] = rsum;
    const size_t M = fsum;
    auto fa = make_unique_for_overwrite<Arc[]>(M ? M : 1);
    auto ra = make_unique_for_overwrite<Arc[]>(M ? M : 1);

    run_threads(T, [&](int t) {
        auto& a = cf[t];
        auto& b = cr[t];
        for (const Edge& x : es[t]) {
            fa[a[x.u]++] = {x.v, x.w};
            ra[b[x.v]++] = {x.u, x.w};
        }
        vector<Edge>().swap(es[t]);
        vector<uint32_t>().swap(a);
        vector<uint32_t>().swap(b);
    });

    // ---------- load queries ----------
    size_t qsize = 0;
    char* qbuf = slurp(query_path, qsize);
    if (!qbuf) return 1;
    vector<pair<uint32_t, uint32_t>> queries;
    {
        const char* q = qbuf;
        const char* e = qbuf + qsize;
        uint32_t s, t;
        while (next_num(q, e, s) && next_num(q, e, t)) queries.push_back({s, t});
    }
    free(qbuf);

    // ---------- answer queries in parallel ----------
    const size_t Q = queries.size();
    vector<uint64_t> ans(Q);
    atomic<size_t> next{0};
    const int QT = (int)max<size_t>(1, min<size_t>((size_t)T, Q));
    run_threads(QT, [&](int) {
        Searcher S(n, fh.data(), fa.get(), rh.data(), ra.get());
        for (;;) {
            size_t i = next.fetch_add(1, memory_order_relaxed);
            if (i >= Q) break;
            ans[i] = S.run(queries[i].first, queries[i].second);
        }
    });

    // ---------- output ----------
    vector<char> out;
    out.reserve(Q * 21 + 16);
    for (size_t i = 0; i < Q; ++i) {
        if (ans[i] == INF) { out.push_back('-'); out.push_back('1'); }
        else {
            char tmp[24]; int len = 0;
            uint64_t x = ans[i];
            if (x == 0) tmp[len++] = '0';
            while (x) { tmp[len++] = (char)('0' + x % 10); x /= 10; }
            while (len) out.push_back(tmp[--len]);
        }
        out.push_back('\n');
    }
    FILE* f = fopen(out_path, "wb");
    if (!f) return 1;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return 0;
}