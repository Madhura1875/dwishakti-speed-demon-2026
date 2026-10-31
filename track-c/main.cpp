#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace std;

struct Agg {
    uint64_t count = 0, idSum = 0, nameBytes = 0, noteBytes = 0;
    int64_t sum = 0, lo = INT64_MAX, hi = INT64_MIN;
};

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

static uint64_t hashBytes(const char* s, size_t n) {
    const uint64_t K = 0x9E3779B97F4A7C15ull;
    uint64_t h = n * K, w;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        memcpy(&w, s + i, 8);
        h = (h ^ w) * K;
        h ^= h >> 32;
    }
    if (i < n) { w = 0; memcpy(&w, s + i, n - i); h = (h ^ w) * K; }
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
    return h;
}

// One thread's share of the work: aggregates per country plus a hash of every record's name.
struct Worker {
    const vector<uint16_t>* region;  // region id -> country index + 1, 0 if absent
    size_t groups;                   // countries + 1; the last group is _UNMATCHED
    vector<Agg> agg;
    vector<vector<uint64_t>> names;
    string tmp;

    Worker(const vector<uint16_t>* r, size_t g) : region(r), groups(g), agg(g), names(g) {}

    // Reads one text field starting at p. Returns a pointer to the comma that ends it.
    // len is the raw length between the delimiting quotes; esc counts the "" pairs inside.
    static const char* field(const char* p, const char* e, const char*& s, size_t& len, size_t& esc) {
        esc = 0;
        if (*p != '"') {
            s = p;
            while (*p != ',') p++;
            len = p - s;
            return p;
        }
        s = ++p;
        for (;;) {
            const char* q = (const char*)memchr(p, '"', e - p);
            if (q[1] != '"') { len = q - s; return q + 1; }
            esc++;
            p = q + 2;
        }
    }

    // Parses the record at p (a complete record ends before e) and returns where the next one starts.
    const char* record(const char* p, const char* e) {
        if ((uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;  // byte order mark
        uint64_t id = 0;
        while (*p != ',') id = id * 10 + (*p++ - '0');
        const char *ns, *ts;
        size_t nlen, nesc, tlen, tesc;
        p = field(p + 1, e, ns, nlen, nesc);
        p = field(p + 1, e, ts, tlen, tesc);
        uint64_t rid = 0;
        for (p++; *p != ','; p++) rid = rid * 10 + (*p - '0');
        bool neg = p[1] == '-';
        p += 1 + neg;
        int64_t cents = 0;
        while (*p != '.') cents = cents * 10 + (*p++ - '0');
        cents = cents * 100 + (p[1] - '0') * 10 + (p[2] - '0');
        p += 3;
        if (neg) cents = -cents;
        if (*p == '\r') p++;
        if (p < e && *p == '\n') p++;

        uint64_t nh;
        if (nesc == 0) {
            nh = hashBytes(ns, nlen);
        } else {  // unescape "" so that the same name hashes the same however it was written
            tmp.clear();
            for (size_t i = 0; i < nlen; i++) { tmp += ns[i]; if (ns[i] == '"') i++; }
            nh = hashBytes(tmp.data(), tmp.size());
        }
        size_t g = rid < region->size() && (*region)[rid] ? (*region)[rid] - 1 : groups - 1;
        Agg& a = agg[g];
        a.count++;
        a.idSum += id;
        a.sum += cents;
        a.lo = min(a.lo, cents);
        a.hi = max(a.hi, cents);
        a.nameBytes += nlen - nesc;
        a.noteBytes += tlen - tesc;
        names[g].push_back(nh);
        return p;
    }
};

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

    // regions.csv: region id -> country index.
    string rtext = slurp(dataDir + "/regions.csv");
    vector<uint32_t> rids;
    vector<uint16_t> rcountry;
    vector<string> countries;
    vector<int> codeIndex(26 * 26 * 26, -1);
    uint32_t maxId = 0;
    for (size_t pos = rtext.find('\n') + 1; pos < rtext.size();) {
        size_t nl = rtext.find('\n', pos);
        if (nl == string::npos) nl = rtext.size();
        const char* s = rtext.data() + pos;
        uint32_t id = 0;
        while (*s != ',') id = id * 10 + (*s++ - '0');
        s++;
        int& slot = codeIndex[(s[0] - 'A') * 676 + (s[1] - 'A') * 26 + (s[2] - 'A')];
        if (slot < 0) { slot = countries.size(); countries.emplace_back(s, 3); }
        rids.push_back(id);
        rcountry.push_back(slot + 1);
        maxId = max(maxId, id);
        pos = nl + 1;
    }
    vector<uint16_t> region(maxId + 1, 0);
    for (size_t k = 0; k < rids.size(); k++) region[rids[k]] = rcountry[k];
    size_t groups = countries.size() + 1;

    int fd = open((dataDir + "/records.csv").c_str(), O_RDONLY);
    if (fd < 0) { fprintf(stderr, "Cannot open records.csv\n"); return 1; }
    off_t size = lseek(fd, 0, SEEK_END);
    cpu_set_t cpus;
    sched_getaffinity(0, sizeof cpus, &cpus);
    int n = clamp(CPU_COUNT(&cpus), 1, 16);
    n = (int)min<off_t>(n, max<off_t>(1, size >> 20));

    // A record starts after a newline that is followed (after an optional BOM) by a digit:
    // quoted text never has digits, so no newline inside a quoted field qualifies.
    auto boundary = [&](off_t pos) -> off_t {
        char buf[4096], nx[4];
        for (ssize_t got; pos < size && (got = pread(fd, buf, sizeof buf, pos)) > 0; pos += got)
            for (ssize_t k = 0; k < got; k++) {
                if (buf[k] != '\n') continue;
                off_t at = pos + k + 1;
                ssize_t m = pread(fd, nx, sizeof nx, at);
                int skip = m >= 3 && (uint8_t)nx[0] == 0xEF && (uint8_t)nx[1] == 0xBB && (uint8_t)nx[2] == 0xBF ? 3 : 0;
                if (skip < m && nx[skip] >= '0' && nx[skip] <= '9') return at;
            }
        return size;
    };
    // Thread i parses the records starting in [bounds[i], bounds[i + 1]); bounds[0] is just after the header.
    vector<off_t> bounds(n + 1, size);
    for (int i = 0; i < n; i++) {
        bounds[i] = boundary(i == 0 ? 0 : size * i / n - 1);
        if (i) bounds[i] = max(bounds[i], bounds[i - 1]);
    }

    vector<Worker> ws;
    ws.reserve(n);
    for (int i = 0; i < n; i++) ws.emplace_back(&region, groups);
    const size_t CH = 1 << 20, MARGIN = 1 << 16;  // a record is far shorter than MARGIN
    parallel(n, [&](int i) {
        Worker& w = ws[i];
        vector<char> buf(CH + 64);
        off_t pos = bounds[i], end = bounds[i + 1];
        size_t have = 0;
        for (;;) {
            size_t want = min<off_t>(CH - have, end - pos);
            ssize_t got = want ? pread(fd, buf.data() + have, want, pos) : 0;
            if (got < 0) break;
            pos += got;
            have += got;
            bool last = pos >= end || (want && got == 0);
            memset(buf.data() + have, 0, 8);
            const char *base = buf.data(), *e = base + have;
            const char* limit = last ? e : have > MARGIN ? e - MARGIN : base;
            const char* p = base;
            while (p < limit) p = w.record(p, e);
            have = e - p;
            memmove(buf.data(), p, have);
            if (last) break;
        }
    });
    close(fd);

    // Merge the threads' aggregates and count each group's distinct names.
    vector<Agg> total(groups);
    for (Worker& w : ws)
        for (size_t g = 0; g < groups; g++) {
            Agg &t = total[g], &a = w.agg[g];
            t.count += a.count;
            t.idSum += a.idSum;
            t.sum += a.sum;
            t.lo = min(t.lo, a.lo);
            t.hi = max(t.hi, a.hi);
            t.nameBytes += a.nameBytes;
            t.noteBytes += a.noteBytes;
        }
    vector<uint64_t> distinct(groups);
    parallel(n, [&](int i) {
        for (size_t g = i; g < groups; g += n) {
            vector<uint64_t> all;
            for (Worker& w : ws) all.insert(all.end(), w.names[g].begin(), w.names[g].end());
            sort(all.begin(), all.end());
            distinct[g] = unique(all.begin(), all.end()) - all.begin();
        }
    });

    auto key = [&](size_t g) { return g + 1 < groups ? countries[g] : string("_UNMATCHED"); };
    vector<size_t> order;
    for (size_t g = 0; g < groups; g++)
        if (total[g].count) order.push_back(g);
    sort(order.begin(), order.end(), [&](size_t a, size_t b) { return key(a) < key(b); });

    string out;
    auto money = [&](int64_t v) {
        if (v < 0) { out += '-'; v = -v; }
        out += to_string(v / 100);
        out += '.';
        out += char('0' + v / 10 % 10);
        out += char('0' + v % 10);
    };
    for (size_t g : order) {
        const Agg& a = total[g];
        out += key(g) + ' ' + to_string(a.count) + ' ' + to_string(a.idSum) + ' ';
        money(a.sum); out += ' ';
        money(a.lo); out += ' ';
        money(a.hi);
        out += ' ' + to_string(distinct[g]) + ' ' + to_string(a.nameBytes) + ' ' + to_string(a.noteBytes) + '\n';
    }

    filesystem::create_directories(outDir);
    FILE* f = fopen((outDir + "/aggregates.txt").c_str(), "wb");
    if (!f) { fprintf(stderr, "Cannot create aggregates.txt\n"); return 1; }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return 0;
}