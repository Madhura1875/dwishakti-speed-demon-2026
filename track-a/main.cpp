






// Per-(hour, endpoint) latency summary: count, p50, p95, p99, errors, error rate (per million).
// Build: g++ -O3 -march=native -std=c++20 -pthread optimized.cpp -o summarize
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
 
#ifndef MADV_POPULATE_READ
#define MADV_POPULATE_READ 22
#endif
 
using namespace std;
 
struct Group { uint32_t koff, eplen, count = 0, start = 0; uint64_t errors = 0; uint32_t hv = 0; };
 
static uint32_t hourOf(const char* t) {
    auto two = [&](int i) { return (t[i] - '0') * 10 + t[i + 1] - '0'; };
    return (((two(0) * 100 + two(2)) * 13 + two(5)) * 32 + two(8)) * 24 + two(11);
}
 
struct Table {
    vector<Group> groups;
    vector<char> keys;                                   // per group: 13-byte hour, then endpoint
    vector<uint64_t> slots = vector<uint64_t>(1 << 12);  // (tag << 32) | (id + 1); 0 = empty
    unique_ptr<uint64_t[]> recs;                         // (group id << 32) | latency
    size_t nrec = 0;
 
    void grow() {
        vector<uint64_t> bigger(slots.size() * 2);
        for (uint64_t e : slots) {
            if (!e) continue;
            uint32_t s = (e >> 32) & (bigger.size() - 1);
            while (bigger[s]) s = (s + 1) & (bigger.size() - 1);
            bigger[s] = e;
        }
        slots.swap(bigger);
    }
 
    uint32_t idOf(const char* ts, const char* ep, uint32_t len) {
        const uint64_t K = 0x9E3779B97F4A7C15ull;
        uint64_t h = len, w;
        memcpy(&w, ts + 5, 8);
        h = (h ^ w) * K;
        uint32_t i = 0;
        for (; i + 8 <= len; i += 8) {
            memcpy(&w, ep + i, 8);
            h = (h ^ w) * K;
            h ^= h >> 29;
        }
        if (i < len) { w = 0; memcpy(&w, ep + i, len - i); h = (h ^ w) * K; }
        h ^= h >> 32; h *= 0xD6E8FEB86659FD93ull; h ^= h >> 32;
 
        uint32_t tag = (uint32_t)h, mask = slots.size() - 1, s = tag & mask;
        for (; slots[s]; s = (s + 1) & mask) {
            if ((uint32_t)(slots[s] >> 32) != tag) continue;
            const Group& g = groups[(uint32_t)slots[s] - 1];
            if (g.eplen == len && !memcmp(&keys[g.koff], ts, 13) && !memcmp(&keys[g.koff + 13], ep, len))
                return (uint32_t)slots[s] - 1;
        }
        uint32_t id = groups.size();
        groups.push_back({(uint32_t)keys.size(), len, 0, 0, 0, hourOf(ts)});
        keys.insert(keys.end(), ts, ts + 13);
        keys.insert(keys.end(), ep, ep + len);
        slots[s] = ((uint64_t)tag << 32) | (id + 1);
        if (groups.size() * 2 > slots.size()) grow();
        return id;
    }
 
    // Parses whole lines in [p, end): "ts method endpoint status latency\n".
    void ingest(const char* p, const char* end) {
        // Smallest possible line is 38 bytes, so this bound can never overflow.
        recs = make_unique_for_overwrite<uint64_t[]>((end - p) / 38 + 1);  // untouched pages cost nothing
        uint64_t* out = recs.get();
        uint32_t lastId = UINT32_MAX;
        const char* lastEp = nullptr;
        uint32_t lastLen = 0;
        char lastHour[13] = {};
        while (p < end) {
            const char* ts = p;
            p += 28;
            p = (const char*)memchr(p, ' ', 16);   // method
            const char* ep = ++p;
            while (*p != ' ') p++;                 // endpoint
            uint32_t len = p - ep;
            uint32_t id;
            // Cheap repeat check: same hour + same endpoint as previous line (common for bursts).
            if (lastId != UINT32_MAX && len == lastLen && !memcmp(ts, lastHour, 13) && !memcmp(ep, lastEp, len))
                id = lastId;
            else {
                id = idOf(ts, ep, len);
                lastId = id; lastLen = len;
                memcpy(lastHour, ts, 13);
                lastEp = &keys[groups[id].koff + 13];  // stable until next insert; refreshed on every miss
            }
            Group& g = groups[id];
            g.count++;
            g.errors += p[1] == '5';
            uint32_t lat = 0;
            for (p += 5; *p != '\n'; p++) lat = lat * 10 + (*p - '0');
            p++;
            *out++ = (uint64_t)id << 32 | lat;
        }
        nrec = out - recs.get();
    }
};
 
template <class F>
static void parallel(int n, F f) {
    vector<thread> workers;
    for (int i = 1; i < n; i++) workers.emplace_back(f, i);
    f(0);
    for (thread& w : workers) w.join();
}
 
// Exact nearest-rank percentiles. Uses a counting histogram for big groups with small latencies
// (O(c + max), no data movement), otherwise falls back to three nth_element calls.
static void select3(uint32_t* b, uint64_t c, const uint64_t rk[3], vector<uint32_t>& hist, uint32_t out[3]) {
    if (c >= 2048) {
        uint32_t mx = 0;
        for (uint64_t i = 0; i < c; i++) mx = max(mx, b[i]);
        if (mx < (1u << 20) && mx <= 4 * c) {
            if (hist.size() < (size_t)mx + 1) hist.resize((size_t)mx + 1);
            memset(hist.data(), 0, ((size_t)mx + 1) * 4);
            for (uint64_t i = 0; i < c; i++) hist[b[i]]++;
            uint64_t cum = 0;
            int k = 0;
            for (uint32_t v = 0; k < 3; v++) {
                cum += hist[v];
                while (k < 3 && cum > rk[k]) out[k++] = v;
            }
            return;
        }
    }
    nth_element(b, b + rk[2], b + c);
    nth_element(b, b + rk[1], b + rk[2]);
    nth_element(b, b + rk[0], b + rk[1]);
    out[0] = b[rk[0]]; out[1] = b[rk[1]]; out[2] = b[rk[2]];
}
 
int main(int argc, char* argv[]) {
    string dataDir = argc > 1 ? argv[1] : "/data";
    string outDir  = argc > 2 ? argv[2] : "/out";
 
    int fd = open((dataDir + "/requests.log").c_str(), O_RDONLY);
    if (fd < 0) { fprintf(stderr, "Cannot open requests.log\n"); return 1; }
    size_t size = lseek(fd, 0, SEEK_END);
    if (size == 0) { fprintf(stderr, "empty log\n"); return 1; }
    const char* data = (const char*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) { fprintf(stderr, "mmap failed\n"); return 1; }
    madvise((void*)data, size, MADV_SEQUENTIAL);
    close(fd);
 
    // Ignore an unterminated last line (same as the original).
    while (size > 0 && data[size - 1] != '\n') size--;
    if (size == 0) return 0;
 
    cpu_set_t cpus;
    sched_getaffinity(0, sizeof cpus, &cpus);
    int n = clamp(CPU_COUNT(&cpus), 1, 64);
 
    vector<size_t> cut(n + 1, size);
    cut[0] = 0;
    for (int i = 1; i < n; i++) {
        size_t lo = size * i / n;
        const char* nl = (const char*)memchr(data + lo - 1, '\n', size - (lo - 1));
        cut[i] = nl ? nl + 1 - data : size;
    }
 
    vector<Table> parts(n);
    parallel(n, [&](int i) {
        if (cut[i + 1] > cut[i]) {
            // Pre-fault this thread's slice in parallel instead of taking 4K page faults while parsing.
            size_t a = cut[i] & ~size_t(4095);
            madvise((void*)(data + a), cut[i + 1] - a, MADV_POPULATE_READ);
            parts[i].ingest(data + cut[i], data + cut[i + 1]);
        }
    });
 
    Table all;
    all.slots.assign(1 << 18, 0);
    all.groups.reserve(1 << 18);
    all.keys.reserve(1 << 23);
    for (Table& t : parts) {
        for (Group& l : t.groups) {
            uint32_t g = all.idOf(&t.keys[l.koff], &t.keys[l.koff + 13], l.eplen);
            all.groups[g].count += l.count;
            all.groups[g].errors += l.errors;
            l.start = g;
        }
        vector<char>().swap(t.keys);
    }
    uint32_t total = 0;
    for (Group& g : all.groups) { g.start = total; total += g.count; }
    vector<uint32_t> fill(all.groups.size());
    for (Table& t : parts)
        for (Group& l : t.groups) {
            uint32_t g = l.start;
            l.start = all.groups[g].start + fill[g];
            fill[g] += l.count;
        }
    auto vals = make_unique_for_overwrite<uint32_t[]>(total);
    parallel(n, [&](int i) {
        Table& t = parts[i];
        for (size_t k = 0; k < t.nrec; k++) {
            uint64_t r = t.recs[k];
            vals[t.groups[r >> 32].start++] = (uint32_t)r;
        }
        t.recs.reset();
    });
    munmap((void*)data, size);
 
    // Order groups by hour (counting sort), then endpoint bytes.
    uint32_t lo = UINT32_MAX, hi = 0;
    for (const Group& g : all.groups) { lo = min(lo, g.hv); hi = max(hi, g.hv); }
    vector<uint32_t> first(hi - lo + 2);
    for (const Group& g : all.groups) first[g.hv - lo + 1]++;
    partial_sum(first.begin(), first.end(), first.begin());
    vector<uint32_t> order(all.groups.size()), put(first.begin(), first.end() - 1);
    for (uint32_t i = 0; i < order.size(); i++) order[put[all.groups[i].hv - lo]++] = i;
    parallel(n, [&](int i) {
        for (size_t h = i; h + 1 < first.size(); h += n)
            sort(order.begin() + first[h], order.begin() + first[h + 1], [&](uint32_t a, uint32_t b) {
                const Group &x = all.groups[a], &y = all.groups[b];
                int c = memcmp(&all.keys[x.koff + 13], &all.keys[y.koff + 13], min(x.eplen, y.eplen));
                return c ? c < 0 : x.eplen < y.eplen;
            });
    });
 
    // Balance output work by record count (percentile cost ~ group size), not group count.
    size_t m = order.size();
    vector<size_t> bound(n + 1, m);
    {
        uint64_t acc = 0; int k = 1;
        bound[0] = 0;
        for (size_t j = 0; j < m && k < n; j++) {
            acc += all.groups[order[j]].count + 64;
            if (acc * n >= (uint64_t)(total + 64 * m) * k) bound[k++] = j + 1;
        }
        for (; k < n; k++) bound[k] = m;
    }
    vector<string> outs(n);
    parallel(n, [&](int i) {
        string& s = outs[i];
        s.reserve(90 * (bound[i + 1] - bound[i]));
        vector<uint32_t> hist;
        auto num = [&](uint64_t v, char sep) {
            char b[24];
            s.append(b, to_chars(b, b + 24, v).ptr);
            s += sep;
        };
        for (size_t j = bound[i]; j < bound[i + 1]; j++) {
            const Group& g = all.groups[order[j]];
            uint64_t c = g.count;
            uint64_t rk[3] = {(50 * c + 99) / 100 - 1, (95 * c + 99) / 100 - 1, (99 * c + 99) / 100 - 1};
            uint32_t p[3];
            select3(vals.get() + g.start, c, rk, hist, p);
 
            s.append(&all.keys[g.koff], 13);
            s += ":00:00Z ";
            s.append(&all.keys[g.koff + 13], g.eplen);
            s += ' ';
            num(c, ' ');
            num(p[0], ' ');
            num(p[1], ' ');
            num(p[2], ' ');
            num(g.errors, ' ');
            num(g.errors * 1000000ULL / c, '\n');
        }
    });
 
    filesystem::create_directories(outDir);
    FILE* f = fopen((outDir + "/summary.txt").c_str(), "wb");
    if (!f) { fprintf(stderr, "Cannot create summary.txt\n"); return 1; }
    for (const string& s : outs) fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    return 0;
}
 
