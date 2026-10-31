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

static constexpr size_t MAX_ENDPOINTS = 2048;
static constexpr size_t TOTAL_GROUPS = 72 * MAX_ENDPOINTS;

struct StringView {
    const char* data;
    uint16_t len;
};

static inline uint64_t hash_endpoint(const char* data, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint8_t>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

static constexpr size_t HASH_TABLE_CAPACITY = 8192;
struct EndpointEntry {
    StringView str;
    uint16_t id = 0;
    std::atomic<bool> occupied{false};
};

class ConcurrentEndpointDictionary {
public:
    EndpointEntry table[HASH_TABLE_CAPACITY];
    std::vector<StringView> endpoints;
    std::mutex insert_mutex;

    ConcurrentEndpointDictionary() {
        endpoints.reserve(2048);
    }

    uint16_t get_or_insert(const char* data, uint16_t len) {
        uint64_t h = hash_endpoint(data, len);
        size_t idx = h & (HASH_TABLE_CAPACITY - 1);

        while (table[idx].occupied.load(std::memory_order_acquire)) {
            if (table[idx].str.len == len && std::memcmp(table[idx].str.data, data, len) == 0) {
                return table[idx].id;
            }
            idx = (idx + 1) & (HASH_TABLE_CAPACITY - 1);
        }

        std::lock_guard<std::mutex> lock(insert_mutex);
        size_t p = h & (HASH_TABLE_CAPACITY - 1);
        while (table[p].occupied.load(std::memory_order_relaxed)) {
            if (table[p].str.len == len && std::memcmp(table[p].str.data, data, len) == 0) {
                return table[p].id;
            }
            p = (p + 1) & (HASH_TABLE_CAPACITY - 1);
        }

        uint16_t new_id = static_cast<uint16_t>(endpoints.size());
        table[p].str = {data, len};
        table[p].id = new_id;
        endpoints.push_back({data, len});
        table[p].occupied.store(true, std::memory_order_release);
        return new_id;
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

static inline void append_u32(std::string& out, uint32_t val) {
    if (val == 0) { out.push_back('0'); return; }
    char buf[16];
    int p = 0;
    while (val > 0) {
        buf[p++] = '0' + (val % 10);
        val /= 10;
    }
    for (int i = p - 1; i >= 0; --i) out.push_back(buf[i]);
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

    // PASS 1: Count records per group per thread (Zero dynamic heap allocations!)
    std::vector<std::vector<uint32_t>> thread_counts(num_threads, std::vector<uint32_t>(TOTAL_GROUPS, 0));
    std::vector<std::vector<uint32_t>> thread_errors(num_threads, std::vector<uint32_t>(TOTAL_GROUPS, 0));

    std::vector<std::thread> workers;
    for (unsigned int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t]() {
            const char* ptr = chunks[t].start;
            const char* end = chunks[t].end;
            auto& my_counts = thread_counts[t];
            auto& my_errors = thread_errors[t];

            while (ptr < end) {
                int day_idx = (ptr[6] == '0') ? (ptr[9] - '0') : (1 + (ptr[9] - '0'));
                int hour = (ptr[11] - '0') * 10 + (ptr[12] - '0');
                uint8_t hour_id = static_cast<uint8_t>((day_idx * 24) + hour - 18);
                ptr += 28;

                while (*ptr != ' ') ptr++;
                ptr++;

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
                if (is_error) my_errors[g_idx]++;
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    // Global totals and prefix sums
    std::vector<uint32_t> group_counts(TOTAL_GROUPS, 0);
    std::vector<uint32_t> group_errors(TOTAL_GROUPS, 0);
    for (unsigned int t = 0; t < num_threads; ++t) {
        for (size_t g = 0; g < TOTAL_GROUPS; ++g) {
            group_counts[g] += thread_counts[t][g];
            group_errors[g] += thread_errors[t][g];
        }
    }

    std::vector<uint32_t> head(TOTAL_GROUPS + 1, 0);
    for (size_t g = 0; g < TOTAL_GROUPS; ++g) {
        head[g + 1] = head[g] + group_counts[g];
    }
    uint32_t total_records = head[TOTAL_GROUPS];

    // Single flat array of ALL latencies (exactly 12.4 MB at full scale!)
    std::vector<uint32_t> all_latencies(total_records);

    // Thread write cursors
    std::vector<std::vector<uint32_t>> thread_cur(num_threads, std::vector<uint32_t>(TOTAL_GROUPS, 0));
    for (size_t g = 0; g < TOTAL_GROUPS; ++g) {
        uint32_t pos = head[g];
        for (unsigned int t = 0; t < num_threads; ++t) {
            thread_cur[t][g] = pos;
            pos += thread_counts[t][g];
        }
    }
    thread_counts.clear();

    // PASS 2: Write latencies directly into flat array in parallel!
    for (unsigned int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t]() {
            const char* ptr = chunks[t].start;
            const char* end = chunks[t].end;
            auto& my_cur = thread_cur[t];

            while (ptr < end) {
                int day_idx = (ptr[6] == '0') ? (ptr[9] - '0') : (1 + (ptr[9] - '0'));
                int hour = (ptr[11] - '0') * 10 + (ptr[12] - '0');
                uint8_t hour_id = static_cast<uint8_t>((day_idx * 24) + hour - 18);
                ptr += 28;

                while (*ptr != ' ') ptr++;
                ptr++;

                const char* ep_start = ptr;
                while (*ptr != ' ') ptr++;
                uint16_t ep_len = static_cast<uint16_t>(ptr - ep_start);
                ptr++;

                uint16_t ep_id = dict.get_or_insert(ep_start, ep_len);
                ptr += 4;

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
    thread_cur.clear();

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

    // Parallel percentile computation and formatting
    std::vector<std::string> thread_output_buffers(num_threads);
    for (unsigned int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t]() {
            int h_start = (t * 72) / num_threads;
            int h_end = ((t + 1) * 72) / num_threads;
            std::string& out_buf = thread_output_buffers[t];
            out_buf.reserve(1024 * 1024);

            for (int hour_id = h_start; hour_id < h_end; ++hour_id) {
                const char* hour_str = HOUR_STRINGS[hour_id];
                for (uint16_t ep_id : sorted_endpoint_ids) {
                    size_t g_idx = static_cast<size_t>(hour_id) * MAX_ENDPOINTS + ep_id;
                    uint32_t cnt = group_counts[g_idx];
                    if (cnt == 0) continue;

                    uint32_t errs = group_errors[g_idx];
                    uint32_t error_ppm = static_cast<uint32_t>((static_cast<uint64_t>(errs) * 1000000ULL) / cnt);

                    uint32_t offset = head[g_idx];
                    auto it_begin = all_latencies.begin() + offset;
                    auto it_end = it_begin + cnt;

                    size_t rank_p50 = (static_cast<size_t>(50) * cnt + 99) / 100;
                    size_t rank_p95 = (static_cast<size_t>(95) * cnt + 99) / 100;
                    size_t rank_p99 = (static_cast<size_t>(99) * cnt + 99) / 100;

                    size_t idx_p50 = (rank_p50 > 0) ? (rank_p50 - 1) : 0;
                    size_t idx_p95 = (rank_p95 > 0) ? (rank_p95 - 1) : 0;
                    size_t idx_p99 = (rank_p99 > 0) ? (rank_p99 - 1) : 0;

                    std::nth_element(it_begin, it_begin + idx_p99, it_end);
                    uint32_t p99 = *(it_begin + idx_p99);

                    std::nth_element(it_begin, it_begin + idx_p95, it_begin + idx_p99);
                    uint32_t p95 = *(it_begin + idx_p95);

                    std::nth_element(it_begin, it_begin + idx_p50, it_begin + idx_p95);
                    uint32_t p50 = *(it_begin + idx_p50);

                    const StringView& ep = dict.endpoints[ep_id];
                    out_buf.append(hour_str);
                    out_buf.push_back(' ');
                    out_buf.append(ep.data, ep.len);
                    out_buf.push_back(' ');
                    append_u32(out_buf, cnt);
                    out_buf.push_back(' ');
                    append_u32(out_buf, p50);
                    out_buf.push_back(' ');
                    append_u32(out_buf, p95);
                    out_buf.push_back(' ');
                    append_u32(out_buf, p99);
                    out_buf.push_back(' ');
                    append_u32(out_buf, errs);
                    out_buf.push_back(' ');
                    append_u32(out_buf, error_ppm);
                    out_buf.push_back('\n');
                }
            }
        });
    }
    for (auto& w : workers) w.join();

    int out_fd = open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd >= 0) {
        for (const auto& buf : thread_output_buffers) {
            write(out_fd, buf.data(), buf.size());
        }
        close(out_fd);
    }

    munmap((void*)data, file_size);
    return 0;
}
