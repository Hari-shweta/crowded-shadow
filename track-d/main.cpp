#include <iostream>
#include <vector>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <thread>
#include <atomic>
#include <algorithm>

struct Edge {
    uint32_t to;
    uint32_t w;
};

struct HeapNode {
    uint64_t dist;
    uint32_t u;
};

// High-performance 4-ary (d-ary with d=4) min-heap with hole-moving optimization
class QuadHeap {
public:
    std::vector<HeapNode> data;

    QuadHeap() {
        data.reserve(16384);
    }

    void clear() {
        data.clear();
    }

    bool empty() const {
        return data.empty();
    }

    void push(uint64_t dist, uint32_t u) {
        size_t idx = data.size();
        data.push_back({dist, u});
        // Sift up: tree depth is cut in half compared to binary heap
        while (idx > 0) {
            size_t p = (idx - 1) >> 2;
            if (dist < data[p].dist) {
                data[idx] = data[p];
                idx = p;
            } else {
                break;
            }
        }
        data[idx] = {dist, u};
    }

    HeapNode pop() {
        HeapNode ret = data[0];
        HeapNode val = data.back();
        data.pop_back();
        if (!data.empty()) {
            size_t idx = 0;
            size_t n = data.size();
            while (true) {
                size_t child = (idx << 2) + 1;
                if (child >= n) break;
                size_t min_child = child;
                uint64_t min_d = data[child].dist;
                size_t end_child = std::min(child + 4, n);
                for (size_t c = child + 1; c < end_child; ++c) {
                    if (data[c].dist < min_d) {
                        min_d = data[c].dist;
                        min_child = c;
                    }
                }
                if (min_d < val.dist) {
                    data[idx] = data[min_child];
                    idx = min_child;
                } else {
                    break;
                }
            }
            data[idx] = val;
        }
        return ret;
    }
};

// Disjoint Set Union (DSU) to instantly detect disconnected components (e.g. islands, isolated nodes)
struct DSU {
    std::vector<uint32_t> parent;
    DSU(uint32_t n) : parent(n) {
        for (uint32_t i = 0; i < n; ++i) parent[i] = i;
    }
    uint32_t find(uint32_t i) {
        uint32_t root = i;
        while (root != parent[root]) root = parent[root];
        uint32_t curr = i;
        while (curr != root) {
            uint32_t nxt = parent[curr];
            parent[curr] = root;
            curr = nxt;
        }
        return root;
    }
    void unite(uint32_t i, uint32_t j) {
        uint32_t ri = find(i);
        uint32_t rj = find(j);
        if (ri != rj) parent[ri] = rj;
    }
};

static inline uint32_t parse_u32(const char*& p) {
    while (*p <= ' ') ++p;
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p++ - '0');
    }
    return v;
}

int main(int argc, char** argv) {
    std::string data_dir = "/data";
    std::string out_path = "/out/distances.txt";
    if (argc >= 2) data_dir = argv[1];
    if (argc >= 3) {
        std::string arg2 = argv[2];
        if (arg2.find(".txt") != std::string::npos) {
            out_path = arg2;
        } else {
            out_path = arg2 + "/distances.txt";
        }
    }

    std::string graph_path = data_dir + "/graph.txt";
    std::string queries_path = data_dir + "/queries.txt";

    // 1. Memory-map graph.txt
    int fd = open(graph_path.c_str(), O_RDONLY);
    if (fd < 0) {
        perror("Failed to open graph.txt");
        return 1;
    }
    struct stat st;
    fstat(fd, &st);
    size_t fsize = st.st_size;
    const char* data = (const char*)mmap(nullptr, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    const char* p = data;
    uint32_t n = parse_u32(p);
    uint32_t m = parse_u32(p);
    const char* edges_start = p;

    // Pass 1: Degree counts, in-degree counts, and component DSU.
    // Zero allocation for raw edges saves tens of megabytes of heap!
    DSU dsu(n);
    std::vector<uint32_t> degree(n, 0);
    std::vector<uint32_t> in_deg(n, 0);

    for (uint32_t i = 0; i < m; ++i) {
        uint32_t u = parse_u32(p);
        uint32_t v = parse_u32(p);
        uint32_t w = parse_u32(p);
        if (u != v) {
            degree[u]++;
            in_deg[v]++;
            dsu.unite(u, v);
        }
    }

    // Flatten DSU for O(1) component ID comparison
    std::vector<uint32_t> comp(n);
    for (uint32_t i = 0; i < n; ++i) {
        comp[i] = dsu.find(i);
    }

    // Build CSR index
    std::vector<uint32_t> head(n + 1, 0);
    for (uint32_t u = 0; u < n; ++u) {
        head[u + 1] = head[u] + degree[u];
    }
    std::vector<uint32_t> cur = head;
    std::vector<Edge> edges(head[n]);

    // Pass 2: Populate contiguous CSR edges array directly from mapped file
    p = edges_start;
    for (uint32_t i = 0; i < m; ++i) {
        uint32_t u = parse_u32(p);
        uint32_t v = parse_u32(p);
        uint32_t w = parse_u32(p);
        if (u != v) {
            edges[cur[u]++] = {v, w};
        }
    }
    munmap((void*)data, fsize);

    // 2. Memory-map queries.txt
    fd = open(queries_path.c_str(), O_RDONLY);
    if (fd < 0) {
        perror("Failed to open queries.txt");
        return 1;
    }
    fstat(fd, &st);
    fsize = st.st_size;
    data = (const char*)mmap(nullptr, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    p = data;
    const char* end = data + fsize;

    struct Query {
        uint32_t s, t;
    };
    std::vector<Query> queries;
    queries.reserve(2048);
    while (p < end) {
        while (p < end && *p <= ' ') ++p;
        if (p >= end) break;
        uint32_t s = parse_u32(p);
        uint32_t t = parse_u32(p);
        queries.push_back({s, t});
    }
    munmap((void*)data, fsize);

    // 3. Multi-threaded Query Solving
    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    std::vector<int64_t> answers(queries.size(), -1);
    std::atomic<size_t> query_idx{0};

    auto worker = [&]() {
        std::vector<uint64_t> dist(n, UINT64_MAX);
        std::vector<uint32_t> touched;
        touched.reserve(16384);
        QuadHeap qh;

        while (true) {
            size_t qi = query_idx.fetch_add(1, std::memory_order_relaxed);
            if (qi >= queries.size()) break;

            uint32_t s = queries[qi].s;
            uint32_t t = queries[qi].t;

            // O(1) Fast paths
            if (s == t) {
                answers[qi] = 0;
                continue;
            }
            if (degree[s] == 0 || in_deg[t] == 0 || comp[s] != comp[t]) {
                answers[qi] = -1;
                continue;
            }

            dist[s] = 0;
            touched.push_back(s);
            qh.push(0, s);

            int64_t ans = -1;
            while (!qh.empty()) {
                HeapNode top = qh.pop();
                uint64_t d = top.dist;
                uint32_t u = top.u;

                if (d > dist[u]) continue;
                if (u == t) {
                    ans = d;
                    break;
                }

                uint32_t start_idx = head[u];
                uint32_t end_idx = head[u + 1];
                for (uint32_t i = start_idx; i < end_idx; ++i) {
                    uint32_t v = edges[i].to;
                    uint64_t nd = d + edges[i].w;
                    if (dist[v] == UINT64_MAX) {
                        dist[v] = nd;
                        touched.push_back(v);
                        qh.push(nd, v);
                    } else if (nd < dist[v]) {
                        dist[v] = nd;
                        qh.push(nd, v);
                    }
                }
            }

            answers[qi] = ans;
            // Clean up only touched nodes (O(visited) reset, avoids clearing 14 MB per query!)
            for (uint32_t x : touched) {
                dist[x] = UINT64_MAX;
            }
            touched.clear();
            qh.clear();
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (unsigned int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker);
    }
    for (auto& th : threads) {
        th.join();
    }

    // 4. Fast output formatting
    std::string out_buf;
    out_buf.reserve(queries.size() * 24);
    char num_buf[32];
    for (int64_t a : answers) {
        if (a < 0) {
            out_buf.append("-1\n");
        } else if (a == 0) {
            out_buf.append("0\n");
        } else {
            uint64_t val = (uint64_t)a;
            int pos = 0;
            while (val > 0) {
                num_buf[pos++] = '0' + (val % 10);
                val /= 10;
            }
            for (int i = pos - 1; i >= 0; --i) {
                out_buf.push_back(num_buf[i]);
            }
            out_buf.push_back('\n');
        }
    }

    int out_fd = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd >= 0) {
        write(out_fd, out_buf.data(), out_buf.size());
        close(out_fd);
    } else {
        perror("Failed to write distances.txt");
        return 1;
    }

    return 0;
}

