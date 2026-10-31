#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace std;

struct Edge { uint32_t u, v, w; };
struct Arc { uint32_t to, w; };
struct Item { uint64_t d; uint32_t v; };

static const uint64_t NONE = UINT64_MAX;

static string slurp(const string& path) {
    string s;
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return s;
    struct stat st;
    fstat(fd, &st);
    s.resize(st.st_size);
    size_t got = 0;
    for (ssize_t r; got < s.size() && (r = read(fd, &s[got], s.size() - got)) > 0;) got += r;
    close(fd);
    s.resize(got);
    return s;
}

template <class F>
static void parallel(int n, F f) {
    vector<thread> workers;
    for (int i = 1; i < n; i++) workers.emplace_back(f, i);
    f(0);
    for (thread& w : workers) w.join();
}

// Parses the whole lines "u v w\n" in [p, e). A self-loop never shortens a path, so it is dropped.
static void parseEdges(const char* p, const char* e, vector<Edge>& out) {
    while (p < e) {
        uint32_t u = 0, v = 0;
        uint64_t w = 0;
        while (*p != ' ') u = u * 10 + (*p++ - '0');
        for (p++; *p != ' '; p++) v = v * 10 + (*p - '0');
        for (p++; *p != '\n'; p++) w = w * 10 + (*p - '0');
        p++;
        if (u != v) out.push_back({u, v, (uint32_t)w});
    }
}

// Per-thread Dijkstra state. dist holds distance + 1 for touched nodes and 0 for the rest,
// so it can come zeroed from calloc without touching every page.
struct Search {
    uint64_t* dist;
    vector<uint32_t> touched;
    vector<Item> heap;
    explicit Search(size_t n) : dist((uint64_t*)calloc(n, sizeof(uint64_t))) {}
    Search(const Search&) = delete;
    ~Search() { free(dist); }
};

int main(int argc, char* argv[]) {
    string dataDir = argc > 1 ? argv[1] : "/data";
    string outDir  = argc > 2 ? argv[2] : "/out";

    int fd = open((dataDir + "/graph.txt").c_str(), O_RDONLY);
    if (fd < 0) { fprintf(stderr, "Cannot open graph.txt\n"); return 1; }
    off_t size = lseek(fd, 0, SEEK_END);
    char head[64];
    ssize_t got = pread(fd, head, sizeof head, 0);
    const char* nl = (const char*)memchr(head, '\n', max<ssize_t>(got, 0));
    if (!nl) { fprintf(stderr, "Bad graph.txt header\n"); return 1; }
    size_t n = 0;
    for (const char* p = head; *p != ' '; p++) n = n * 10 + (*p - '0');
    off_t body = nl - head + 1;

    cpu_set_t cpus;
    sched_getaffinity(0, sizeof cpus, &cpus);
    int cores = clamp(CPU_COUNT(&cpus), 1, 16);
    int T = (int)min<off_t>(cores, max<off_t>(1, (size - body) >> 20));

    // Slice i is the edge lines starting in [cut[i], cut[i + 1]).
    vector<off_t> cut(T + 1, size);
    cut[0] = body;
    for (int i = 1; i < T; i++) {
        off_t lo = body + (size - body) * i / T;
        char b[256];
        ssize_t g = max<ssize_t>(pread(fd, b, sizeof b, lo - 1), 0);
        const char* e = (const char*)memchr(b, '\n', g);
        cut[i] = max(cut[i - 1], e ? lo + (e - b) : size);
    }

    vector<vector<Edge>> parts(T);
    parallel(T, [&](int i) {
        vector<Edge>& out = parts[i];
        out.reserve((cut[i + 1] - cut[i]) / 16 + 16);
        vector<char> buf(1 << 18);
        size_t have = 0;
        off_t pos = cut[i], end = cut[i + 1];
        while (pos < end) {
            size_t want = min<off_t>(buf.size() - have, end - pos);
            ssize_t r = pread(fd, buf.data() + have, want, pos);
            if (r <= 0) break;
            pos += r;
            have += r;
            char* last = (char*)memrchr(buf.data(), '\n', have);
            if (!last) continue;
            parseEdges(buf.data(), last + 1, out);
            have -= last + 1 - buf.data();
            memmove(buf.data(), last + 1, have);
        }
    });
    close(fd);

    // Compact adjacency arrays (CSR); hasOut / hasIn mark nodes with a real out-edge / in-edge.
    vector<uint32_t> off(n + 1, 0);
    vector<uint64_t> hasOut((n + 63) / 64), hasIn((n + 63) / 64);
    for (const vector<Edge>& part : parts)
        for (const Edge& e : part) {
            off[e.u + 1]++;
            hasOut[e.u >> 6] |= 1ull << (e.u & 63);
            hasIn[e.v >> 6] |= 1ull << (e.v & 63);
        }
    for (size_t u = 0; u < n; u++) off[u + 1] += off[u];
    auto arcs = make_unique_for_overwrite<Arc[]>(off[n]);
    vector<uint32_t> pos(off.begin(), off.begin() + n);
    for (vector<Edge>& part : parts) {
        for (const Edge& e : part) arcs[pos[e.u]++] = {e.v, e.w};
        vector<Edge>().swap(part);
    }
    vector<uint32_t>().swap(pos);

    string qtext = slurp(dataDir + "/queries.txt");
    if (!qtext.empty() && qtext.back() != '\n') qtext += '\n';
    vector<pair<uint32_t, uint32_t>> qs;
    for (const char* p = qtext.data(), *e = p + qtext.size(); p < e;) {
        uint32_t s = 0, t = 0;
        while (*p != ' ') s = s * 10 + (*p++ - '0');
        for (p++; *p != '\n'; p++) t = t * 10 + (*p - '0');
        p++;
        qs.push_back({s, t});
    }

    auto has = [](const vector<uint64_t>& b, uint32_t x) { return (b[x >> 6] >> (x & 63)) & 1; };
    auto greater = [](const Item& a, const Item& b) { return a.d > b.d; };
    // Shortest distance from s to t by Dijkstra, stopping when t is settled.
    auto solve = [&](Search& w, uint32_t s, uint32_t t) -> uint64_t {
        if (s == t) return 0;
        if (!has(hasOut, s) || !has(hasIn, t)) return NONE;  // nothing leaves s, or nothing reaches t
        uint64_t result = NONE;
        w.heap.clear();
        w.touched.clear();
        w.dist[s] = 1;
        w.touched.push_back(s);
        w.heap.push_back({0, s});
        while (!w.heap.empty()) {
            pop_heap(w.heap.begin(), w.heap.end(), greater);
            Item it = w.heap.back();
            w.heap.pop_back();
            if (it.d + 1 != w.dist[it.v]) continue;  // stale entry
            if (it.v == t) { result = it.d; break; }
            for (uint32_t k = off[it.v]; k < off[it.v + 1]; k++) {
                uint64_t nd = it.d + arcs[k].w + 1;
                uint64_t& cur = w.dist[arcs[k].to];
                if (cur == 0) w.touched.push_back(arcs[k].to);
                if (cur != 0 && nd >= cur) continue;
                cur = nd;
                w.heap.push_back({nd - 1, arcs[k].to});
                push_heap(w.heap.begin(), w.heap.end(), greater);
            }
        }
        for (uint32_t v : w.touched) w.dist[v] = 0;
        return result;
    };

    vector<uint64_t> ans(qs.size());
    atomic<size_t> next{0};
    parallel(cores, [&](int) {
        Search w(n);
        for (size_t i; (i = next++) < qs.size();) ans[i] = solve(w, qs[i].first, qs[i].second);
    });

    string out;
    for (uint64_t d : ans) {
        if (d == NONE) { out += "-1\n"; continue; }
        char b[24];
        out.append(b, to_chars(b, b + 24, d).ptr);
        out += '\n';
    }
    filesystem::create_directories(outDir);
    FILE* f = fopen((outDir + "/distances.txt").c_str(), "wb");
    if (!f) { fprintf(stderr, "Cannot create distances.txt\n"); return 1; }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return 0;
}