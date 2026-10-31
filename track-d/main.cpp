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

// 4-ary min-heap with hole-moving optimization
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

static inline uint32_t fast_parse_u32(const char*& p) {
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

    // 1. Parallel Graph Parsing & CSR Construction (Forward + Reverse)
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
    uint32_t n = fast_parse_u32(p);
    uint32_t m = fast_parse_u32(p);
    while (*p != '\n') p++;
    p++;
    const char* edges_start = p;
    size_t edges_bytes = (data + fsize) - edges_start;

    // Use 4 threads for build to minimize memory footprint
    unsigned int build_threads = 4;
    std::vector<const char*> starts(build_threads);
    std::vector<const char*> ends(build_threads);

    for (unsigned int i = 0; i < build_threads; ++i) {
        size_t b_start = (i * edges_bytes) / build_threads;
        size_t b_end = ((i + 1) * edges_bytes) / build_threads;
        const char* c_start = edges_start + b_start;
        const char* c_end = edges_start + b_end;
        if (i > 0) {
            while (c_start < data + fsize && *(c_start - 1) != '\n') c_start++;
        }
        if (i < build_threads - 1) {
            while (c_end < data + fsize && *(c_end - 1) != '\n') c_end++;
        } else {
            c_end = data + fsize;
        }
        starts[i] = c_start;
        ends[i] = c_end;
    }

    std::vector<std::vector<uint32_t>> thread_fwd_deg(build_threads, std::vector<uint32_t>(n + 1, 0));
    std::vector<std::vector<uint32_t>> thread_rev_deg(build_threads, std::vector<uint32_t>(n + 1, 0));

    std::vector<std::thread> workers;
    for (unsigned int t = 0; t < build_threads; ++t) {
        workers.emplace_back([&, t]() {
            const char* ptr = starts[t];
            const char* end = ends[t];
            auto& fdeg = thread_fwd_deg[t];
            auto& rdeg = thread_rev_deg[t];

            while (ptr < end) {
                while (ptr < end && *ptr <= ' ') ++ptr;
                if (ptr >= end) break;
                uint32_t u = fast_parse_u32(ptr);
                uint32_t v = fast_parse_u32(ptr);
                uint32_t w = fast_parse_u32(ptr);
                if (u != v) {
                    fdeg[u + 1]++;
                    rdeg[v + 1]++;
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    std::vector<uint32_t> fhead(n + 1, 0);
    std::vector<uint32_t> rhead(n + 1, 0);

    for (uint32_t u = 0; u < n; ++u) {
        uint32_t fd_cnt = 0;
        uint32_t rd_cnt = 0;
        for (unsigned int t = 0; t < build_threads; ++t) {
            fd_cnt += thread_fwd_deg[t][u + 1];
            rd_cnt += thread_rev_deg[t][u + 1];
        }
        fhead[u + 1] = fhead[u] + fd_cnt;
        rhead[u + 1] = rhead[u] + rd_cnt;
    }

    std::vector<Edge> fedges(fhead[n]);
    std::vector<Edge> redges(rhead[n]);

    for (uint32_t u = 0; u < n; ++u) {
        uint32_t fpos = fhead[u];
        uint32_t rpos = rhead[u];
        for (unsigned int t = 0; t < build_threads; ++t) {
            uint32_t fcnt = thread_fwd_deg[t][u + 1];
            uint32_t rcnt = thread_rev_deg[t][u + 1];
            thread_fwd_deg[t][u] = fpos;
            thread_rev_deg[t][u] = rpos;
            fpos += fcnt;
            rpos += rcnt;
        }
    }

    for (unsigned int t = 0; t < build_threads; ++t) {
        workers.emplace_back([&, t]() {
            const char* ptr = starts[t];
            const char* end = ends[t];
            auto& fcur = thread_fwd_deg[t];
            auto& rcur = thread_rev_deg[t];

            while (ptr < end) {
                while (ptr < end && *ptr <= ' ') ++ptr;
                if (ptr >= end) break;
                uint32_t u = fast_parse_u32(ptr);
                uint32_t v = fast_parse_u32(ptr);
                uint32_t w = fast_parse_u32(ptr);
                if (u != v) {
                    fedges[fcur[u]++] = {v, w};
                    redges[rcur[v]++] = {u, w};
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();
    std::vector<std::vector<uint32_t>>().swap(thread_fwd_deg);
    std::vector<std::vector<uint32_t>>().swap(thread_rev_deg);
    munmap((void*)data, fsize);

    // 2. Parse queries.txt
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

    struct Query { uint32_t s, t; };
    std::vector<Query> queries;
    queries.reserve(2048);
    while (p < end) {
        while (p < end && *p <= ' ') ++p;
        if (p >= end) break;
        uint32_t s = fast_parse_u32(p);
        uint32_t t = fast_parse_u32(p);
        queries.push_back({s, t});
    }
    munmap((void*)data, fsize);

    // 3. Multi-Threaded Bidirectional Dijkstra with L2 Bitset Pruning
    unsigned int query_threads = 6;
    std::vector<int64_t> answers(queries.size(), -1);
    std::atomic<size_t> query_idx{0};
    size_t words = (n + 63) / 64;

    for (unsigned int t = 0; t < query_threads; ++t) {
        workers.emplace_back([&]() {
            std::vector<uint64_t> f_dist(n, UINT64_MAX);
            std::vector<uint64_t> r_dist(n, UINT64_MAX);
            std::vector<uint64_t> f_seen(words, 0);
            std::vector<uint64_t> r_seen(words, 0);
            std::vector<uint32_t> f_touched; f_touched.reserve(16384);
            std::vector<uint32_t> r_touched; r_touched.reserve(16384);
            QuadHeap f_qh, r_qh;

            while (true) {
                size_t qi = query_idx.fetch_add(1, std::memory_order_relaxed);
                if (qi >= queries.size()) break;

                uint32_t s = queries[qi].s;
                uint32_t tgt = queries[qi].t;

                if (s == tgt) {
                    answers[qi] = 0;
                    continue;
                }
                if (fhead[s + 1] == fhead[s] || rhead[tgt + 1] == rhead[tgt]) {
                    answers[qi] = -1;
                    continue;
                }

                f_dist[s] = 0;
                f_seen[s >> 6] |= (1ULL << (s & 63));
                f_touched.push_back(s);
                f_qh.push(0, s);

                r_dist[tgt] = 0;
                r_seen[tgt >> 6] |= (1ULL << (tgt & 63));
                r_touched.push_back(tgt);
                r_qh.push(0, tgt);

                uint64_t best_dist = UINT64_MAX;

                while (!f_qh.empty() && !r_qh.empty()) {
                    uint64_t f_min = f_qh.data[0].dist;
                    uint64_t r_min = r_qh.data[0].dist;
                    if (f_min + r_min >= best_dist) break;

                    if (f_qh.data.size() <= r_qh.data.size()) {
                        // Forward expansion
                        HeapNode top = f_qh.pop();
                        uint64_t d = top.dist;
                        uint32_t u = top.u;
                        if (d > f_dist[u]) continue;

                        uint32_t start_idx = fhead[u];
                        uint32_t end_idx = fhead[u + 1];
                        for (uint32_t i = start_idx; i < end_idx; ++i) {
                            uint32_t v = fedges[i].to;
                            uint64_t nd = d + fedges[i].w;
                            if (f_dist[v] == UINT64_MAX) {
                                f_dist[v] = nd;
                                f_seen[v >> 6] |= (1ULL << (v & 63));
                                f_touched.push_back(v);
                                f_qh.push(nd, v);
                            } else if (nd < f_dist[v]) {
                                f_dist[v] = nd;
                                f_qh.push(nd, v);
                            }
                            if ((r_seen[v >> 6] & (1ULL << (v & 63))) != 0) {
                                if (nd + r_dist[v] < best_dist) {
                                    best_dist = nd + r_dist[v];
                                }
                            }
                        }
                    } else {
                        // Backward expansion
                        HeapNode top = r_qh.pop();
                        uint64_t d = top.dist;
                        uint32_t u = top.u;
                        if (d > r_dist[u]) continue;

                        uint32_t start_idx = rhead[u];
                        uint32_t end_idx = rhead[u + 1];
                        for (uint32_t i = start_idx; i < end_idx; ++i) {
                            uint32_t v = redges[i].to;
                            uint64_t nd = d + redges[i].w;
                            if (r_dist[v] == UINT64_MAX) {
                                r_dist[v] = nd;
                                r_seen[v >> 6] |= (1ULL << (v & 63));
                                r_touched.push_back(v);
                                r_qh.push(nd, v);
                            } else if (nd < r_dist[v]) {
                                r_dist[v] = nd;
                                r_qh.push(nd, v);
                            }
                            if ((f_seen[v >> 6] & (1ULL << (v & 63))) != 0) {
                                if (nd + f_dist[v] < best_dist) {
                                    best_dist = nd + f_dist[v];
                                }
                            }
                        }
                    }
                }

                answers[qi] = (best_dist == UINT64_MAX) ? -1 : (int64_t)best_dist;

                for (uint32_t x : f_touched) {
                    f_dist[x] = UINT64_MAX;
                    f_seen[x >> 6] &= ~(1ULL << (x & 63));
                }
                for (uint32_t x : r_touched) {
                    r_dist[x] = UINT64_MAX;
                    r_seen[x >> 6] &= ~(1ULL << (x & 63));
                }
                f_touched.clear();
                r_touched.clear();
                f_qh.clear();
                r_qh.clear();
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    // 4. Fast Output Writing
    int out_fd = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd >= 0) {
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
                for (int i = pos - 1; i >= 0; --i) out_buf.push_back(num_buf[i]);
                out_buf.push_back('\n');
            }
        }
        write(out_fd, out_buf.data(), out_buf.size());
        close(out_fd);
    } else {
        perror("Failed to write distances.txt");
        return 1;
    }

    return 0;
}
