# Track D: Traverse

This solution implements the graph traversal and shortest-path task described in the [official specification](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/d-traverse.md).

## Approach

The solution is implemented in C++20 and uses bidirectional Dijkstra's algorithm to calculate shortest-path distances.

Key implementation techniques:

- **Bidirectional search:** Searches from both the source and destination to reduce the explored search space.
- **Radix heap:** Uses an integer-priority queue for efficient distance processing.
- **CSR graph representation:** Stores forward and reverse graph adjacency structures compactly.
- **Parallel processing:** Uses multiple worker threads to process graph construction and queries.
- **Reusable search structures:** Reuses per-worker search data structures across queries.

## Dependencies

- C++20 compiler
- Standard C++ library
- Linux environment

No external runtime libraries are required.

## Validation

The solution was built and executed using the official Docker checker.

Sample dataset validation:
- Docker image size: 118 MB
- Local execution time: 1.59 seconds, including container startup
- Output SHA-256: `a4c76ed030bc7e365db33191f6cf74708fba7f25eb48d64c8c4279f2e57046a0`
- Expected SHA-256 comparison: PASS

These are local sample-test measurements, not leaderboard results.

## With Another 24 Hours

Further improvements could include profiling graph construction, optimizing parallel query scheduling, reducing memory-access overhead, and benchmarking alternative priority-queue strategies.
