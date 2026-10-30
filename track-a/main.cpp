#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

using namespace std;

struct Group { uint32_t koff, eplen, count = 0, start = 0; uint64_t errors = 0; };

static vector<Group> groups;
static vector<char> keys;                // per group: 13-byte hour, then endpoint
static vector<uint64_t> table(1 << 18);  // (tag << 32) | (id + 1); 0 = empty
static vector<uint64_t> recs;            // (group id << 32) | latency

static void grow() {
    vector<uint64_t> bigger(table.size() * 2);
    uint32_t mask = bigger.size() - 1;
    for (uint64_t e : table) {
        if (!e) continue;
        uint32_t s = (e >> 32) & mask;
        while (bigger[s]) s = (s + 1) & mask;
        bigger[s] = e;
    }
    table.swap(bigger);
}

static uint32_t findGroup(const char* ts, const char* ep, uint32_t len) {
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

    uint32_t tag = (uint32_t)h, mask = table.size() - 1, s = tag & mask;
    for (;; s = (s + 1) & mask) {
        uint64_t e = table[s];
        if (!e) break;
        if ((uint32_t)(e >> 32) != tag) continue;
        uint32_t id = (uint32_t)e - 1;
        const Group& g = groups[id];
        if (g.eplen == len && !memcmp(&keys[g.koff], ts, 13) && !memcmp(&keys[g.koff + 13], ep, len))
            return id;
    }
    uint32_t id = groups.size();
    groups.push_back({(uint32_t)keys.size(), len});
    keys.insert(keys.end(), ts, ts + 13);
    keys.insert(keys.end(), ep, ep + len);
    table[s] = ((uint64_t)tag << 32) | (id + 1);
    if (groups.size() * 2 > table.size()) grow();
    return id;
}

// Parses whole lines in [p, end): "ts method endpoint status latency\n".
static void ingest(const char* p, const char* end) {
    while (p < end) {
        const char* ts = p;
        p += 28;                       // 27-byte timestamp + space
        while (*p != ' ') p++;         // method
        const char* ep = ++p;
        while (*p != ' ') p++;         // endpoint
        uint32_t id = findGroup(ts, ep, p - ep);
        bool is5xx = p[1] == '5';
        uint32_t lat = 0;
        for (p += 5; *p != '\n'; p++) lat = lat * 10 + (*p - '0');
        p++;
        groups[id].count++;
        groups[id].errors += is5xx;
        recs.push_back((uint64_t)id << 32 | lat);
    }
}

int main(int argc, char* argv[]) {
    string dataDir = argc > 1 ? argv[1] : "/data";
    string outDir  = argc > 2 ? argv[2] : "/out";

    int fd = open((dataDir + "/requests.log").c_str(), O_RDONLY);
    if (fd < 0) { fprintf(stderr, "Cannot open requests.log\n"); return 1; }
    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    recs.reserve(size / 40 + 1);       // a line is at least 40 bytes
    groups.reserve(1 << 18);
    keys.reserve(1 << 23);

    vector<char> buf(1 << 20);
    size_t have = 0;
    for (ssize_t n; (n = read(fd, buf.data() + have, buf.size() - have)) > 0;) {
        have += n;
        char* last = (char*)memrchr(buf.data(), '\n', have);
        if (!last) continue;
        ingest(buf.data(), last + 1);
        have -= last + 1 - buf.data();
        memmove(buf.data(), last + 1, have);
    }
    close(fd);

    // Put each group's records in its own slice, in place (American flag sort).
    uint32_t total = 0;
    vector<uint32_t> next(groups.size());
    for (size_t g = 0; g < groups.size(); g++) {
        groups[g].start = next[g] = total;
        total += groups[g].count;
    }
    for (uint32_t g = 0; g < groups.size(); g++) {
        uint32_t end = groups[g].start + groups[g].count;
        while (next[g] < end) {
            uint64_t r = recs[next[g]];
            uint32_t t = r >> 32;
            if (t == g) { next[g]++; continue; }
            swap(r, recs[next[t]++]);
            recs[next[g]] = r;
        }
    }

    vector<uint32_t> order(groups.size());
    iota(order.begin(), order.end(), 0);
    sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) {
        const Group &x = groups[a], &y = groups[b];
        int c = memcmp(&keys[x.koff], &keys[y.koff], 13);
        if (!c) c = memcmp(&keys[x.koff + 13], &keys[y.koff + 13], min(x.eplen, y.eplen));
        return c ? c < 0 : x.eplen < y.eplen;
    });

    filesystem::create_directories(outDir);
    FILE* f = fopen((outDir + "/summary.txt").c_str(), "wb");
    if (!f) { fprintf(stderr, "Cannot create summary.txt\n"); return 1; }

    string out;
    auto num = [&](uint64_t v, char sep) {
        char b[24];
        out.append(b, to_chars(b, b + 24, v).ptr);
        out += sep;
    };
    for (uint32_t id : order) {
        const Group& g = groups[id];
        uint64_t n = g.count;
        uint64_t* b = recs.data() + g.start;
        uint64_t r50 = (50 * n + 99) / 100 - 1, r95 = (95 * n + 99) / 100 - 1, r99 = (99 * n + 99) / 100 - 1;
        nth_element(b, b + r99, b + n);
        nth_element(b, b + r95, b + r99);
        nth_element(b, b + r50, b + r95);

        out.append(&keys[g.koff], 13);
        out.append(":00:00Z ");
        out.append(&keys[g.koff + 13], g.eplen);
        out += ' ';
        num(n, ' ');
        num((uint32_t)b[r50], ' ');
        num((uint32_t)b[r95], ' ');
        num((uint32_t)b[r99], ' ');
        num(g.errors, ' ');
        num(g.errors * 1000000ULL / n, '\n');
        if (out.size() > (1 << 16)) { fwrite(out.data(), 1, out.size(), f); out.clear(); }
    }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return 0;
}