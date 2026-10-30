#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

using namespace std;

struct G {
    uint32_t koff, eplen, hash, count = 0, start = 0, fill = 0;
    uint64_t errors = 0;
};

static vector<G> groups;
static vector<char> keys;  // per group: 13 bytes hour, then endpoint
static vector<uint32_t> table(1 << 16, UINT32_MAX);
static uint32_t mask = (1 << 16) - 1;

static uint32_t findGroup(const char* p, const char* ep, uint32_t eplen) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 13; i++) h = (h ^ (uint8_t)p[i]) * 16777619u;
    for (uint32_t i = 0; i < eplen; i++) h = (h ^ (uint8_t)ep[i]) * 16777619u;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12;
    uint32_t i = h & mask;
    while (true) {
        uint32_t id = table[i];
        if (id == UINT32_MAX) {
            id = (uint32_t)groups.size();
            G g;
            g.koff = (uint32_t)keys.size(); g.eplen = eplen; g.hash = h;
            keys.insert(keys.end(), p, p + 13);
            keys.insert(keys.end(), ep, ep + eplen);
            groups.push_back(g);
            table[i] = id;
            if (groups.size() * 2 > table.size()) {
                table.assign(table.size() * 2, UINT32_MAX);
                mask = (uint32_t)table.size() - 1;
                for (uint32_t k = 0; k < groups.size(); k++) {
                    uint32_t j = groups[k].hash & mask;
                    while (table[j] != UINT32_MAX) j = (j + 1) & mask;
                    table[j] = k;
                }
            }
            return id;
        }
        const G& g = groups[id];
        if (g.hash == h && g.eplen == eplen &&
            memcmp(&keys[g.koff], p, 13) == 0 && memcmp(&keys[g.koff + 13], ep, eplen) == 0)
            return id;
        i = (i + 1) & mask;
    }
}

// Splits line into fields; returns false if malformed.
static bool parseLine(const char* p, const char* le, uint32_t& id, int& status, uint32_t& lat) {
    const char* s1 = (const char*)memchr(p, ' ', le - p);
    if (!s1) return false;
    const char* s2 = (const char*)memchr(s1 + 1, ' ', le - s1 - 1);
    if (!s2) return false;
    const char* s3 = (const char*)memchr(s2 + 1, ' ', le - s2 - 1);
    if (!s3 || le - s3 < 4) return false;
    const char* s4 = (const char*)memchr(s3 + 1, ' ', le - s3 - 1);
    if (!s4) return false;
    id = findGroup(p, s2 + 1, (uint32_t)(s3 - s2 - 1));
    status = (s3[1] - '0') * 100 + (s3[2] - '0') * 10 + (s3[3] - '0');
    lat = 0;
    for (const char* q = s4 + 1; q < le && *q >= '0' && *q <= '9'; q++) lat = lat * 10 + (*q - '0');
    return true;
}

template <class F>
static void scan(int fd, F f) {
    lseek(fd, 0, SEEK_SET);
    vector<char> buf(1 << 20);
    size_t have = 0;
    while (true) {
        ssize_t n = read(fd, buf.data() + have, buf.size() - have);
        if (n <= 0) break;
        have += n;
        char* last = (char*)memrchr(buf.data(), '\n', have);
        if (!last) {
            if (have == buf.size()) buf.resize(buf.size() * 2);
            continue;
        }
        char* p = buf.data();
        char* e = last + 1;
        while (p < e) {
            char* nl = (char*)memchr(p, '\n', e - p);
            f(p, nl);
            p = nl + 1;
        }
        have = (buf.data() + have) - e;
        memmove(buf.data(), e, have);
    }
    if (have) f(buf.data(), buf.data() + have);
}

static uint32_t rankOf(uint64_t p, uint64_t n) { return (uint32_t)((p * n + 99) / 100) - 1; }

int main(int argc, char* argv[]) {
    string dataDir = argc > 1 ? argv[1] : "/data";
    string outDir  = argc > 2 ? argv[2] : "/out";

    int fd = open((dataDir + "/requests.log").c_str(), O_RDONLY);
    if (fd < 0) { fprintf(stderr, "Cannot open requests.log\n"); return 1; }

    // Pass 1: counts and errors per group.
    scan(fd, [](const char* p, const char* le) {
        uint32_t id, lat; int status;
        if (!parseLine(p, le, id, status, lat)) return;
        G& g = groups[id];
        g.count++;
        if (status >= 500 && status <= 599) g.errors++;
    });

    uint32_t total = 0;
    for (G& g : groups) { g.start = total; total += g.count; }
    vector<uint32_t> vals(total);

    // Pass 2: place each latency directly into its group's slice.
    scan(fd, [&](const char* p, const char* le) {
        uint32_t id, lat; int status;
        if (!parseLine(p, le, id, status, lat)) return;
        G& g = groups[id];
        vals[g.start + g.fill++] = lat;
    });
    close(fd);

    vector<uint32_t> order(groups.size());
    for (uint32_t k = 0; k < order.size(); k++) order[k] = k;
    sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const G &x = groups[a], &y = groups[b];
        int c = memcmp(&keys[x.koff], &keys[y.koff], 13);
        if (c) return c < 0;
        c = memcmp(&keys[x.koff + 13], &keys[y.koff + 13], min(x.eplen, y.eplen));
        if (c) return c < 0;
        return x.eplen < y.eplen;
    });

    string out;
    out.reserve(groups.size() * 64);
    char buf[24];
    auto num = [&](uint64_t v) { auto r = to_chars(buf, buf + 24, v); out.append(buf, r.ptr); };

    for (uint32_t id : order) {
        G& g = groups[id];
        uint32_t* b = vals.data() + g.start;
        uint32_t* e = b + g.count;
        uint32_t r50 = rankOf(50, g.count), r95 = rankOf(95, g.count), r99 = rankOf(99, g.count);
        nth_element(b, b + r99, e);
        nth_element(b, b + r95, b + r99);
        nth_element(b, b + r50, b + r95);

        out.append(&keys[g.koff], 13);
        out += ":00:00Z ";
        out.append(&keys[g.koff + 13], g.eplen);
        out += ' '; num(g.count);
        out += ' '; num(b[r50]);
        out += ' '; num(b[r95]);
        out += ' '; num(b[r99]);
        out += ' '; num(g.errors);
        out += ' '; num(g.errors * 1000000ULL / g.count);
        out += '\n';
    }

    filesystem::create_directories(outDir);
    FILE* f = fopen((outDir + "/summary.txt").c_str(), "wb");
    if (!f) { fprintf(stderr, "Cannot create summary.txt\n"); return 1; }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return 0;
}