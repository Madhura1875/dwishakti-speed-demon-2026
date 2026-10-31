#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace std;

static uint8_t cls[256];  // lowercase letter or digit for token bytes, 0 for separators

struct Query { bool phrase = false; vector<uint32_t> terms; string ops; };  // ops[k - 1] joins terms[k - 1] and terms[k]

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

// True if the token s[0, len), lowercased, equals term.
static bool same(const char* s, size_t len, const string& term) {
    if (len != term.size()) return false;
    for (size_t i = 0; i < len; i++)
        if (cls[(uint8_t)s[i]] != (uint8_t)term[i]) return false;
    return true;
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
    for (int c = 0; c < 256; c++)
        cls[c] = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ? c : (c >= 'A' && c <= 'Z') ? c + 32 : 0;

    // Queries: every distinct term gets an id, and only those terms are indexed.
    string qtext = slurp(dataDir + "/queries.txt");
    vector<Query> queries;
    vector<string> terms;
    unordered_map<string, uint32_t> termId;
    auto intern = [&](string_view w) {
        auto [it, fresh] = termId.try_emplace(string(w), (uint32_t)terms.size());
        if (fresh) terms.emplace_back(w);
        return it->second;
    };
    for (size_t pos = 0; pos < qtext.size();) {
        size_t nl = qtext.find('\n', pos);
        if (nl == string::npos) nl = qtext.size();
        string_view line(qtext.data() + pos, nl - pos), rest;
        pos = nl + 1;
        size_t sp = line.find(' ');
        Query q;
        q.phrase = line.substr(0, sp) == "PHRASE";
        if (sp != string_view::npos) rest = line.substr(sp + 1);
        if (q.phrase && rest.size() >= 2) rest = rest.substr(1, rest.size() - 2);  // strip the quotes
        for (size_t k = 0, at = 0; at <= rest.size(); k++) {
            size_t e = rest.find(' ', at);
            if (e == string_view::npos) e = rest.size();
            string_view w = rest.substr(at, e - at);
            if (q.phrase || k % 2 == 0) q.terms.push_back(intern(w));
            else q.ops += w.empty() ? 'A' : w[0];  // A(ND), O(R) or N(OT)
            at = e + 1;
        }
        queries.push_back(move(q));
    }

    // Open-addressing table: (hash high 32 bits << 32) | (term id + 1).
    size_t T = terms.size();
    uint32_t mask = 1;
    while (mask < 4 * T + 16) mask <<= 1;
    vector<uint64_t> slots(mask);
    mask--;
    for (uint32_t id = 0; id < T; id++) {
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : terms[id]) h = (h ^ c) * 1099511628211ull;
        uint32_t s = (uint32_t)h & mask;
        while (slots[s]) s = (s + 1) & mask;
        slots[s] = (h >> 32 << 32) | (id + 1);
    }

    int fd = open((dataDir + "/docs.txt").c_str(), O_RDONLY);
    if (fd < 0) { fprintf(stderr, "Cannot open docs.txt\n"); return 1; }
    off_t size = lseek(fd, 0, SEEK_END);
    const char* base = size ? (const char*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0) : "";
    if (base == MAP_FAILED) { fprintf(stderr, "mmap failed\n"); return 1; }
    close(fd);
    const char* fileEnd = base + size;

    cpu_set_t cpus;
    sched_getaffinity(0, sizeof cpus, &cpus);
    int n = clamp(CPU_COUNT(&cpus), 1, 16);

    // Slice i is the lines starting in [cut[i], cut[i + 1]).
    vector<off_t> cut(n + 1, size);
    cut[0] = 0;
    for (int i = 1; i < n; i++) {
        off_t lo = size * i / n;
        if (lo == 0) continue;
        const char* nl = (const char*)memchr(base + lo - 1, '\n', size - (lo - 1));
        if (nl) cut[i] = nl + 1 - base;
    }

    // Each thread scans its slice: per query term, the ascending ids of the docs containing it.
    struct Slice { vector<vector<uint32_t>> post; vector<uint64_t> start; };  // start: offset of each doc's text
    vector<Slice> slices(n);
    parallel(n, [&](int i) {
        Slice& s = slices[i];
        s.post.resize(T);
        for (const char* p = base + cut[i]; p < base + cut[i + 1];) {
            const char* nl = (const char*)memchr(p, '\n', fileEnd - p);
            const char* le = nl ? nl : fileEnd;
            const char* tab = (const char*)memchr(p, '\t', le - p);
            uint32_t id = 0;
            for (const char* q = p; q < le && *q >= '0' && *q <= '9'; q++) id = id * 10 + (*q - '0');
            const char* t = tab ? tab + 1 : le;
            s.start.push_back(t - base);
            while (t < le) {
                uint8_t c = cls[(uint8_t)*t];
                if (!c) { t++; continue; }
                const char* b = t;
                uint64_t h = 1469598103934665603ull;
                do {
                    h = (h ^ c) * 1099511628211ull;
                    c = ++t < le ? cls[(uint8_t)*t] : 0;
                } while (c);
                for (uint32_t sl = (uint32_t)h & mask; slots[sl]; sl = (sl + 1) & mask) {
                    if ((uint32_t)(slots[sl] >> 32) != (uint32_t)(h >> 32)) continue;
                    uint32_t term = (uint32_t)slots[sl] - 1;
                    if (!same(b, t - b, terms[term])) continue;
                    vector<uint32_t>& v = s.post[term];
                    if (v.empty() || v.back() != id) v.push_back(id);
                    break;
                }
            }
            p = le + 1;
        }
    });
    vector<uint64_t> off;  // off[id]: where doc id's text starts (ids are 0..N-1 in file order)
    for (Slice& s : slices) off.insert(off.end(), s.start.begin(), s.start.end());
    size_t W = (off.size() + 63) / 64;

    // Answer the queries with bitsets over the doc ids.
    const uint64_t M[6] = {0xAAAAAAAAAAAAAAAAull, 0xCCCCCCCCCCCCCCCCull, 0xF0F0F0F0F0F0F0F0ull,
                           0xFF00FF00FF00FF00ull, 0xFFFF0000FFFF0000ull, 0xFFFFFFFF00000000ull};
    vector<string> outs(queries.size());
    parallel(n, [&](int i) {
        vector<uint64_t> acc(W), tmp(W);
        vector<pair<const char*, uint32_t>> toks;
        auto each = [&](uint32_t t, auto&& f) {
            for (const Slice& s : slices)
                for (uint32_t d : s.post[t]) f(d);
        };
        // Does doc d contain the terms ts as consecutive tokens?
        auto matches = [&](uint32_t d, const vector<uint32_t>& ts) {
            const char* t = base + off[d];
            const char* e = (const char*)memchr(t, '\n', fileEnd - t);
            if (!e) e = fileEnd;
            toks.clear();
            while (t < e) {
                if (!cls[(uint8_t)*t]) { t++; continue; }
                const char* b = t;
                while (t < e && cls[(uint8_t)*t]) t++;
                toks.push_back({b, (uint32_t)(t - b)});
            }
            for (size_t a = 0; a + ts.size() <= toks.size(); a++) {
                size_t j = 0;
                while (j < ts.size() && same(toks[a + j].first, toks[a + j].second, terms[ts[j]])) j++;
                if (j == ts.size()) return true;
            }
            return false;
        };

        for (size_t qi = i; qi < queries.size(); qi += n) {
            const Query& q = queries[qi];
            fill(acc.begin(), acc.end(), 0);
            each(q.terms[0], [&](uint32_t d) { acc[d >> 6] |= 1ull << (d & 63); });
            for (size_t k = 1; k < q.terms.size(); k++) {
                char op = q.phrase || k - 1 >= q.ops.size() ? 'A' : q.ops[k - 1];
                if (op == 'O') {
                    each(q.terms[k], [&](uint32_t d) { acc[d >> 6] |= 1ull << (d & 63); });
                } else if (op == 'N') {
                    each(q.terms[k], [&](uint32_t d) { acc[d >> 6] &= ~(1ull << (d & 63)); });
                } else {
                    fill(tmp.begin(), tmp.end(), 0);
                    each(q.terms[k], [&](uint32_t d) { tmp[d >> 6] |= 1ull << (d & 63); });
                    for (size_t w = 0; w < W; w++) acc[w] &= tmp[w];
                }
            }

            // Count, sum of ids and the 10 smallest ids of the result.
            uint64_t cnt = 0, sum = 0;
            uint32_t first[10], nf = 0;
            for (size_t w = 0; w < W; w++) {
                uint64_t x = acc[w];
                if (!x) continue;
                if (q.phrase) {
                    for (; x; x &= x - 1) {
                        uint32_t d = 64 * w + countr_zero(x);
                        if (!matches(d, q.terms)) continue;
                        cnt++;
                        sum += d;
                        if (nf < 10) first[nf++] = d;
                    }
                } else {
                    uint64_t c = popcount(x), s = 64 * w * c;
                    for (int j = 0; j < 6; j++) s += (uint64_t)popcount(x & M[j]) << j;
                    cnt += c;
                    sum += s;
                    for (; nf < 10 && x; x &= x - 1) first[nf++] = 64 * w + countr_zero(x);
                }
            }
            string& s = outs[qi];
            auto put = [&](uint64_t v) { char b[24]; s.append(b, to_chars(b, b + 24, v).ptr); };
            put(cnt);
            s += ' ';
            put(sum);
            for (uint32_t k = 0; k < nf; k++) { s += ' '; put(first[k]); }
            s += '\n';
        }
    });

    filesystem::create_directories(outDir);
    FILE* f = fopen((outDir + "/results.txt").c_str(), "wb");
    if (!f) { fprintf(stderr, "Cannot create results.txt\n"); return 1; }
    for (const string& s : outs) fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    return 0;
}