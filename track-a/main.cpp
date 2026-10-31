#include <iostream>
#include <vector>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <thread>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <cstdio>

#if defined(__x86_64__)
#include <nmmintrin.h>
static inline uint32_t hash_endpoint(const char* data, size_t len) {
    uint64_t h = len;
    while (len >= 8) {
        uint64_t v; std::memcpy(&v, data, 8);
        h = _mm_crc32_u64(h, v);
        data += 8; len -= 8;
    }
    if (len >= 4) {
        uint32_t v; std::memcpy(&v, data, 4);
        h = _mm_crc32_u32((uint32_t)h, v);
        data += 4; len -= 4;
    }
    while (len > 0) {
        h = _mm_crc32_u8((uint8_t)h, (uint8_t)*data++);
        len--;
    }
    return (uint32_t)h;
}
#elif defined(__aarch64__)
#include <arm_acle.h>
static inline uint32_t hash_endpoint(const char* data, size_t len) {
    uint64_t h = len;
    while (len >= 8) {
        uint64_t v; std::memcpy(&v, data, 8);
        h = __crc32cd(h, v);
        data += 8; len -= 8;
    }
    if (len >= 4) {
        uint32_t v; std::memcpy(&v, data, 4);
        h = __crc32cw((uint32_t)h, v);
        data += 4; len -= 4;
    }
    while (len > 0) {
        h = __crc32cb((uint8_t)h, (uint8_t)*data++);
        len--;
    }
    return (uint32_t)h;
}
#else
static inline uint32_t hash_endpoint(const char* data, size_t len) {
    uint32_t h = 2166136261U ^ static_cast<uint32_t>(len);
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint8_t>(data[i]);
        h *= 16777619U;
    }
    return h;
}
#endif

struct SimpleBarrier {
    std::atomic<int> count{0}, phase{0};
    int total{0};
    SimpleBarrier(int n) : total(n) {}
    void wait() {
        int p = phase.load(std::memory_order_relaxed);
        if (count.fetch_add(1, std::memory_order_acq_rel) + 1 == total) {
            count.store(0, std::memory_order_relaxed);
            phase.store(p + 1, std::memory_order_release);
        } else {
            while (phase.load(std::memory_order_acquire) == p) {
                #if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
                #elif defined(__aarch64__)
                asm volatile("yield");
                #endif
            }
        }
    }
};

static constexpr size_t MAX_ENDPOINTS = 2048;
static constexpr size_t TOTAL_GROUPS = 72 * MAX_ENDPOINTS;

struct StringView {
    const char* data;
    uint16_t len;
};

static constexpr size_t HASH_TABLE_CAPACITY = 8192;
struct EndpointEntry {
    StringView str;
    uint32_t hash = 0;
    uint16_t id = 0;
    std::atomic<bool> occupied{false};
};

class ConcurrentEndpointDictionary {
public:
    EndpointEntry table[HASH_TABLE_CAPACITY];
    std::vector<StringView> endpoints;
    std::mutex insert_mutex;

    ConcurrentEndpointDictionary() {
        endpoints.reserve(MAX_ENDPOINTS);
    }

    uint16_t get_or_insert(const char* data, uint16_t len) {
        uint32_t h = hash_endpoint(data, len);
        size_t mask = HASH_TABLE_CAPACITY - 1;
        size_t idx = h & mask;

        while (table[idx].occupied.load(std::memory_order_acquire)) {
            if (table[idx].hash == h && table[idx].str.len == len) {
                if (memcmp(table[idx].str.data, data, len) == 0) return table[idx].id;
            }
            idx = (idx + 1) & mask;
        }

        std::lock_guard<std::mutex> lock(insert_mutex);
        while (table[idx].occupied.load(std::memory_order_relaxed)) {
            if (table[idx].hash == h && table[idx].str.len == len) {
                if (memcmp(table[idx].str.data, data, len) == 0) return table[idx].id;
            }
            idx = (idx + 1) & mask;
        }

        uint16_t new_id = static_cast<uint16_t>(endpoints.size());
        endpoints.push_back({data, len});
        table[idx].str = {data, len};
        table[idx].hash = h;
        table[idx].id = new_id;
        table[idx].occupied.store(true, std::memory_order_release);
        return new_id;
    }
};

static const char* HOUR_STRINGS[72] = {
    "2026-10-30T18:00:00Z", "2026-10-30T19:00:00Z", "2026-10-30T20:00:00Z", "2026-10-30T21:00:00Z",
    "2026-10-30T22:00:00Z", "2026-10-30T23:00:00Z", "2026-10-31T00:00:00Z", "2026-10-31T01:00:00Z",
    "2026-10-31T02:00:00Z", "2026-10-31T03:00:00Z", "2026-10-31T04:00:00Z", "2026-10-31T05:00:00Z",
    "2026-10-31T06:00:00Z", "2026-10-31T07:00:00Z", "2026-10-31T08:00:00Z", "2026-10-31T09:00:00Z",
    "2026-10-31T10:00:00Z", "2026-10-31T11:00:00Z", "2026-10-31T12:00:00Z", "2026-10-31T13:00:00Z",
    "2026-10-31T14:00:00Z", "2026-10-31T15:00:00Z", "2026-10-31T16:00:00Z", "2026-10-31T17:00:00Z",
    "2026-10-31T18:00:00Z", "2026-10-31T19:00:00Z", "2026-10-31T20:00:00Z", "2026-10-31T21:00:00Z",
    "2026-10-31T22:00:00Z", "2026-10-31T23:00:00Z", "2026-11-01T00:00:00Z", "2026-11-01T01:00:00Z",
    "2026-11-01T02:00:00Z", "2026-11-01T03:00:00Z", "2026-11-01T04:00:00Z", "2026-11-01T05:00:00Z",
    "2026-11-01T06:00:00Z", "2026-11-01T07:00:00Z", "2026-11-01T08:00:00Z", "2026-11-01T09:00:00Z",
    "2026-11-01T10:00:00Z", "2026-11-01T11:00:00Z", "2026-11-01T12:00:00Z", "2026-11-01T13:00:00Z",
    "2026-11-01T14:00:00Z", "2026-11-01T15:00:00Z", "2026-11-01T16:00:00Z", "2026-11-01T17:00:00Z",
    "2026-11-01T18:00:00Z", "2026-11-01T19:00:00Z", "2026-11-01T20:00:00Z", "2026-11-01T21:00:00Z",
    "2026-11-01T22:00:00Z", "2026-11-01T23:00:00Z", "2026-11-02T00:00:00Z", "2026-11-02T01:00:00Z",
    "2026-11-02T02:00:00Z", "2026-11-02T03:00:00Z", "2026-11-02T04:00:00Z", "2026-11-02T05:00:00Z",
    "2026-11-02T06:00:00Z", "2026-11-02T07:00:00Z", "2026-11-02T08:00:00Z", "2026-11-02T09:00:00Z",
    "2026-11-02T10:00:00Z", "2026-11-02T11:00:00Z", "2026-11-02T12:00:00Z", "2026-11-02T13:00:00Z",
    "2026-11-02T14:00:00Z", "2026-11-02T15:00:00Z", "2026-11-02T16:00:00Z", "2026-11-02T17:00:00Z"
};

std::string resolve_output_path(const std::string& out_dir) {
    struct stat s;
    if (stat(out_dir.c_str(), &s) == 0 && S_ISDIR(s.st_mode)) return out_dir + "/summary.txt";
    return out_dir;
}

int main(int argc, char** argv) {
    std::string data_dir = (argc > 1) ? argv[1] : "/data";
    std::string output_path = resolve_output_path((argc > 2) ? argv[2] : "/out/summary.txt");

    int fd = open((data_dir + "/requests.log").c_str(), O_RDONLY);
    struct stat sb; fstat(fd, &sb);
    size_t file_size = sb.st_size;
    const char* data = (const char*)mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);

    unsigned int num_threads = std::min(8u, std::max(1u, std::thread::hardware_concurrency()));

    struct Chunk { const char* start; const char* end; };
    std::vector<Chunk> chunks(num_threads);
    for (unsigned int i = 0; i < num_threads; ++i) {
        size_t b_start = (i * file_size) / num_threads;
        size_t b_end = ((i + 1) * file_size) / num_threads;
        const char* c_start = data + b_start;
        const char* c_end = data + b_end;
        if (i > 0) while (c_start < data + file_size && *(c_start - 1) != '\n') c_start++;
        if (i < num_threads - 1) while (c_end < data + file_size && *(c_end - 1) != '\n') c_end++;
        else c_end = data + file_size;
        chunks[i] = {c_start, c_end};
    }

    ConcurrentEndpointDictionary dict;

    struct ThreadState {
        std::vector<uint32_t> counts;
        std::vector<uint64_t> records;
        std::vector<uint32_t> offsets;
    };
    std::vector<ThreadState> thread_states(num_threads);
    for (unsigned int t = 0; t < num_threads; ++t) {
        thread_states[t].counts.resize(TOTAL_GROUPS, 0);
        thread_states[t].offsets.resize(TOTAL_GROUPS + 1, 0);
        thread_states[t].records.reserve((chunks[t].end - chunks[t].start) / 40);
    }
    std::vector<std::atomic<uint32_t>> group_errors(TOTAL_GROUPS);
    for (size_t g = 0; g < TOTAL_GROUPS; ++g) group_errors[g].store(0, std::memory_order_relaxed);

    std::vector<uint32_t> group_counts(TOTAL_GROUPS, 0);
    std::vector<uint32_t> head(TOTAL_GROUPS + 1, 0);
    std::vector<uint32_t> all_latencies;
    std::vector<uint16_t> sorted_endpoint_ids;

    struct GroupPercentiles { uint32_t p50, p95, p99; };
    std::vector<GroupPercentiles> group_p(TOTAL_GROUPS);

    SimpleBarrier barrier(num_threads);
    std::vector<std::thread> workers; workers.reserve(num_threads);

    for (unsigned int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t]() {
            // PHASE 1: Single-pass file ingestion (Zero re-reading, zero Pass 2)
            const char* ptr = chunks[t].start;
            const char* end = chunks[t].end;
            auto& state = thread_states[t];
            auto& my_counts = state.counts;
            auto& my_records = state.records;

            while (ptr < end) {
                uint32_t day_offset = (ptr[6] - '0') + (ptr[9] - '0');
                uint32_t hour = (ptr[11] - '0') * 10 + (ptr[12] - '0');
                uint8_t hour_id = static_cast<uint8_t>(day_offset * 24 + hour - 18);
                ptr += 28;

                if (ptr[3] == ' ') ptr += 4;
                else if (ptr[4] == ' ') ptr += 5;
                else if (ptr[5] == ' ') ptr += 6;
                else if (ptr[6] == ' ') ptr += 7;
                else ptr += 8;

                const char* ep_start = ptr;
                while (*ptr != ' ') ptr++;
                uint16_t ep_len = static_cast<uint16_t>(ptr - ep_start);
                ptr++;

                uint16_t ep_id = dict.get_or_insert(ep_start, ep_len);
                bool is_error = (ptr[0] == '5');
                ptr += 4;

                uint32_t latency = 0;
                while (ptr < end && *ptr >= '0' && *ptr <= '9') {
                    latency = latency * 10 + (*ptr++ - '0');
                }
                if (ptr < end && *ptr == '\n') ptr++;

                size_t g_idx = static_cast<size_t>(hour_id) * MAX_ENDPOINTS + ep_id;
                my_counts[g_idx]++;
                if (is_error) group_errors[g_idx].fetch_add(1, std::memory_order_relaxed);

                my_records.push_back(((uint64_t)g_idx << 32) | latency);
            }

            #ifdef MADV_DONTNEED
            madvise((void*)chunks[t].start, chunks[t].end - chunks[t].start, MADV_DONTNEED);
            #endif

            std::sort(my_records.begin(), my_records.end());

            size_t r_size = my_records.size(), cur = 0;
            for (size_t g = 0; g < TOTAL_GROUPS; ++g) {
                state.offsets[g] = (uint32_t)cur;
                while (cur < r_size && (my_records[cur] >> 32) == g) cur++;
            }
            state.offsets[TOTAL_GROUPS] = (uint32_t)r_size;

            barrier.wait();

            // Thread 0 computes prefix sums and sorted endpoints
            if (t == 0) {
                for (size_t g = 0; g < TOTAL_GROUPS; ++g) {
                    uint32_t sum = 0;
                    for (unsigned int th = 0; th < num_threads; ++th) sum += thread_states[th].counts[g];
                    group_counts[g] = sum;
                    head[g + 1] = head[g] + sum;
                }
                all_latencies.resize(head[TOTAL_GROUPS]);

                uint16_t num_ep = static_cast<uint16_t>(dict.endpoints.size());
                sorted_endpoint_ids.resize(num_ep);
                for (uint16_t i = 0; i < num_ep; ++i) sorted_endpoint_ids[i] = i;
                std::sort(sorted_endpoint_ids.begin(), sorted_endpoint_ids.end(), [&](uint16_t a, uint16_t b) {
                    const auto& sa = dict.endpoints[a];
                    const auto& sb = dict.endpoints[b];
                    size_t min_l = std::min(sa.len, sb.len);
                    int cmp = memcmp(sa.data, sb.data, min_l);
                    if (cmp != 0) return cmp < 0;
                    return sa.len < sb.len;
                });
            }
            barrier.wait();

            // PHASE 2: Parallel scatter into all_latencies
            for (size_t g = t; g < TOTAL_GROUPS; g += num_threads) {
                if (group_counts[g] == 0) continue;
                uint32_t write_pos = head[g];
                for (unsigned int th = 0; th < num_threads; ++th) {
                    uint32_t s_idx = thread_states[th].offsets[g];
                    uint32_t e_idx = thread_states[th].offsets[g + 1];
                    for (uint32_t k = s_idx; k < e_idx; ++k) {
                        all_latencies[write_pos++] = (uint32_t)(thread_states[th].records[k] & 0xFFFFFFFFULL);
                    }
                }
            }
            barrier.wait();

            state.records.clear();
            state.records.shrink_to_fit();

            // PHASE 3: Parallel percentile calculation
            for (int hour_id = t; hour_id < 72; hour_id += num_threads) {
                for (uint16_t ep_id : sorted_endpoint_ids) {
                    size_t g_idx = static_cast<size_t>(hour_id) * MAX_ENDPOINTS + ep_id;
                    uint32_t cnt = group_counts[g_idx];
                    if (cnt == 0) continue;

                    uint32_t offset = head[g_idx];
                    uint32_t* begin = &all_latencies[offset];
                    uint32_t* end = begin + cnt;

                    if (cnt == 1) {
                        uint32_t v = *begin;
                        group_p[g_idx] = {v, v, v};
                    } else if (cnt <= 64) {
                        std::sort(begin, end);
                        size_t r50 = (50ULL * cnt + 99) / 100 - 1;
                        size_t r95 = (95ULL * cnt + 99) / 100 - 1;
                        size_t r99 = (99ULL * cnt + 99) / 100 - 1;
                        group_p[g_idx] = {begin[r50], begin[r95], begin[r99]};
                    } else {
                        size_t r50 = (50ULL * cnt + 99) / 100 - 1;
                        size_t r95 = (95ULL * cnt + 99) / 100 - 1;
                        size_t r99 = (99ULL * cnt + 99) / 100 - 1;

                        std::nth_element(begin, begin + r99, end);
                        uint32_t p99 = begin[r99];

                        std::nth_element(begin, begin + r95, begin + r99);
                        uint32_t p95 = begin[r95];

                        std::nth_element(begin, begin + r50, begin + r95);
                        uint32_t p50 = begin[r50];

                        group_p[g_idx] = {p50, p95, p99};
                    }
                }
            }
        });
    }
    for (auto& w : workers) w.join();

    // 4. Output write with 1 MB buffered stream
    FILE* out = fopen(output_path.c_str(), "w");
    if (out) {
        std::vector<char> write_buf(1024 * 1024);
        setvbuf(out, write_buf.data(), _IOFBF, write_buf.size());

        for (int hour_id = 0; hour_id < 72; ++hour_id) {
            const char* hour_str = HOUR_STRINGS[hour_id];
            for (uint16_t ep_id : sorted_endpoint_ids) {
                size_t g_idx = static_cast<size_t>(hour_id) * MAX_ENDPOINTS + ep_id;
                uint32_t cnt = group_counts[g_idx];
                if (cnt == 0) continue;

                const auto& ep = dict.endpoints[ep_id];
                uint32_t errs = group_errors[g_idx].load(std::memory_order_relaxed);
                uint64_t ppm = (static_cast<uint64_t>(errs) * 1000000ULL) / cnt;

                const auto& p = group_p[g_idx];
                std::fprintf(out, "%s %.*s %u %u %u %u %u %llu\n",
                             hour_str, (int)ep.len, ep.data,
                             cnt, p.p50, p.p95, p.p99, errs, (unsigned long long)ppm);
            }
        }
        fclose(out);
    }
    return 0;
}
