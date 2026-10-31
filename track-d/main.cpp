#include <iostream>
#include <vector>
#include <queue>
#include <fstream>
#include <string>
#include <algorithm>
#include <cstdint>

using namespace std;

const uint64_t INF = UINT64_MAX;

struct RawEdge {
    int u;
    int v;
    uint32_t w;
};

int main() {
    ios_base::sync_with_stdio(false);
    cin.tie(NULL);

    // Read graph from /data/graph.txt
    ifstream graph_file("/data/graph.txt");
    if (!graph_file) {
        cerr << "Error opening graph.txt\n";
        return 1;
    }

    int n, m;
    graph_file >> n >> m;

    // We will build CSR (Compressed Sparse Row) representation for maximum cache locality
    vector<int> degrees(n + 1, 0);
    vector<RawEdge> temp_edges;
    temp_edges.reserve(m);

    for (int i = 0; i < m; ++i) {
        int u, v;
        uint32_t w;
        graph_file >> u >> v >> w;
        degrees[u + 1]++;
        temp_edges.push_back({u, v, w});
    }

    // Prefix sums for offsets
    for (int i = 0; i < n; ++i) {
        degrees[i + 1] += degrees[i];
    }

    vector<int> head(n + 1);
    for (int i = 0; i <= n; ++i) {
        head[i] = degrees[i];
    }

    vector<int> edges_v(m);
    vector<uint32_t> edges_w(m);

    // Populate adjacency lists
    for (const auto& edge : temp_edges) {
        int u = edge.u;
        int v = edge.v;
        uint32_t w = edge.w;
        int idx = head[u]++;
        edges_v[idx] = v;
        edges_w[idx] = w;
    }

    // Reset head array to start pointers
    for (int i = 0; i < n; ++i) {
        head[i] = degrees[i];
    }

    // Read queries from /data/queries.txt
    ifstream queries_file("/data/queries.txt");
    if (!queries_file) {
        cerr << "Error opening queries.txt\n";
        return 1;
    }

    vector<pair<int, int>> queries;
    int s_q, t_q;
    while (queries_file >> s_q >> t_q) {
        queries.push_back({s_q, t_q});
    }

    // Open output file
    ofstream out_file("/out/distances.txt");
    if (!out_file) {
        cerr << "Error opening output file\n";
        return 1;
    }

    // Reuse vectors for Dijkstra to avoid reallocation overhead across queries
    vector<uint64_t> dist(n, INF);
    vector<int> visited_token(n, 0);
    int token = 0;

    // Process each query
    for (size_t q_idx = 0; q_idx < queries.size(); ++q_idx) {
        int s = queries[q_idx].first;
        int t = queries[q_idx].second;

        if (s == t) {
            out_file << 0 << "\n";
            continue;
        }

        token++;
        priority_queue<pair<uint64_t, int>, vector<pair<uint64_t, int>>, greater<pair<uint64_t, int>>> pq;

        dist[s] = 0;
        visited_token[s] = token;
        pq.push({0, s});

        uint64_t shortest_dist = INF;

        while (!pq.empty()) {
            auto [d, u] = pq.top();
            pq.pop();

            if (d > dist[u]) continue;
            if (u == t) {
                shortest_dist = d;
                break;
            }

            int start = head[u];
            int end = head[u + 1];
            for (int i = start; i < end; ++i) {
                int v = edges_v[i];
                uint32_t w = edges_w[i];
                uint64_t nd = d + w;

                if (visited_token[v] != token || nd < dist[v]) {
                    dist[v] = nd;
                    visited_token[v] = token;
                    pq.push({nd, v});
                }
            }
        }

        if (shortest_dist == INF) {
            out_file << -1 << "\n";
        } else {
            out_file << shortest_dist << "\n";
        }
    }

    return 0;
}