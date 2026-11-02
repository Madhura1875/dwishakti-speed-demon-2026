// Track A: group 3.1M log lines by (hour, endpoint) and report count / p50 / p95 / p99 / 5xx.
//
// Why it is fast: every timestamp lies in the same 72 hours (spec section 2), so an hour is a number 0..71 and a
// group is simply hour * E + rank, where rank is the endpoint's position in byte order. Group ids therefore
// come out already sorted: no per-group table, no sort of groups, only one sort of the ~1.8k distinct endpoints.
// Each thread reads its own slice of the file, counts per (hour, local endpoint) and keeps (key, latency)
// records; after a cheap merge the records are scattered into one array grouped by group, and the
// percentiles come from nth_element.
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
using namespace std;
typedef uint32_t u32;
typedef uint64_t u64;
constexpr int HOURS = 72;
static inline u64 spaceMask(u64 w) {  // lowest set 0x80 bit marks the first ' ' byte
    u64 x = w ^ 0x2020202020202020ull;
    return (x - 0x0101010101010101ull) & ~x & 0x8080808080808080ull;
}

// One slice of the file: endpoint dictionary, per-(endpoint, hour) counters and the records.
struct Part {
    vector<u32> slots = vector<u32>(1 << 12);      // open addressing, endpoint id + 1, 0 = empty
    vector<u32> off, len;                          // endpoint id -> bytes inside `names`
    vector<u64> hs;                                // endpoint id -> its hash
    string names;
    vector<u32> cnt, err;                          // indexed by key = endpoint id * HOURS + hour
    vector<u64> recs;                              // key << 32 | latency

    u32 idOf(const char* e, u32 n, u64 h) {
        u32 mask = slots.size() - 1, s = h & mask;
        for (; slots[s]; s = (s + 1) & mask)
            if (len[slots[s] - 1] == n && !memcmp(&names[off[slots[s] - 1]], e, n)) return slots[s] - 1;
        u32 id = off.size();
        off.push_back(names.size()); len.push_back(n); hs.push_back(h);
        names.append(e, n);
        cnt.resize(cnt.size() + HOURS); err.resize(err.size() + HOURS);
        slots[s] = id + 1;
        if (off.size() * 2 > slots.size()) {       // grow and rehash
            vector<u32> old(slots.size() * 2);
            old.swap(slots);
            for (u32 e2 = 0; e2 < off.size(); e2++) {
                u64 h2 = hs[e2];
                u32 t = h2 & (slots.size() - 1);
                while (slots[t]) t = (t + 1) & (slots.size() - 1);
                slots[t] = e2 + 1;
            }
        }
        return id;
    }

    // Whole lines "ts method endpoint status latency\n" in [p, end).
    void ingest(const char* p, const char* end) {
        while (p < end) {
            const char* ts = p;
            // hour 0 = 2026-10-30T18; October has 31 days, so November day d is day 31 + d.
            u32 day = (ts[6] == '1' ? 31 : 0) + (ts[8] - '0') * 10 + ts[9] - '0';
            u32 hour = (day * 24 + (ts[11] - '0') * 10 + ts[12] - '0') - (30 * 24 + 18);
            p += 28;
            u64 w, m;
            memcpy(&w, p, 8);                      // method: up to 7 bytes and a space
            if ((m = spaceMask(w))) p += (__builtin_ctzll(m) >> 3) + 1;
            else { p += 8; while (*p != ' ') p++; p++; }
            const char* ep = p;
            u64 h = 0x9E3779B97F4A7C15ull;
            for (;;) {                             // endpoint: find the space and hash in one pass, 8 bytes at a time
                memcpy(&w, p, 8);
                if ((m = spaceMask(w))) {
                    unsigned k = __builtin_ctzll(m) >> 3;
                    if (k) h = (h ^ (w & (~0ull >> (64 - 8 * k)))) * 0x9E3779B97F4A7C15ull;
                    p += k;
                    break;
                }
                h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29;
                p += 8;
            }
            u32 elen = p - ep;
            h = (h ^ elen) * 0x9E3779B97F4A7C15ull; h ^= h >> 32; h *= 0xD6E8FEB86659FD93ull; h ^= h >> 32;
            u32 key = idOf(ep, elen, h) * HOURS + hour;
            cnt[key]++;
            err[key] += p[1] == '5';
            u32 lat = 0;
            for (p += 5; *p != '\n'; p++) lat = lat * 10 + (*p - '0');
            p++;
            recs.push_back((u64)key << 32 | lat);
        }
    }

    void scan(int fd, off_t pos, off_t end) {
        vector<char> buf((1 << 18) + 64);   // slack: the 8-byte loads may run a little past the data
        size_t have = 0;
        while (pos < end) {
            ssize_t got = pread(fd, buf.data() + have, min<off_t>((1 << 18) - have, end - pos), pos);
            if (got <= 0) break;
            pos += got; have += got;
            char* last = (char*)memrchr(buf.data(), '\n', have);
            if (!last) continue;
            ingest(buf.data(), last + 1);
            have -= last + 1 - buf.data();
            memmove(buf.data(), last + 1, have);
        }
    }
};

template <class F> static void parallel(int n, F f) {
    vector<thread> workers;
    for (int i = 1; i < n; i++) workers.emplace_back(f, i);
    f(0);
    for (thread& w : workers) w.join();
}

int main(int argc, char* argv[]) {
    string dataDir = argc > 1 ? argv[1] : "/data", outDir = argc > 2 ? argv[2] : "/out";
    int fd = open((dataDir + "/requests.log").c_str(), O_RDONLY);
    if (fd < 0) { fprintf(stderr, "Cannot open requests.log\n"); return 1; }
    off_t size = lseek(fd, 0, SEEK_END);
    cpu_set_t cpus;
    sched_getaffinity(0, sizeof cpus, &cpus);
#ifdef NTHREADS
    int n = NTHREADS;
#else
    int n = clamp(CPU_COUNT(&cpus), 1, 16);
#endif

    // Slice i holds the lines that start in [cut[i], cut[i + 1]); each cut is moved to a line start.
    vector<off_t> cut(n + 1, size);
    cut[0] = 0;
    for (int i = 1; i < n; i++) {
        char b[256];
        off_t lo = size * i / n;
        ssize_t got = max<ssize_t>(pread(fd, b, sizeof b, lo - 1), 0);
        if (auto nl = (const char*)memchr(b, '\n', got)) cut[i] = lo + (nl - b);
    }
    vector<Part> parts(n);
    parallel(n, [&](int i) {
        parts[i].recs.reserve((cut[i + 1] - cut[i]) / 64 + 1);
        parts[i].scan(fd, cut[i], cut[i + 1]);
    });
    close(fd);

    // Global endpoint list in byte order. Every part sorts its own (few) endpoints in parallel, then the sorted
    // lists are merged pairwise, so no step here is a long single-threaded loop.
    vector<vector<string_view>> lists(n);
    parallel(n, [&](int i) {
        Part& p = parts[i];
        for (size_t e = 0; e < p.off.size(); e++) lists[i].emplace_back(&p.names[p.off[e]], p.len[e]);
        sort(lists[i].begin(), lists[i].end());
    });
    while (lists.size() > 1) {
        vector<vector<string_view>> merged((lists.size() + 1) / 2);
        for (size_t k = 0; k < lists.size(); k += 2) {
            if (k + 1 == lists.size()) merged[k / 2] = move(lists[k]);
            else set_union(lists[k].begin(), lists[k].end(), lists[k + 1].begin(), lists[k + 1].end(),
                           back_inserter(merged[k / 2]));
        }
        lists.swap(merged);
    }
    const vector<string_view>& eps = lists[0];
    const size_t E = eps.size(), G = E * HOURS;       // group g = hour * E + endpoint rank

    // keyOf[i][r]: part i's id of the endpoint with global rank r (-1 if it never saw it).
    // cursor[i][key]: where part i writes its next latency of that (endpoint, hour).
    vector<vector<int32_t>> keyOf(n);
    vector<vector<u32>> cursor(n);
    parallel(n, [&](int i) {
        Part& p = parts[i];
        keyOf[i].assign(E, -1);
        cursor[i].resize(p.cnt.size());
        for (size_t e = 0; e < p.off.size(); e++)
            keyOf[i][lower_bound(eps.begin(), eps.end(), string_view(&p.names[p.off[e]], p.len[e])) - eps.begin()] = e;
    });
    // Pass 1 (parallel over ranges of groups): count and error totals per group.
    // Pass 2: prefix sums, then every part's cursor. total[g] ends up as the first slot of group g.
    vector<u32> total(G + 1), errors(G);
    vector<u64> chunk(n + 1);
    parallel(n, [&](int t) {
        u64 sum = 0;
        size_t g0 = G * t / n, g1 = G * (t + 1) / n, h = E ? g0 / E : 0, r = E ? g0 % E : 0;
        for (size_t g = g0; g < g1; g++, r == E - 1 ? (r = 0, h++) : r++) {
            u32 c = 0, er = 0;
            for (int i = 0; i < n; i++)
                if (int32_t k = keyOf[i][r]; k >= 0) {
                    c += parts[i].cnt[k * HOURS + h];
                    er += parts[i].err[k * HOURS + h];
                }
            total[g] = c; errors[g] = er; sum += c;
        }
        chunk[t + 1] = sum;
    });
    for (int t = 0; t < n; t++) chunk[t + 1] += chunk[t];
    total[G] = chunk[n];
    parallel(n, [&](int t) {
        u32 run = chunk[t];
        size_t g0 = G * t / n, g1 = G * (t + 1) / n, h = E ? g0 / E : 0, r = E ? g0 % E : 0;
        for (size_t g = g0; g < g1; g++, r == E - 1 ? (r = 0, h++) : r++) {
            u32 c = total[g], off = run;
            for (int i = 0; i < n; i++)
                if (int32_t k = keyOf[i][r]; k >= 0) {
                    size_t key = k * HOURS + h;
                    cursor[i][key] = off;
                    off += parts[i].cnt[key];
                }
            total[g] = run;
            run += c;
        }
    });
    auto vals = make_unique_for_overwrite<u32[]>(total[G]);
    parallel(n, [&](int i) {
        for (u64 r : parts[i].recs) vals[cursor[i][r >> 32]++] = (u32)r;
        vector<u64>().swap(parts[i].recs);
    });

    // Split the groups so that each thread owns about the same number of latencies.
    vector<size_t> split(n + 1, G);
    for (int i = 0; i < n; i++)
        split[i] = upper_bound(total.begin(), total.end(), (u64)total[G] * i / n) - total.begin() - 1;
    split[0] = 0;
    static const char* days[] = {"2026-10-30", "2026-10-31", "2026-11-01", "2026-11-02"};
    vector<string> outs(n);
    parallel(n, [&](int i) {
        string& s = outs[i];
        auto num = [&](u64 v, char sep) {
            char b[24];
            s.append(b, to_chars(b, b + 24, v).ptr);
            s += sep;
        };
        for (size_t g = split[i]; g < split[i + 1]; g++) {
            u64 c = total[g + 1] - total[g];
            if (!c) continue;
            u32* b = vals.get() + total[g];
            u64 r50 = (50 * c + 99) / 100 - 1, r95 = (95 * c + 99) / 100 - 1, r99 = (99 * c + 99) / 100 - 1;
            nth_element(b, b + r99, b + c);
            nth_element(b, b + r95, b + r99);
            nth_element(b, b + r50, b + r95);
            u32 h = g / E + 18;                    // hours since 2026-10-30T00
            char hd[32];
            snprintf(hd, sizeof hd, "%sT%02u:00:00Z ", days[h / 24], h % 24);
            s += hd;
            s += eps[g % E];
            s += ' ';
            num(c, ' '); num(b[r50], ' '); num(b[r95], ' '); num(b[r99], ' ');
            num(errors[g], ' '); num(errors[g] * 1000000ULL / c, '\n');
        }
    });

    filesystem::create_directories(outDir);
    int ofd = open((outDir + "/summary.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) { fprintf(stderr, "Cannot create summary.txt\n"); return 1; }
    vector<size_t> at(n + 1);                         // where each thread's text goes in the file
    for (int i = 0; i < n; i++) at[i + 1] = at[i] + outs[i].size();
    if (ftruncate(ofd, at[n]) != 0) return 1;
    parallel(n, [&](int i) {
        for (size_t done = 0; done < outs[i].size();) {
            ssize_t r = pwrite(ofd, outs[i].data() + done, outs[i].size() - done, at[i] + done);
            if (r <= 0) break;
            done += r;
        }
    });
    close(ofd);
}