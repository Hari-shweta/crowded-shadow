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
            size_t idx = 0, n = data.size();
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
    unsigned int count, initial, gen{0};
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
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    return v;
}

int main(int argc, char** argv) {
    std::string data_dir = "/data", out_path = "/out/distances.txt";
    if (argc >= 2) data_dir = argv[1];
    if (argc >= 3) {
        std::string arg2 = argv[2];
        out_path = (arg2.find(".txt") != std::string::npos) ? arg2 : arg2 + "/distances.txt";
    }

    // 1. Parallel Graph Parsing & Bidirectional CSR Construction
    int fd = open((data_dir + "/graph.txt").c_str(), O_RDONLY);
    if (fd < 0) return 1;
    struct stat st; fstat(fd, &st);
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
    std::vector<const char*> starts(nthreads), ends(nthreads);
    for (unsigned int i = 0; i < nthreads; ++i) {
        size_t b_start = (i * edges_bytes) / nthreads;
        size_t b_end = ((i + 1) * edges_bytes) / nthreads;
        const char* c_start = edges_start + b_start;
        const char* c_end = edges_start + b_end;
        if (i > 0) while (c_start < data + fsize && *(c_start - 1) != '\n') c_start++;
        if (i < nthreads - 1) while (c_end < data + fsize && *(c_end - 1) != '\n') c_end++;
        else c_end = data + fsize;
        starts[i] = c_start; ends[i] = c_end;
    }

    std::vector<std::vector<uint32_t>> thread_fwd_deg(nthreads, std::vector<uint32_t>(n + 1, 0));
    std::vector<std::vector<uint32_t>> thread_rev_deg(nthreads, std::vector<uint32_t>(n + 1, 0));
    std::vector<uint32_t> fhead(n + 1, 0), rhead(n + 1, 0);
    std::vector<Edge> fedges, redges;

    // 2. Parse queries.txt
    fd = open((data_dir + "/queries.txt").c_str(), O_RDONLY);
    if (fd < 0) return 1;
    fstat(fd, &st);
    size_t q_fsize = st.st_size;
    const char* q_data = (const char*)mmap(nullptr, q_fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    const char* q_p = q_data, *q_end = q_data + q_fsize;
    struct Query { uint32_t s, t; };
    std::vector<Query> queries; queries.reserve(2048);
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
            // Stage 1: Degree counting (skip weight w in pass 1)
            const char* ptr = starts[tid], *end = ends[tid];
            auto& fdeg = thread_fwd_deg[tid];
            auto& rdeg = thread_rev_deg[tid];
            while (ptr < end) {
                while (ptr < end && *ptr <= ' ') ++ptr;
                if (ptr >= end) break;
                uint32_t u = fast_parse_u32(ptr);
                uint32_t v = fast_parse_u32(ptr);
                if (u != v) { fdeg[u + 1]++; rdeg[v + 1]++; }
                while (ptr < end && *ptr != '\n') ++ptr;
                if (ptr < end) ptr++;
            }

            barrier.arrive_and_wait();

            if (tid == 0) {
                for (uint32_t u = 0; u < n; ++u) {
                    uint32_t fcnt = 0, rcnt = 0;
                    for (unsigned int t = 0; t < nthreads; ++t) {
                        fcnt += thread_fwd_deg[t][u + 1];
                        rcnt += thread_rev_deg[t][u + 1];
                    }
                    fhead[u + 1] = fhead[u] + fcnt;
                    rhead[u + 1] = rhead[u] + rcnt;
                }
                fedges.resize(fhead[n]);
                redges.resize(rhead[n]);
                for (uint32_t u = 0; u < n; ++u) {
                    uint32_t fpos = fhead[u], rpos = rhead[u];
                    for (unsigned int t = 0; t < nthreads; ++t) {
                        uint32_t fcnt = thread_fwd_deg[t][u + 1];
                        uint32_t rcnt = thread_rev_deg[t][u + 1];
                        thread_fwd_deg[t][u] = fpos;
                        thread_rev_deg[t][u] = rpos;
                        fpos += fcnt; rpos += rcnt;
                    }
                }
            }

            barrier.arrive_and_wait();

            // Stage 2: Populate fedges and redges
            ptr = starts[tid];
            auto& fcur = thread_fwd_deg[tid];
            auto& rcur = thread_rev_deg[tid];
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

            madvise((void*)starts[tid], ends[tid] - starts[tid], MADV_DONTNEED);
            barrier.arrive_and_wait();

            if (tid == 0) {
                munmap((void*)data, fsize);
                std::vector<std::vector<uint32_t>>().swap(thread_fwd_deg);
                std::vector<std::vector<uint32_t>>().swap(thread_rev_deg);
            }

            barrier.arrive_and_wait();

            // Stage 3: Bidirectional Dijkstra with 4-ary Min-Heaps
            std::vector<uint64_t> f_dist(n, UINT64_MAX), r_dist(n, UINT64_MAX);
            std::vector<uint32_t> f_touched, r_touched;
            f_touched.reserve(16384); r_touched.reserve(16384);
            QuadHeap f_qh, r_qh;

            while (true) {
                size_t qi = query_idx.fetch_add(1, std::memory_order_relaxed);
                if (qi >= queries.size()) break;
                uint32_t s = queries[qi].s, tgt = queries[qi].t;
                if (s == tgt) { answers[qi] = 0; continue; }
                if (fhead[s + 1] == fhead[s] || rhead[tgt + 1] == rhead[tgt]) {
                    answers[qi] = -1; continue;
                }

                f_dist[s] = 0; f_touched.push_back(s); f_qh.push(0, s);
                r_dist[tgt] = 0; r_touched.push_back(tgt); r_qh.push(0, tgt);
                uint64_t best_dist = UINT64_MAX;

                while (!f_qh.empty() && !r_qh.empty()) {
                    uint64_t f_min = f_qh.data[0].dist;
                    uint64_t r_min = r_qh.data[0].dist;
                    if (f_min + r_min >= best_dist) break;

                    if (f_qh.data.size() <= r_qh.data.size()) {
                        HeapNode top = f_qh.pop();
                        uint64_t d = top.dist; uint32_t u = top.u;
                        if (d > f_dist[u]) continue;
                        uint32_t s_idx = fhead[u], e_idx = fhead[u + 1];
                        for (uint32_t i = s_idx; i < e_idx; ++i) {
                            uint32_t v = fedges[i].to; uint64_t nd = d + fedges[i].w;
                            if (f_dist[v] == UINT64_MAX) {
                                f_dist[v] = nd; f_touched.push_back(v); f_qh.push(nd, v);
                            } else if (nd < f_dist[v]) {
                                f_dist[v] = nd; f_qh.push(nd, v);
                            }
                            if (r_dist[v] != UINT64_MAX && nd + r_dist[v] < best_dist) {
                                best_dist = nd + r_dist[v];
                            }
                        }
                    } else {
                        HeapNode top = r_qh.pop();
                        uint64_t d = top.dist; uint32_t u = top.u;
                        if (d > r_dist[u]) continue;
                        uint32_t s_idx = rhead[u], e_idx = rhead[u + 1];
                        for (uint32_t i = s_idx; i < e_idx; ++i) {
                            uint32_t v = redges[i].to; uint64_t nd = d + redges[i].w;
                            if (r_dist[v] == UINT64_MAX) {
                                r_dist[v] = nd; r_touched.push_back(v); r_qh.push(nd, v);
                            } else if (nd < r_dist[v]) {
                                r_dist[v] = nd; r_qh.push(nd, v);
                            }
                            if (f_dist[v] != UINT64_MAX && nd + f_dist[v] < best_dist) {
                                best_dist = nd + f_dist[v];
                            }
                        }
                    }
                }
                answers[qi] = (best_dist == UINT64_MAX) ? -1 : (int64_t)best_dist;
                for (uint32_t x : f_touched) f_dist[x] = UINT64_MAX;
                for (uint32_t x : r_touched) r_dist[x] = UINT64_MAX;
                f_touched.clear(); r_touched.clear(); f_qh.clear(); r_qh.clear();
            }
        });
    }
    for (auto& w : workers) w.join();

    // 4. Output Writing via Batched Buffered I/O
    int out_fd = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd < 0) out_fd = open("distances.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd >= 0) {
        std::vector<char> io_buf(1024 * 1024); size_t io_pos = 0;
        char num_buf[32];
        for (int64_t a : answers) {
            int len = 0;
            if (a < 0) { num_buf[len++] = '-'; num_buf[len++] = '1'; num_buf[len++] = '\n'; }
            else if (a == 0) { num_buf[len++] = '0'; num_buf[len++] = '\n'; }
            else {
                uint64_t val = (uint64_t)a; int pos = 0; char tmp[32];
                while (val > 0) { tmp[pos++] = '0' + (val % 10); val /= 10; }
                for (int i = pos - 1; i >= 0; --i) num_buf[len++] = tmp[i];
                num_buf[len++] = '\n';
            }
            if (io_pos + len > io_buf.size()) { write(out_fd, io_buf.data(), io_pos); io_pos = 0; }
            memcpy(io_buf.data() + io_pos, num_buf, len);
            io_pos += len;
        }
        if (io_pos > 0) write(out_fd, io_buf.data(), io_pos);
        close(out_fd);
    }
    return 0;
}