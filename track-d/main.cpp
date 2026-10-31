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
#include <mutex>
#include <condition_variable>

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
    QuadHeap() { data.reserve(16384); }
    inline void clear() { data.clear(); }
    inline bool empty() const { return data.empty(); }
    inline void push(uint64_t dist, uint32_t u) {
        size_t idx = data.size();
        data.push_back({dist, u});
        while (idx > 0) {
            size_t p = (idx - 1) >> 2;
            if (dist < data[p].dist) {
                data[idx] = data[p];
                idx = p;
            } else break;
        }
        data[idx] = {dist, u};
    }
    inline HeapNode pop() {
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
                } else break;
            }
            data[idx] = val;
        }
        return ret;
    }
};

class SimpleBarrier {
private:
    std::mutex mtx;
    std::condition_variable cv;
    unsigned int count;
    unsigned int initial;
    unsigned int gen{0};
public:
    explicit SimpleBarrier(unsigned int num_threads) : count(num_threads), initial(num_threads) {}
    inline void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(mtx);
        unsigned int g = gen;
        if (--count == 0) {
            gen++;
            count = initial;
            cv.notify_all();
        } else {
            cv.wait(lock, [this, g] { return gen != g; });
        }
    }
    inline void wait() { arrive_and_wait(); }
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

    // 1. Parallel Graph Parsing & Forward CSR Construction
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

    unsigned int nthreads = std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
    std::vector<const char*> starts(nthreads);
    std::vector<const char*> ends(nthreads);
    for (unsigned int i = 0; i < nthreads; ++i) {
        size_t b_start = (i * edges_bytes) / nthreads;
        size_t b_end = ((i + 1) * edges_bytes) / nthreads;
        const char* c_start = edges_start + b_start;
        const char* c_end = edges_start + b_end;
        if (i > 0) while (c_start < data + fsize && *(c_start - 1) != '\n') c_start++;
        if (i < nthreads - 1) while (c_end < data + fsize && *(c_end - 1) != '\n') c_end++;
        else c_end = data + fsize;
        starts[i] = c_start;
        ends[i] = c_end;
    }

    std::vector<std::vector<uint32_t>> thread_deg(nthreads, std::vector<uint32_t>(n + 1, 0));
    std::vector<uint32_t> fhead(n + 1, 0);
    std::vector<Edge> fedges;

    // 2. Parse queries.txt
    fd = open(queries_path.c_str(), O_RDONLY);
    if (fd < 0) {
        perror("Failed to open queries.txt");
        return 1;
    }
    fstat(fd, &st);
    size_t q_fsize = st.st_size;
    const char* q_data = (const char*)mmap(nullptr, q_fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    const char* q_p = q_data;
    const char* q_end = q_data + q_fsize;
    struct Query { uint32_t s, t; };
    std::vector<Query> queries;
    queries.reserve(2048);
    while (q_p < q_end) {
        while (q_p < q_end && *q_p <= ' ') ++q_p;
        if (q_p >= q_end) break;
        uint32_t s = fast_parse_u32(q_p);
        uint32_t t = fast_parse_u32(q_p);
        queries.push_back({s, t});
    }
    munmap((void*)q_data, q_fsize);

    std::vector<int64_t> answers(queries.size(), -1);
    std::atomic<size_t> query_idx{0};
    SimpleBarrier barrier(nthreads);
    std::vector<std::thread> workers;

    for (unsigned int tid = 0; tid < nthreads; ++tid) {
        workers.emplace_back([&, tid]() {
            // Stage 1: Degree counting (skip weight w)
            const char* ptr = starts[tid];
            const char* end = ends[tid];
            auto& deg = thread_deg[tid];
            while (ptr < end) {
                while (ptr < end && *ptr <= ' ') ++ptr;
                if (ptr >= end) break;
                uint32_t u = fast_parse_u32(ptr);
                uint32_t v = fast_parse_u32(ptr);
                if (u != v) deg[u + 1]++;
                while (ptr < end && *ptr != '\n') ++ptr;
                if (ptr < end) ptr++;
            }

            barrier.arrive_and_wait();

            // Thread 0 computes fhead and assigns per-thread offsets
            if (tid == 0) {
                for (uint32_t u = 0; u < n; ++u) {
                    uint32_t cnt = 0;
                    for (unsigned int t = 0; t < nthreads; ++t) cnt += thread_deg[t][u + 1];
                    fhead[u + 1] = fhead[u] + cnt;
                }
                fedges.resize(fhead[n]);
                for (uint32_t u = 0; u < n; ++u) {
                    uint32_t pos = fhead[u];
                    for (unsigned int t = 0; t < nthreads; ++t) {
                        uint32_t cnt = thread_deg[t][u + 1];
                        thread_deg[t][u] = pos;
                        pos += cnt;
                    }
                }
            }

            barrier.arrive_and_wait();

            // Stage 2: Populate fedges
            ptr = starts[tid];
            auto& cur = thread_deg[tid];
            while (ptr < end) {
                while (ptr < end && *ptr <= ' ') ++ptr;
                if (ptr >= end) break;
                uint32_t u = fast_parse_u32(ptr);
                uint32_t v = fast_parse_u32(ptr);
                uint32_t w = fast_parse_u32(ptr);
                if (u != v) fedges[cur[u]++] = {v, w};
            }

            // Immediately evict parsed file pages from physical memory
            madvise((void*)starts[tid], ends[tid] - starts[tid], MADV_DONTNEED);

            barrier.arrive_and_wait();

            if (tid == 0) {
                munmap((void*)data, fsize);
                std::vector<std::vector<uint32_t>>().swap(thread_deg);
            }

            barrier.arrive_and_wait();

            // Stage 3: Dynamic work-stealing shortest-path queries
            std::vector<uint64_t> dist(n, UINT64_MAX);
            std::vector<uint32_t> touched;
            touched.reserve(16384);
            QuadHeap qh;

            while (true) {
                size_t qi = query_idx.fetch_add(1, std::memory_order_relaxed);
                if (qi >= queries.size()) break;
                uint32_t s = queries[qi].s;
                uint32_t tgt = queries[qi].t;
                if (s == tgt) { answers[qi] = 0; continue; }
                if (fhead[s + 1] == fhead[s]) { answers[qi] = -1; continue; }

                dist[s] = 0;
                touched.push_back(s);
                qh.push(0, s);
                uint64_t ans = UINT64_MAX;

                while (!qh.empty()) {
                    HeapNode top = qh.pop();
                    if (top.u == tgt) { ans = top.dist; break; }
                    if (top.dist > dist[top.u]) continue;

                    uint32_t st_idx = fhead[top.u];
                    uint32_t en_idx = fhead[top.u + 1];
                    for (uint32_t i = st_idx; i < en_idx; ++i) {
                        uint32_t v = fedges[i].to;
                        uint64_t nd = top.dist + fedges[i].w;
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
                answers[qi] = (ans == UINT64_MAX) ? -1 : (int64_t)ans;
                for (uint32_t x : touched) dist[x] = UINT64_MAX;
                touched.clear();
                qh.clear();
            }
        });
    }
    for (auto& w : workers) w.join();

    // 4. Output Writing
    FILE* f = fopen(out_path.c_str(), "w");
    if (!f) {
        f = fopen("distances.txt", "w");
    }
    if (f) {
        char buf[65536];
        setvbuf(f, buf, _IOFBF, sizeof(buf));
        char num_buf[32];
        for (int64_t a : answers) {
            if (a < 0) {
                fputs("-1\n", f);
            } else if (a == 0) {
                fputs("0\n", f);
            } else {
                uint64_t val = (uint64_t)a;
                int pos = 0;
                while (val > 0) {
                    num_buf[pos++] = '0' + (val % 10);
                    val /= 10;
                }
                for (int i = pos - 1; i >= 0; --i) fputc(num_buf[i], f);
                fputc('\n', f);
            }
        }
        fclose(f);
    } else {
        std::cerr << "Failed to open output file: " << strerror(errno) << std::endl;
        return 1;
    }

    return 0;
}
