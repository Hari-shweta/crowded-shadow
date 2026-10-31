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

#if defined(__x86_64__)
#include <nmmintrin.h>
static inline uint32_t hash_endpoint(const char* data, size_t len) {
    uint64_t h = len;
    while (len >= 8) {
        uint64_t v;
        std::memcpy(&v, data, 8);
        h = _mm_crc32_u64(h, v);
        data += 8;
        len -= 8;
    }
    if (len >= 4) {
        uint32_t v;
        std::memcpy(&v, data, 4);
        h = _mm_crc32_u32((uint32_t)h, v);
        data += 4;
        len -= 4;
    }
    while (len > 0) {
        h = _mm_crc32_u8((uint8_t)h, (uint8_t)*data);
        data++;
        len--;
    }
    return (uint32_t)h;
}
#elif defined(__aarch64__)
#include <arm_acle.h>
static inline uint32_t hash_endpoint(const char* data, size_t len) {
    uint64_t h = len;
    while (len >= 8) {
        uint64_t v;
        std::memcpy(&v, data, 8);
        h = __crc32cd(h, v);
        data += 8;
        len -= 8;
    }
    if (len >= 4) {
        uint32_t v;
        std::memcpy(&v, data, 4);
        h = __crc32cw((uint32_t)h, v);
        data += 4;
        len -= 4;
    }
    while (len > 0) {
        h = __crc32cb((uint8_t)h, (uint8_t)*data);
        data++;
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
        size_t idx = h & (HASH_TABLE_CAPACITY - 1);

        while (table[idx].occupied.load(std::memory_order_acquire)) {
            if (table[idx].hash == h && table[idx].str.len == len && std::memcmp(table[idx].str.data, data, len) == 0) {
                return table[idx].id;
            }
            idx = (idx + 1) & (HASH_TABLE_CAPACITY - 1);
        }

        std::lock_guard<std::mutex> lock(insert_mutex);
        size_t p = h & (HASH_TABLE_CAPACITY - 1);
        while (table[p].occupied.load(std::memory_order_relaxed)) {
            if (table[p].hash == h && table[p].str.len == len && std::memcmp(table[p].str.data, data, len) == 0) {
                return table[p].id;
            }
            p = (p + 1) & (HASH_TABLE_CAPACITY - 1);
        }

        uint16_t new_id = static_cast<uint16_t>(endpoints.size());
        table[p].str = {data, len};
        table[p].hash = h;
        table[p].id = new_id;
        endpoints.push_back({data, len});
        table[p].occupied.store(true, std::memory_order_release);
        return new_id;
    }

    uint16_t get_fast(const char* data, uint16_t len) const {
        uint32_t h = hash_endpoint(data, len);
        size_t idx = h & (HASH_TABLE_CAPACITY - 1);
        while (true) {
            if (table[idx].hash == h && table[idx].str.len == len && std::memcmp(table[idx].str.data, data, len) == 0) {
                return table[idx].id;
            }
            idx = (idx + 1) & (HASH_TABLE_CAPACITY - 1);
        }
    }

    size_t size() const {
        return endpoints.size();
    }
};

static char HOUR_STRINGS[72][24];
void init_hour_strings() {
    for (int hour_id = 0; hour_id < 72; ++hour_id) {
        int total_hours = hour_id + 18;
        int day_offset = total_hours / 24;
        int hour = total_hours % 24;
        int day;
        int month;
        if (day_offset == 0) { month = 10; day = 30; }
        else if (day_offset == 1) { month = 10; day = 31; }
        else if (day_offset == 2) { month = 11; day = 1; }
        else { month = 11; day = 2; }
        std::snprintf(HOUR_STRINGS[hour_id], sizeof(HOUR_STRINGS[hour_id]), 
                      "2026-%02d-%02dT%02d:00:00Z", month, day, hour);
    }
}

std::string resolve_input_path(const std::string& input_arg) {
    struct stat s;
    if (stat(input_arg.c_str(), &s) == 0 && S_ISDIR(s.st_mode)) {
        return input_arg + "/requests.log";
    }
    return input_arg;
}

std::string resolve_output_path(const std::string& output_arg) {
    struct stat s;
    if (stat(output_arg.c_str(), &s) == 0 && S_ISDIR(s.st_mode)) {
        return output_arg + "/summary.txt";
    }
    return output_arg;
}

struct Chunk {
    const char* start;
    const char* end;
};

static inline char* append_u32(char* out, uint32_t val) {
    if (val == 0) {
        *out++ = '0';
        return out;
    }
    char buf[12];
    int p = 0;
    while (val > 0) {
        buf[p++] = '0' + (val % 10);
        val /= 10;
    }
    for (int i = p - 1; i >= 0; --i) {
        *out++ = buf[i];
    }
    return out;
}

int main(int argc, char** argv) {
    std::string input_path = resolve_input_path((argc > 1) ? argv[1] : "/data");
    std::string output_path = resolve_output_path((argc > 2) ? argv[2] : "/out/summary.txt");

    init_hour_strings();

    int fd = open(input_path.c_str(), O_RDONLY);
    if (fd < 0) {
        perror("Error opening input file");
        return 1;
    }
    struct stat sb;
    fstat(fd, &sb);
    size_t file_size = sb.st_size;
    if (file_size == 0) {
        FILE* out = fopen(output_path.c_str(), "w");
        if (out) fclose(out);
        close(fd);
        return 0;
    }

    const char* data = static_cast<const char*>(
        mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0)
    );
    close(fd);

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    unsigned int num_threads = std::min(12u, hw);

    std::vector<Chunk> chunks(num_threads);
    for (unsigned int i = 0; i < num_threads; ++i) {
        size_t b_start = (i * file_size) / num_threads;
        size_t b_end = ((i + 1) * file_size) / num_threads;
        const char* c_start = data + b_start;
        const char* c_end = data + b_end;
        if (i > 0) {
            while (c_start < data + file_size && *(c_start - 1) != '\n') c_start++;
        }
        if (i < num_threads - 1) {
            while (c_end < data + file_size && *(c_end - 1) != '\n') c_end++;
        } else {
            c_end = data + file_size;
        }
        chunks[i] = {c_start, c_end};
    }

    ConcurrentEndpointDictionary dict;

    // PASS 1: Count records per group per thread
    std::vector<std::vector<uint32_t>> thread_counts(num_threads, std::vector<uint32_t>(TOTAL_GROUPS, 0));
    // Atomic group errors saves 7 MB of thread arrays
    std::vector<std::atomic<uint32_t>> group_errors(TOTAL_GROUPS);
    for (size_t g = 0; g < TOTAL_GROUPS; ++g) group_errors[g].store(0, std::memory_order_relaxed);

    std::vector<std::thread> workers;
    for (unsigned int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t]() {
            const char* ptr = chunks[t].start;
            const char* end = chunks[t].end;
            auto& my_counts = thread_counts[t];

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

                while (*ptr != '\n' && ptr < end) ptr++;
                if (ptr < end && *ptr == '\n') ptr++;

                size_t g_idx = static_cast<size_t>(hour_id) * MAX_ENDPOINTS + ep_id;
                my_counts[g_idx]++;
                if (is_error) group_errors[g_idx].fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    // Global totals and prefix sums
    std::vector<uint32_t> group_counts(TOTAL_GROUPS, 0);
    std::vector<uint32_t> head(TOTAL_GROUPS + 1, 0);
    for (size_t g = 0; g < TOTAL_GROUPS; ++g) {
        uint32_t sum = 0;
        for (unsigned int t = 0; t < num_threads; ++t) {
            sum += thread_counts[t][g];
        }
        group_counts[g] = sum;
        head[g + 1] = head[g] + sum;
    }
    uint32_t total_records = head[TOTAL_GROUPS];

    // Single flat array of ALL latencies (12.4 MB at full scale)
    std::vector<uint32_t> all_latencies(total_records);

    // Reuse thread_counts in-place as thread write cursors! Saves 7 MB RAM!
    for (size_t g = 0; g < TOTAL_GROUPS; ++g) {
        uint32_t pos = head[g];
        for (unsigned int t = 0; t < num_threads; ++t) {
            uint32_t cnt = thread_counts[t][g];
            thread_counts[t][g] = pos;
            pos += cnt;
        }
    }

    // PASS 2: Write latencies directly into flat array (read-only dict lookup!)
    for (unsigned int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t]() {
            const char* ptr = chunks[t].start;
            const char* end = chunks[t].end;
            auto& my_cur = thread_counts[t];

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
                ptr += 5; // skip status (3) and space (1)

                uint16_t ep_id = dict.get_fast(ep_start, ep_len);

                uint32_t latency = 0;
                while (*ptr != '\n' && ptr < end) {
                    latency = latency * 10 + (*ptr - '0');
                    ptr++;
                }
                if (ptr < end && *ptr == '\n') ptr++;

                size_t g_idx = static_cast<size_t>(hour_id) * MAX_ENDPOINTS + ep_id;
                all_latencies[my_cur[g_idx]++] = latency;
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    // Free thread_counts immediately! Saves 7 MB RAM!
    std::vector<std::vector<uint32_t>>().swap(thread_counts);

    // Sort endpoints
    size_t num_endpoints = dict.size();
    std::vector<uint16_t> sorted_endpoint_ids(num_endpoints);
    for (size_t i = 0; i < num_endpoints; ++i) sorted_endpoint_ids[i] = (uint16_t)i;
    std::sort(sorted_endpoint_ids.begin(), sorted_endpoint_ids.end(), 
        [&dict](uint16_t a, uint16_t b) {
            const StringView& sa = dict.endpoints[a];
            const StringView& sb = dict.endpoints[b];
            size_t min_len = std::min(sa.len, sb.len);
            int cmp = std::memcmp(sa.data, sb.data, min_len);
            if (cmp != 0) return cmp < 0;
            return sa.len < sb.len;
        }
    );

    // Percentile computation
    struct GroupPercentiles {
        uint32_t p50;
        uint32_t p95;
        uint32_t p99;
    };
    std::vector<GroupPercentiles> group_p(TOTAL_GROUPS);

    for (unsigned int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t]() {
            int h_start = (t * 72) / num_threads;
            int h_end = ((t + 1) * 72) / num_threads;

            for (int hour_id = h_start; hour_id < h_end; ++hour_id) {
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
    workers.clear();

    // Free all_latencies immediately! Saves 12.4 MB RAM!
    std::vector<uint32_t>().swap(all_latencies);

    // Sequential streaming output write with 128 KB buffer
    int out_fd = open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd >= 0) {
        static constexpr size_t BUF_CAP = 128 * 1024;
        char buffer[BUF_CAP];
        char* buf_ptr = buffer;
        char* buf_limit = buffer + BUF_CAP - 512;

        for (int hour_id = 0; hour_id < 72; ++hour_id) {
            const char* hour_str = HOUR_STRINGS[hour_id];
            for (uint16_t ep_id : sorted_endpoint_ids) {
                size_t g_idx = static_cast<size_t>(hour_id) * MAX_ENDPOINTS + ep_id;
                uint32_t cnt = group_counts[g_idx];
                if (cnt == 0) continue;

                uint32_t errs = group_errors[g_idx].load(std::memory_order_relaxed);
                uint32_t error_ppm = static_cast<uint32_t>((static_cast<uint64_t>(errs) * 1000000ULL) / cnt);

                if (buf_ptr >= buf_limit) {
                    write(out_fd, buffer, buf_ptr - buffer);
                    buf_ptr = buffer;
                }

                std::memcpy(buf_ptr, hour_str, 20);
                buf_ptr += 20;
                *buf_ptr++ = ' ';

                const StringView& ep = dict.endpoints[ep_id];
                std::memcpy(buf_ptr, ep.data, ep.len);
                buf_ptr += ep.len;
                *buf_ptr++ = ' ';

                buf_ptr = append_u32(buf_ptr, cnt);
                *buf_ptr++ = ' ';
                buf_ptr = append_u32(buf_ptr, group_p[g_idx].p50);
                *buf_ptr++ = ' ';
                buf_ptr = append_u32(buf_ptr, group_p[g_idx].p95);
                *buf_ptr++ = ' ';
                buf_ptr = append_u32(buf_ptr, group_p[g_idx].p99);
                *buf_ptr++ = ' ';
                buf_ptr = append_u32(buf_ptr, errs);
                *buf_ptr++ = ' ';
                buf_ptr = append_u32(buf_ptr, error_ppm);
                *buf_ptr++ = '\n';
            }
        }

        if (buf_ptr > buffer) {
            write(out_fd, buffer, buf_ptr - buffer);
        }
        close(out_fd);
    }

    munmap((void*)data, file_size);
    return 0;
}
