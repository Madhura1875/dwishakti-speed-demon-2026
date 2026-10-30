#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace std;

struct G {
    const char* hour;
    const char* ep;
    uint32_t eplen, hash, count = 0, start = 0;
    uint64_t errors = 0;
};

static uint32_t rankOf(uint64_t p, uint64_t n) { return (uint32_t)((p * n + 99) / 100) - 1; }

int main(int argc, char* argv[]) {
    string dataDir = argc > 1 ? argv[1] : "/data";
    string outDir  = argc > 2 ? argv[2] : "/out";

    int fd = open((dataDir + "/requests.log").c_str(), O_RDONLY);
    if (fd < 0) { fprintf(stderr, "Cannot open requests.log\n"); return 1; }
    struct stat st;
    fstat(fd, &st);
    size_t size = st.st_size;
    const char* base = "";
    if (size) {
        void* m = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (m == MAP_FAILED) { fprintf(stderr, "mmap failed\n"); return 1; }
        madvise(m, size, MADV_SEQUENTIAL);
        base = (const char*)m;
    }

    vector<G> groups;
    groups.reserve(1 << 16);
    uint32_t tsize = 1 << 18, mask = tsize - 1;
    vector<uint32_t> table(tsize, UINT32_MAX);
    vector<uint32_t> gids, lats;
    gids.reserve(size / 48 + 16);
    lats.reserve(size / 48 + 16);

    const char* p = base;
    const char* end = base + size;
    while (p < end) {
        const char* nl = (const char*)memchr(p, '\n', end - p);
        const char* le = nl ? nl : end;
        const char* next = nl ? nl + 1 : end;

        const char* s1 = (const char*)memchr(p, ' ', le - p);
        if (!s1) { p = next; continue; }
        const char* s2 = (const char*)memchr(s1 + 1, ' ', le - s1 - 1);
        if (!s2) { p = next; continue; }
        const char* s3 = (const char*)memchr(s2 + 1, ' ', le - s2 - 1);
        if (!s3) { p = next; continue; }
        const char* s4 = (const char*)memchr(s3 + 1, ' ', le - s3 - 1);
        if (!s4) { p = next; continue; }

        const char* ep = s2 + 1;
        uint32_t eplen = (uint32_t)(s3 - ep);

        uint32_t h = 2166136261u;
        for (int i = 0; i < 13; i++) h = (h ^ (uint8_t)p[i]) * 16777619u;
        for (uint32_t i = 0; i < eplen; i++) h = (h ^ (uint8_t)ep[i]) * 16777619u;
        h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12;

        uint32_t i = h & mask, id;
        while (true) {
            id = table[i];
            if (id == UINT32_MAX) {
                id = (uint32_t)groups.size();
                G g;
                g.hour = p; g.ep = ep; g.eplen = eplen; g.hash = h;
                groups.push_back(g);
                table[i] = id;
                if (groups.size() * 2 > tsize) {  // grow
                    tsize <<= 1; mask = tsize - 1;
                    table.assign(tsize, UINT32_MAX);
                    for (uint32_t k = 0; k < groups.size(); k++) {
                        uint32_t j = groups[k].hash & mask;
                        while (table[j] != UINT32_MAX) j = (j + 1) & mask;
                        table[j] = k;
                    }
                }
                break;
            }
            const G& g = groups[id];
            if (g.hash == h && g.eplen == eplen &&
                memcmp(g.hour, p, 13) == 0 && memcmp(g.ep, ep, eplen) == 0) break;
            i = (i + 1) & mask;
        }

        int status = (s3[1] - '0') * 100 + (s3[2] - '0') * 10 + (s3[3] - '0');
        uint32_t lat = 0;
        for (const char* q = s4 + 1; q < le && *q >= '0' && *q <= '9'; q++)
            lat = lat * 10 + (*q - '0');

        G& g = groups[id];
        g.count++;
        if (status >= 500 && status <= 599) g.errors++;
        gids.push_back(id);
        lats.push_back(lat);
        p = next;
    }

    // Offsets, then scatter latencies into one contiguous array.
    uint32_t total = 0;
    for (G& g : groups) { g.start = total; total += g.count; }
    vector<uint32_t> cursor(groups.size());
    for (size_t k = 0; k < groups.size(); k++) cursor[k] = groups[k].start;
    vector<uint32_t> vals(total);
    for (size_t k = 0; k < gids.size(); k++) vals[cursor[gids[k]]++] = lats[k];
    vector<uint32_t>().swap(gids);
    vector<uint32_t>().swap(lats);

    vector<uint32_t> order(groups.size());
    for (uint32_t k = 0; k < order.size(); k++) order[k] = k;
    sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        int c = memcmp(groups[a].hour, groups[b].hour, 13);
        if (c) return c < 0;
        uint32_t m = min(groups[a].eplen, groups[b].eplen);
        c = memcmp(groups[a].ep, groups[b].ep, m);
        if (c) return c < 0;
        return groups[a].eplen < groups[b].eplen;
    });

    string out;
    out.reserve(groups.size() * 64);
    char buf[24];
    auto num = [&](uint64_t v) {
        auto r = to_chars(buf, buf + 24, v);
        out.append(buf, r.ptr);
    };

    for (uint32_t id : order) {
        G& g = groups[id];
        uint32_t* b = vals.data() + g.start;
        uint32_t* e = b + g.count;
        uint32_t r50 = rankOf(50, g.count), r95 = rankOf(95, g.count), r99 = rankOf(99, g.count);
        nth_element(b, b + r99, e);
        nth_element(b, b + r95, b + r99);
        nth_element(b, b + r50, b + r95);

        out.append(g.hour, 13);
        out += ":00:00Z ";
        out.append(g.ep, g.eplen);
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