#include <iostream>
#include <vector>
#include <thread>
#include <string>
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

// Speed Demon 2026 - Track C: Join
struct SimpleBarrier {
    std::mutex mtx; std::condition_variable cv;
    unsigned int count, initial, gen{0};
    explicit SimpleBarrier(unsigned int n) : count(n), initial(n) {}
    void wait() {
        std::unique_lock<std::mutex> lock(mtx);
        unsigned int g = gen;
        if (--count == 0) { gen++; count = initial; cv.notify_all(); }
        else { cv.wait(lock, [this, g] { return gen != g; }); }
    }
};

static inline size_t count_quotes(const char* ptr, size_t len) {
    size_t count = 0; const char* end = ptr + len;
#if defined(__x86_64__) || defined(_M_X64)
    __m256i q = _mm256_set1_epi8('"');
    while (ptr + 64 <= end) {
        __m256i c0 = _mm256_loadu_si256((const __m256i*)ptr);
        __m256i c1 = _mm256_loadu_si256((const __m256i*)(ptr + 32));
        count += __builtin_popcount(_mm256_movemask_epi8(_mm256_cmpeq_epi8(c0, q))) +
                 __builtin_popcount(_mm256_movemask_epi8(_mm256_cmpeq_epi8(c1, q)));
        ptr += 64;
    }
#elif defined(__aarch64__)
    uint8x16_t q = vdupq_n_u8('"');
    while (ptr + 64 <= end) {
        uint8x16_t c0 = vld1q_u8((const uint8_t*)ptr), c1 = vld1q_u8((const uint8_t*)ptr + 16);
        uint8x16_t c2 = vld1q_u8((const uint8_t*)ptr + 32), c3 = vld1q_u8((const uint8_t*)ptr + 48);
        uint8x16_t s0 = vshrq_n_u8(vceqq_u8(c0, q), 7), s1 = vshrq_n_u8(vceqq_u8(c1, q), 7);
        uint8x16_t s2 = vshrq_n_u8(vceqq_u8(c2, q), 7), s3 = vshrq_n_u8(vceqq_u8(c3, q), 7);
        count += vaddlvq_u8(vaddq_u8(vaddq_u8(s0, s1), vaddq_u8(s2, s3)));
        ptr += 64;
    }
#endif
    while (ptr < end) { if (*ptr++ == '"') count++; }
    return count;
}

static inline uint64_t hash64(const char* data, size_t len) {
    uint64_t h = len * 0x517cc1b727220a95ULL ^ 0x9e3779b97f4a7c15ULL;
    const uint8_t* p = (const uint8_t*)data;
    while (len >= 8) {
        uint64_t k; std::memcpy(&k, p, 8);
        k = (k * 0xbf58476d1ce4e5b9ULL) ^ ((k * 0xbf58476d1ce4e5b9ULL) >> 31);
        h = ((h ^ (k * 0x94d049bb133111ebULL)) << 27) | ((h ^ (k * 0x94d049bb133111ebULL)) >> 37);
        p += 8; len -= 8;
    }
    if (len >= 4) {
        uint32_t k; std::memcpy(&k, p, 4);
        h = ((h ^ ((uint64_t)k * 0xbf58476d1ce4e5b9ULL)) << 13) | ((h ^ ((uint64_t)k * 0xbf58476d1ce4e5b9ULL)) >> 51);
        p += 4; len -= 4;
    }
    while (len > 0) {
        uint64_t v = (uint64_t)(*p++) * 0x94d049bb133111ebULL;
        h = ((h ^ v) << 7) | ((h ^ v) >> 57); len--;
    }
    h = (h ^ (h >> 33)) * 0xff51afd7ed558ccdULL;
    h = (h ^ (h >> 33)) * 0xc4ceb9fe1a85ec53ULL;
    return h ^ (h >> 33);
}

struct ThreadAggregate { uint32_t count = 0; uint64_t id_sum = 0, name_bytes = 0, note_bytes = 0; int64_t amount_sum = 0, amount_min = 0, amount_max = 0; };
struct FinalCountry { std::string name; uint32_t count = 0; uint64_t id_sum = 0, distinct_names = 0, name_bytes = 0, note_bytes = 0; int64_t amount_sum = 0, amount_min = 0, amount_max = 0; };

std::string format_cents(int64_t cents) {
    bool neg = (cents < 0); uint64_t abs_c = neg ? -cents : cents; char buf[64];
    std::snprintf(buf, sizeof(buf), neg ? "-%llu.%02llu" : "%llu.%02llu", (unsigned long long)(abs_c / 100), (unsigned long long)(abs_c % 100));
    return std::string(buf);
}

std::string resolve_output_path(const std::string& out_dir) {
    struct stat s;
    return (stat(out_dir.c_str(), &s) == 0 && S_ISDIR(s.st_mode)) ? out_dir + "/aggregates.txt" : out_dir;
}

int main(int argc, char** argv) {
    std::string data_dir = (argc > 1) ? argv[1] : "/data";
    std::string out_path = resolve_output_path((argc > 2) ? argv[2] : "/out/aggregates.txt");

    // 1. Fast Pre-discovery of Countries from regions.csv
    int rfd = open((data_dir + "/regions.csv").c_str(), O_RDONLY);
    struct stat rsb; fstat(rfd, &rsb);
    const char* r_data = (const char*)mmap(nullptr, rsb.st_size, PROT_READ, MAP_PRIVATE, rfd, 0);

    struct CountryDict { uint32_t code = 0; char name[16] = {0}; uint8_t len = 0; } dict[256];
    uint8_t num_dict = 0;
    struct CodeSlot { uint32_t code = 0; uint8_t cid = 255; } country_hash[1024];
    for (int i = 0; i < 1024; ++i) country_hash[i].cid = 255;

    auto get_cid = [&](const char* str, size_t len) -> uint8_t {
        uint32_t c = 0; std::memcpy(&c, str, std::min(len, (size_t)4));
        uint32_t slot = (c * 0x9E3779B9u) & 1023;
        while (country_hash[slot].cid != 255) {
            if (country_hash[slot].code == c) return country_hash[slot].cid;
            slot = (slot + 1) & 1023;
        }
        uint8_t id = num_dict++; country_hash[slot] = {c, id};
        dict[id].code = c; dict[id].len = (uint8_t)len;
        std::memcpy(dict[id].name, str, len);
        return id;
    };

    static constexpr size_t DIRECT_TABLE_SIZE = 1048576;
    std::vector<uint8_t> region_to_country(DIRECT_TABLE_SIZE, 255);

    const char* rp = r_data, *r_end = r_data + rsb.st_size;
    while (rp < r_end && *rp++ != '\n');
    while (rp < r_end) {
        while (rp < r_end && *rp != ',') rp++;
        if (rp >= r_end) break;
        rp++; const char* cs = rp;
        while (rp < r_end && *rp != '\r' && *rp != '\n') rp++;
        get_cid(cs, rp - cs);
        if (rp < r_end && *rp == '\r') rp++;
        if (rp < r_end && *rp == '\n') rp++;
    }
    uint8_t unmatched_cid = num_dict++;
    std::memcpy(dict[unmatched_cid].name, "_UNMATCHED", 10);
    dict[unmatched_cid].len = 10;
    size_t num_countries = num_dict;

    // 2. Map records.csv
    int rec_fd = open((data_dir + "/records.csv").c_str(), O_RDONLY);
    struct stat rec_sb; fstat(rec_fd, &rec_sb);
    const char* data = (const char*)mmap(nullptr, rec_sb.st_size, PROT_READ, MAP_PRIVATE, rec_fd, 0);
    const char* end = data + rec_sb.st_size, *start = data;

    if (end - start >= 3 && (uint8_t)start[0] == 0xEF && (uint8_t)start[1] == 0xBB && (uint8_t)start[2] == 0xBF) start += 3;
    while (start < end && *start++ != '\n');

    size_t total_len = end - start;
    int T = std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
    size_t chunk_size = total_len / T;

    std::vector<size_t> q_counts(T, 0);
    std::vector<int> prefix_parity(T + 1, 0);
    std::vector<const char*> boundaries(T + 1);
    boundaries[0] = start; boundaries[T] = end;

    struct ThreadState {
        std::vector<ThreadAggregate> aggs;
        std::vector<uint64_t> names;
        std::vector<uint32_t> offsets;
    };
    std::vector<ThreadState> thread_states(T);
    for (int i = 0; i < T; ++i) {
        thread_states[i].aggs.resize(num_countries);
        thread_states[i].offsets.resize(num_countries + 1, 0);
        thread_states[i].names.reserve(chunk_size / 40);
    }
    std::vector<FinalCountry> final_countries(num_countries);
    for (size_t i = 0; i < num_countries; ++i) final_countries[i].name = std::string(dict[i].name, dict[i].len);

    SimpleBarrier barrier(T);
    std::vector<std::thread> workers; workers.reserve(T);

    for (int t = 0; t < T; ++t) {
        workers.emplace_back([&, t]() {
            // PHASE 1: Parallel Regions Ingest
            size_t r_bstart = t * (rsb.st_size / T);
            size_t r_bend = (t == T - 1) ? rsb.st_size : (t + 1) * (rsb.st_size / T);
            const char* r_ptr = r_data + r_bstart, *r_chunk_end = r_data + r_bend;
            if (t > 0) { while (r_ptr < r_chunk_end && *r_ptr != '\n') r_ptr++; if (r_ptr < r_chunk_end) r_ptr++; }
            while (r_ptr < r_chunk_end) {
                if (*r_ptr == '\r' || *r_ptr == '\n') { r_ptr++; continue; }
                uint32_t rid = 0;
                while (r_ptr < r_end && *r_ptr >= '0' && *r_ptr <= '9') rid = rid * 10 + (*r_ptr++ - '0');
                if (r_ptr < r_end && *r_ptr == ',') r_ptr++;
                const char* cs = r_ptr;
                while (r_ptr < r_end && *r_ptr != '\r' && *r_ptr != '\n') r_ptr++;
                size_t clen = r_ptr - cs;
                if (r_ptr < r_end && *r_ptr == '\r') r_ptr++;
                if (r_ptr < r_end && *r_ptr == '\n') r_ptr++;

                uint32_t c = 0; std::memcpy(&c, cs, std::min(clen, (size_t)4));
                uint32_t slot = (c * 0x9E3779B9u) & 1023;
                uint8_t cid = unmatched_cid;
                while (country_hash[slot].cid != 255) {
                    if (country_hash[slot].code == c) { cid = country_hash[slot].cid; break; }
                    slot = (slot + 1) & 1023;
                }
                if (rid < DIRECT_TABLE_SIZE) region_to_country[rid] = cid;
            }

            // PHASE 2: SIMD Quote Scan
            const char* c_start = start + t * chunk_size;
            size_t len = (t == T - 1) ? (end - c_start) : chunk_size;
            q_counts[t] = count_quotes(c_start, len);

            barrier.wait();

            if (t == 0) {
                for (int i = 0; i < T; ++i) prefix_parity[i + 1] = (prefix_parity[i] + q_counts[i]) % 2;
                for (int i = 1; i < T; ++i) {
                    const char* ptr = start + i * chunk_size;
                    int p = prefix_parity[i];
                    while (ptr < end) {
                        if (*ptr == '"') p ^= 1;
                        else if (p == 0 && *ptr == '\n') {
                            ptr++;
                            if (ptr + 2 < end && (uint8_t)ptr[0] == 0xEF && (uint8_t)ptr[1] == 0xBB && (uint8_t)ptr[2] == 0xBF) ptr += 3;
                            boundaries[i] = ptr; break;
                        }
                        ptr++;
                    }
                }
            }
            barrier.wait();

            // PHASE 3: Parallel Record Parsing
            auto& state = thread_states[t];
            auto& aggs = state.aggs; auto& names = state.names;
            const char* ptr = boundaries[t], *c_end = boundaries[t + 1];
            char name_buf[512];

            while (ptr < c_end) {
                if (c_end - ptr >= 3 && (uint8_t)ptr[0] == 0xEF && (uint8_t)ptr[1] == 0xBB && (uint8_t)ptr[2] == 0xBF) ptr += 3;
                if (ptr >= c_end) break;

                uint64_t id = 0;
                while (ptr < c_end && *ptr >= '0' && *ptr <= '9') id = id * 10 + (*ptr++ - '0');
                if (ptr < c_end && *ptr == ',') ptr++;

                size_t name_len = 0; uint64_t nh;
                if (ptr < c_end && *ptr == '"') {
                    ptr++;
                    while (ptr < c_end) {
                        if (*ptr == '"') {
                            if (ptr + 1 < c_end && *(ptr + 1) == '"') {
                                if (name_len < sizeof(name_buf)) name_buf[name_len++] = '"';
                                ptr += 2;
                            } else { ptr++; break; }
                        } else {
                            if (name_len < sizeof(name_buf)) name_buf[name_len++] = *ptr;
                            ptr++;
                        }
                    }
                    nh = hash64(name_buf, name_len);
                } else {
                    const char* ns = ptr;
                    while (ptr < c_end && *ptr != ',') ptr++;
                    name_len = ptr - ns; nh = hash64(ns, name_len);
                }
                if (ptr < c_end && *ptr == ',') ptr++;

                size_t note_bytes = 0;
                if (ptr < c_end && *ptr == '"') {
                    ptr++;
                    while (ptr < c_end) {
                        if (*ptr == '"') {
                            if (ptr + 1 < c_end && *(ptr + 1) == '"') { note_bytes++; ptr += 2; }
                            else { ptr++; break; }
                        } else { note_bytes++; ptr++; }
                    }
                } else {
                    const char* nts = ptr;
                    while (ptr < c_end && *ptr != ',') ptr++;
                    note_bytes = ptr - nts;
                }
                if (ptr < c_end && *ptr == ',') ptr++;

                uint32_t region_id = 0;
                while (ptr < c_end && *ptr >= '0' && *ptr <= '9') region_id = region_id * 10 + (*ptr++ - '0');
                if (ptr < c_end && *ptr == ',') ptr++;

                bool negative = false;
                if (ptr < c_end && *ptr == '-') { negative = true; ptr++; }
                int64_t whole = 0;
                while (ptr < c_end && *ptr >= '0' && *ptr <= '9') whole = whole * 10 + (*ptr++ - '0');
                if (ptr < c_end && *ptr == '.') ptr++;
                int64_t frac = 0;
                if (ptr + 1 < c_end && ptr[0] >= '0' && ptr[0] <= '9' && ptr[1] >= '0' && ptr[1] <= '9') {
                    frac = (ptr[0] - '0') * 10 + (ptr[1] - '0');
                    ptr += 2;
                }
                int64_t cents = (whole * 100 + frac) * (negative ? -1 : 1);
                if (ptr < c_end && *ptr == '\r') ptr++;
                if (ptr < c_end && *ptr == '\n') ptr++;

                uint8_t cid = (region_id < DIRECT_TABLE_SIZE) ? region_to_country[region_id] : unmatched_cid;
                if (cid == 255) cid = unmatched_cid;

                auto& c = aggs[cid];
                c.count++; c.id_sum += id; c.amount_sum += cents;
                if (c.count == 1 || cents < c.amount_min) c.amount_min = cents;
                if (c.count == 1 || cents > c.amount_max) c.amount_max = cents;
                c.name_bytes += name_len; c.note_bytes += note_bytes;

                names.push_back(((uint64_t)cid << 56) | (nh & 0x00FFFFFFFFFFFFFFULL));
            }

            madvise((void*)boundaries[t], (boundaries[t + 1] - boundaries[t]), MADV_DONTNEED);

            std::sort(names.begin(), names.end());
            size_t n_size = names.size(), cur_idx = 0;
            for (size_t c = 0; c < num_countries; ++c) {
                state.offsets[c] = (uint32_t)cur_idx;
                while (cur_idx < n_size && (names[cur_idx] >> 56) == c) cur_idx++;
            }
            state.offsets[num_countries] = (uint32_t)n_size;

            barrier.wait();

            if (t == 0) {
                for (int th = 0; th < T; ++th) {
                    for (size_t c = 0; c < num_countries; ++c) {
                        const auto& src = thread_states[th].aggs[c];
                        if (src.count == 0) continue;
                        auto& dst = final_countries[c];
                        if (dst.count == 0) { dst.amount_min = src.amount_min; dst.amount_max = src.amount_max; }
                        else {
                            if (src.amount_min < dst.amount_min) dst.amount_min = src.amount_min;
                            if (src.amount_max > dst.amount_max) dst.amount_max = src.amount_max;
                        }
                        dst.count += src.count; dst.id_sum += src.id_sum; dst.amount_sum += src.amount_sum;
                        dst.name_bytes += src.name_bytes; dst.note_bytes += src.note_bytes;
                    }
                }
            }
            barrier.wait();

            // PHASE 4: Distinct Names K-way Merge
            const uint64_t* ptrs[8]; const uint64_t* ends[8];
            for (size_t c = t; c < num_countries; c += T) {
                int active_count = 0;
                for (int th = 0; th < T; ++th) {
                    ptrs[th] = thread_states[th].names.data() + thread_states[th].offsets[c];
                    ends[th] = thread_states[th].names.data() + thread_states[th].offsets[c + 1];
                    if (ptrs[th] < ends[th]) active_count++;
                }
                if (active_count == 0) { final_countries[c].distinct_names = 0; continue; }

                size_t distinct = 0; uint64_t last = UINT64_MAX;
                while (true) {
                    uint64_t min_val = UINT64_MAX; int min_th = -1;
                    for (int th = 0; th < T; ++th) {
                        if (ptrs[th] < ends[th] && *ptrs[th] < min_val) { min_val = *ptrs[th]; min_th = th; }
                    }
                    if (min_th == -1) break;
                    ptrs[min_th]++;
                    if (min_val != last) { distinct++; last = min_val; }
                }
                final_countries[c].distinct_names = distinct;
            }
        });
    }
    for (auto& w : workers) w.join();

    // 5. Output sorted country aggregates
    std::vector<uint8_t> sorted_cids(num_countries);
    for (size_t i = 0; i < num_countries; ++i) sorted_cids[i] = (uint8_t)i;
    std::sort(sorted_cids.begin(), sorted_cids.end(), [&](uint8_t a, uint8_t b) { return final_countries[a].name < final_countries[b].name; });

    FILE* out = fopen(out_path.c_str(), "w");
    if (!out) out = fopen("aggregates.txt", "w");
    if (!out) return 1;
    std::vector<char> write_buf(1024 * 1024);
    setvbuf(out, write_buf.data(), _IOFBF, write_buf.size());

    for (uint8_t cid : sorted_cids) {
        const auto& c = final_countries[cid];
        if (c.count == 0) continue;
        std::fprintf(out, "%s %u %llu %s %s %s %llu %llu %llu\n",
                     c.name.c_str(), c.count, (unsigned long long)c.id_sum,
                     format_cents(c.amount_sum).c_str(), format_cents(c.amount_min).c_str(), format_cents(c.amount_max).c_str(),
                     (unsigned long long)c.distinct_names,
                     (unsigned long long)c.name_bytes, (unsigned long long)c.note_bytes);
    }
    std::fclose(out);
    return 0;
}