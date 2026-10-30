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
#include <unistd.h>

using namespace std;

struct Group { uint32_t koff, eplen, count = 0, start = 0; uint64_t errors = 0; };

// The (hour, endpoint) groups seen so far, with an open-addressing hash index.
struct Table {
    vector<Group> groups;
    vector<char> keys;                                   // per group: 13-byte hour, then endpoint
    vector<uint64_t> slots = vector<uint64_t>(1 << 12);  // (tag << 32) | (id + 1); 0 = empty
    vector<uint64_t> recs;                               // (group id << 32) | latency

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
        memcpy(&w, ts + 5, 8);  // "MM-DDTHH"
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
        groups.push_back({(uint32_t)keys.size(), len});
        keys.insert(keys.end(), ts, ts + 13);
        keys.insert(keys.end(), ep, ep + len);
        slots[s] = ((uint64_t)tag << 32) | (id + 1);
        if (groups.size() * 2 > slots.size()) grow();
        return id;
    }

    // Parses whole lines in [p, end): "ts method endpoint status latency\n".
    void ingest(const char* p, const char* end) {
        while (p < end) {
            const char* ts = p;
            p += 28;                   // 27-byte timestamp + space
            while (*p != ' ') p++;     // method
            const char* ep = ++p;
            while (*p != ' ') p++;     // endpoint
            uint32_t id = idOf(ts, ep, p - ep);
            groups[id].count++;
            groups[id].errors += p[1] == '5';
            uint32_t lat = 0;
            for (p += 5; *p != '\n'; p++) lat = lat * 10 + (*p - '0');
            p++;
            recs.push_back((uint64_t)id << 32 | lat);
        }
    }
};

// Reads the lines that start in [pos, end) of the file, in chunks.
static void scan(int fd, off_t pos, off_t end, Table& t) {
    vector<char> buf(1 << 18);
    size_t have = 0;
    while (pos < end) {
        size_t want = min<off_t>(buf.size() - have, end - pos);
        ssize_t got = pread(fd, buf.data() + have, want, pos);
        if (got <= 0) break;
        pos += got;
        have += got;
        char* last = (char*)memrchr(buf.data(), '\n', have);
        if (!last) continue;
        t.ingest(buf.data(), last + 1);
        have -= last + 1 - buf.data();
        memmove(buf.data(), last + 1, have);
    }
}

template <class F>
static void parallel(int n, F f) {
    vector<thread> workers;
    for (int i = 1; i < n; i++) workers.emplace_back(f, i);
    f(0);
    for (thread& w : workers) w.join();
}

int main(int argc, char* argv[]) {
    string dataDir = argc > 1 ? argv[1] : "/data";
    string outDir  = argc > 2 ? argv[2] : "/out";

    int fd = open((dataDir + "/requests.log").c_str(), O_RDONLY);
    if (fd < 0) { fprintf(stderr, "Cannot open requests.log\n"); return 1; }
    off_t size = lseek(fd, 0, SEEK_END);
    cpu_set_t cpus;
    sched_getaffinity(0, sizeof cpus, &cpus);
    int n = clamp(CPU_COUNT(&cpus), 1, 16);

    // Slice i is the lines starting in [cut[i], cut[i + 1]); cuts sit on line starts.
    vector<off_t> cut(n + 1, size);
    cut[0] = 0;
    for (int i = 1; i < n; i++) {
        off_t lo = size * i / n;
        char b[256];
        ssize_t got = max<ssize_t>(pread(fd, b, sizeof b, lo - 1), 0);
        const char* nl = (const char*)memchr(b, '\n', got);
        if (nl) cut[i] = lo + (nl - b);
    }

    vector<Table> parts(n);
    parallel(n, [&](int i) {
        parts[i].recs.reserve((cut[i + 1] - cut[i]) / 40 + 1);  // a line is at least 40 bytes
        scan(fd, cut[i], cut[i + 1], parts[i]);
    });
    close(fd);

    // Merge the slices' groups into one table and give each group a slice of vals.
    Table all;
    for (Table& t : parts) {
        for (Group& l : t.groups) {
            uint32_t g = all.idOf(&t.keys[l.koff], &t.keys[l.koff + 13], l.eplen);
            all.groups[g].count += l.count;
            all.groups[g].errors += l.errors;
            l.start = g;  // for now: local group -> global id
        }
        vector<char>().swap(t.keys);
    }
    uint32_t total = 0;
    for (Group& g : all.groups) { g.start = total; total += g.count; }
    vector<uint32_t> fill(all.groups.size());
    for (Table& t : parts)
        for (Group& l : t.groups) {
            uint32_t g = l.start;
            l.start = all.groups[g].start + fill[g];  // where this slice's records of g go
            fill[g] += l.count;
        }
    auto vals = make_unique_for_overwrite<uint32_t[]>(total);
    parallel(n, [&](int i) {
        Table& t = parts[i];
        for (uint64_t r : t.recs) vals[t.groups[r >> 32].start++] = (uint32_t)r;
        vector<uint64_t>().swap(t.recs);
    });

    // Sort the groups by hour, then endpoint (unsigned bytes): sorted chunks, then merged.
    vector<uint32_t> order(all.groups.size());
    iota(order.begin(), order.end(), 0);
    auto before = [&](uint32_t a, uint32_t b) {
        const Group &x = all.groups[a], &y = all.groups[b];
        int c = memcmp(&all.keys[x.koff], &all.keys[y.koff], 13);
        if (!c) c = memcmp(&all.keys[x.koff + 13], &all.keys[y.koff + 13], min(x.eplen, y.eplen));
        return c ? c < 0 : x.eplen < y.eplen;
    };
    size_t m = order.size();
    auto at = [&](int i) { return order.begin() + m * i / n; };
    parallel(n, [&](int i) { sort(at(i), at(i + 1), before); });
    for (int w = 1; w < n; w *= 2)
        for (int i = 0; i + w < n; i += 2 * w)
            inplace_merge(at(i), at(i + w), at(min(i + 2 * w, n)), before);

    // Each thread turns its share of the sorted groups into output text.
    vector<string> outs(n);
    parallel(n, [&](int i) {
        string& s = outs[i];
        s.reserve(90 * (at(i + 1) - at(i)));
        auto num = [&](uint64_t v, char sep) {
            char b[24];
            s.append(b, to_chars(b, b + 24, v).ptr);
            s += sep;
        };
        for (auto it = at(i); it != at(i + 1); ++it) {
            const Group& g = all.groups[*it];
            uint64_t c = g.count;
            uint32_t* b = vals.get() + g.start;
            uint64_t r50 = (50 * c + 99) / 100 - 1, r95 = (95 * c + 99) / 100 - 1, r99 = (99 * c + 99) / 100 - 1;
            nth_element(b, b + r99, b + c);
            nth_element(b, b + r95, b + r99);
            nth_element(b, b + r50, b + r95);

            s.append(&all.keys[g.koff], 13);
            s += ":00:00Z ";
            s.append(&all.keys[g.koff + 13], g.eplen);
            s += ' ';
            num(c, ' ');
            num(b[r50], ' ');
            num(b[r95], ' ');
            num(b[r99], ' ');
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