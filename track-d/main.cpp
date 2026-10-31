#include <iostream>
#include <vector>
#include <queue>
#include <algorithm>
#include <cstdint>
#include <cstdio>

using namespace std;

const uint64_t INF = UINT64_MAX;

inline int parse_int(const char*& p) {
    int x = 0;
    while (*p >= '0' && *p <= '9') {
        x = x * 10 + (*p - '0');
        ++p;
    }
    ++p;
    return x;
}

inline uint32_t parse_u32(const char*& p) {
    uint32_t x = 0;
    while (*p >= '0' && *p <= '9') {
        x = x * 10 + (*p - '0');
        ++p;
    }
    ++p;
    return x;
}

struct RawEdge {
    int u, v;
    uint32_t w;
    bool operator<(const RawEdge& other) const {
        if (u != other.u) return u < other.u;
        if (v != other.v) return v < other.v;
        return w < other.w;
    }
};

struct CSRGraph {
    int n;
    vector<int> head;
    vector<int> edges_v;
    vector<uint32_t> edges_w;
};

int main() {
    ios_base::sync_with_stdio(false);
    cin.tie(NULL);

    FILE* f = fopen("/data/graph.txt", "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    vector<char> buffer(size + 1);
    fread(buffer.data(), 1, size, f);
    fclose(f);

    const char* p = buffer.data();
    int n = parse_int(p);
    int m = parse_u32(p);

    vector<RawEdge> raw_edges;
    raw_edges.reserve(m);
    for (int i = 0; i < m; ++i) {
        int u = parse_int(p);
        int v = parse_int(p);
        uint32_t w = parse_u32(p);
        if (u != v) raw_edges.push_back({u, v, w});
    }

    sort(raw_edges.begin(), raw_edges.end());
    vector<RawEdge> edges;
    edges.reserve(raw_edges.size());
    for (size_t i = 0; i < raw_edges.size(); ++i) {
        if (i + 1 < raw_edges.size() && raw_edges[i].u == raw_edges[i+1].u && raw_edges[i].v == raw_edges[i+1].v) {
            edges.push_back(raw_edges[i]);
            while (i + 1 < raw_edges.size() && raw_edges[i].u == raw_edges[i+1].u && raw_edges[i].v == raw_edges[i+1].v) i++;
        } else {
            edges.push_back(raw_edges[i]);
        }
    }

    int num_clean_edges = edges.size();
    vector<int> f_deg(n + 1, 0), b_deg(n + 1, 0);
    for (const auto& e : edges) {
        f_deg[e.u + 1]++;
        b_deg[e.v + 1]++;
    }

    for (int i = 0; i < n; ++i) {
        f_deg[i + 1] += f_deg[i];
        b_deg[i + 1] += b_deg[i];
    }

    CSRGraph fwd, rev;
    fwd.n = rev.n = n;
    fwd.head.resize(n + 1);
    rev.head.resize(n + 1);
    for (int i = 0; i <= n; ++i) {
        fwd.head[i] = f_deg[i];
        rev.head[i] = b_deg[i];
    }

    fwd.edges_v.resize(num_clean_edges);
    fwd.edges_w.resize(num_clean_edges);
    rev.edges_v.resize(num_clean_edges);
    rev.edges_w.resize(num_clean_edges);

    for (const auto& e : edges) {
        int idx1 = f_deg[e.u]++;
        fwd.edges_v[idx1] = e.v;
        fwd.edges_w[idx1] = e.w;

        int idx2 = b_deg[e.v]++;
        rev.edges_v[idx2] = e.u;
        rev.edges_w[idx2] = e.w;
    }

    FILE* qf = fopen("/data/queries.txt", "rb");
    if (!qf) return 1;
    fseek(qf, 0, SEEK_END);
    size_t q_size = ftell(qf);
    fseek(qf, 0, SEEK_SET);

    vector<char> q_buffer(q_size + 1);
    fread(q_buffer.data(), 1, q_size, qf);
    fclose(qf);

    const char* qp = q_buffer.data();
    vector<pair<int, int>> queries;
    while (*qp != '\0' && *qp != '\r' && *qp != '\n') {
        int s = parse_int(qp);
        int t = parse_int(qp);
        queries.push_back({s, t});
    }

    int num_queries = queries.size();
    vector<uint64_t> dist_f(n, INF);
    vector<uint64_t> dist_b(n, INF);
    vector<int> visited_f(n, 0);
    vector<int> visited_b(n, 0);
    int token = 0;

    priority_queue<pair<uint64_t, int>, vector<pair<uint64_t, int>>, greater<pair<uint64_t, int>>> pq_f, pq_b;
    vector<uint64_t> answers(num_queries);

    for (int q = 0; q < num_queries; ++q) {
        int s = queries[q].first;
        int t = queries[q].second;

        if (s == t) {
            answers[q] = 0;
            continue;
        }

        token++;
        while (!pq_f.empty()) pq_f.pop();
        while (!pq_b.empty()) pq_b.pop();

        dist_f[s] = 0;
        visited_f[s] = token;
        pq_f.push({0, s});

        dist_b[t] = 0;
        visited_b[t] = token;
        pq_b.push({0, t});

        uint64_t shortest_dist = INF;

        while (!pq_f.empty() || !pq_b.empty()) {
            if (!pq_f.empty()) {
                auto [d, u] = pq_f.top();
                pq_f.pop();

                if (d > dist_f[u]) continue;
                if (d >= shortest_dist) break;

                if (visited_b[u] == token) {
                    shortest_dist = min(shortest_dist, d + dist_b[u]);
                }

                int start = fwd.head[u];
                int end = fwd.head[u + 1];
                for (int i = start; i < end; ++i) {
                    int v = fwd.edges_v[i];
                    uint32_t w = fwd.edges_w[i];
                    uint64_t nd = d + w;

                    if (nd < shortest_dist) {
                        if (visited_f[v] != token || nd < dist_f[v]) {
                            dist_f[v] = nd;
                            visited_f[v] = token;
                            pq_f.push({nd, v});
                            if (visited_b[v] == token) {
                                shortest_dist = min(shortest_dist, nd + dist_b[v]);
                            }
                        }
                    }
                }
            }

            if (!pq_b.empty()) {
                auto [d, u] = pq_b.top();
                pq_b.pop();

                if (d > dist_b[u]) continue;
                if (d >= shortest_dist) break;

                if (visited_f[u] == token) {
                    shortest_dist = min(shortest_dist, d + dist_f[u]);
                }

                int start = rev.head[u];
                int end = rev.head[u + 1];
                for (int i = start; i < end; ++i) {
                    int v = rev.edges_v[i];
                    uint32_t w = rev.edges_w[i];
                    uint64_t nd = d + w;

                    if (nd < shortest_dist) {
                        if (visited_b[v] != token || nd < dist_b[v]) {
                            dist_b[v] = nd;
                            visited_b[v] = token;
                            pq_b.push({nd, v});
                            if (visited_f[v] == token) {
                                shortest_dist = min(shortest_dist, nd + dist_f[v]);
                            }
                        }
                    }
                }
            }
        }

        answers[q] = (shortest_dist == INF) ? -1 : shortest_dist;
    }

    FILE* out = fopen("/out/distances.txt", "wb");
    for (int q = 0; q < num_queries; ++q) {
        char buf[32];
        int len;
        if (answers[q] == (uint64_t)-1) {
            len = sprintf(buf, "-1\n");
        } else {
            len = sprintf(buf, "%llu\n", (unsigned long long)answers[q]);
        }
        fwrite(buf, 1, len, out);
    }
    fclose(out);

    return 0;
}