#include <iostream>
#include <string>
#include <chrono>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <unordered_set>
#include <unordered_map>

/**
 * SPEED DEMON 2026 - Track C: Join
 * 
 * 1. Flat direct-lookup table for 10M region IDs -> country ID (O(1) in 1 cycle).
 * 2. Memory-mapped hostile CSV reading (mmap + MADV_SEQUENTIAL).
 * 3. Mid-file BOM handling (U+FEFF / 0xEF 0xBB 0xBF detection at record boundaries).
 * 4. Mixed CRLF (\r\n) and LF (\n) record terminator support.
 * 5. Full RFC CSV unescaping ("" -> ") and embedded newline support.
 * 6. Zero-float integer cents money tracking.
 * 7. Online aggregation with distinct name counting.
 * 8. Byte-exact output formatting to /out/aggregates.txt.
 */

struct MappedFile {
    int fd = -1;
    size_t size = 0;
    const char* data = nullptr;

    bool open_and_map(const std::string& path) {
        fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;

        struct stat sb;
        if (fstat(fd, &sb) < 0) {
            close(fd);
            return false;
        }
        size = sb.st_size;
        if (size == 0) return true;

        data = static_cast<const char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
        if (data == MAP_FAILED) {
            close(fd);
            return false;
        }
        #ifdef MADV_SEQUENTIAL
        madvise((void*)data, size, MADV_SEQUENTIAL);
        #endif
        return true;
    }

    void close_map() {
        if (data && data != MAP_FAILED && size > 0) {
            munmap((void*)data, size);
        }
        if (fd >= 0) close(fd);
    }

    ~MappedFile() {
        close_map();
    }
};

struct CountryAggregate {
    std::string name;
    uint32_t count = 0;
    uint64_t id_sum = 0;
    int64_t amount_sum = 0;
    int64_t amount_min = 0;
    int64_t amount_max = 0;
    uint64_t name_bytes = 0;
    uint64_t note_bytes = 0;
    std::unordered_set<std::string> names;
};

// Formats integer cents into exact decimal string: -525 -> "-5.25", -5 -> "-0.05", 0 -> "0.00"
std::string format_cents(int64_t cents) {
    bool neg = (cents < 0);
    uint64_t abs_cents = neg ? -cents : cents;
    uint64_t dollars = abs_cents / 100;
    uint64_t rem = abs_cents % 100;

    char buf[64];
    if (neg) {
        std::snprintf(buf, sizeof(buf), "-%llu.%02llu", static_cast<unsigned long long>(dollars), static_cast<unsigned long long>(rem));
    } else {
        std::snprintf(buf, sizeof(buf), "%llu.%02llu", static_cast<unsigned long long>(dollars), static_cast<unsigned long long>(rem));
    }
    return std::string(buf);
}

std::string resolve_output_path(const std::string& out_dir) {
    struct stat s;
    if (stat(out_dir.c_str(), &s) == 0 && S_ISDIR(s.st_mode)) {
        return out_dir + "/aggregates.txt";
    }
    return out_dir;
}

int main(int argc, char** argv) {
    std::string data_dir = (argc > 1) ? argv[1] : "/data";
    std::string out_arg = (argc > 2) ? argv[2] : "/out/aggregates.txt";
    std::string out_path = resolve_output_path(out_arg);

    std::string regions_path = data_dir + "/regions.csv";
    std::string records_path = data_dir + "/records.csv";

    auto start_time = std::chrono::high_resolution_clock::now();

    // 1. Load regions.csv
    MappedFile regions_file;
    if (!regions_file.open_and_map(regions_path)) {
        std::cerr << "Error opening " << regions_path << std::endl;
        return 1;
    }

    // Direct lookup table for regions up to 10,000,000
    // 10MB flat array gives O(1) single-cycle country lookup
    static constexpr size_t MAX_REGIONS = 10000000;
    std::vector<uint8_t> region_to_country(MAX_REGIONS, 255);

    std::vector<std::string> country_codes;
    country_codes.reserve(256);

    std::unordered_map<std::string, uint8_t> country_code_map;

    const char* r_ptr = regions_file.data;
    const char* r_end = regions_file.data + regions_file.size;

    // Skip regions header "region_id,country\n"
    while (r_ptr < r_end && *r_ptr != '\n') r_ptr++;
    if (r_ptr < r_end && *r_ptr == '\n') r_ptr++;

    while (r_ptr < r_end) {
        if (*r_ptr == '\r' || *r_ptr == '\n') {
            r_ptr++;
            continue;
        }

        uint32_t rid = 0;
        while (r_ptr < r_end && *r_ptr >= '0' && *r_ptr <= '9') {
            rid = rid * 10 + (*r_ptr - '0');
            r_ptr++;
        }
        if (r_ptr < r_end && *r_ptr == ',') r_ptr++;

        const char* c_start = r_ptr;
        while (r_ptr < r_end && *r_ptr != '\r' && *r_ptr != '\n') r_ptr++;
        std::string country(c_start, r_ptr - c_start);

        if (r_ptr < r_end && *r_ptr == '\r') r_ptr++;
        if (r_ptr < r_end && *r_ptr == '\n') r_ptr++;

        auto it = country_code_map.find(country);
        uint8_t cid;
        if (it == country_code_map.end()) {
            cid = static_cast<uint8_t>(country_codes.size());
            country_code_map[country] = cid;
            country_codes.push_back(country);
        } else {
            cid = it->second;
        }

        if (rid < MAX_REGIONS) {
            region_to_country[rid] = cid;
        }
    }

    uint8_t unmatched_cid = static_cast<uint8_t>(country_codes.size());
    country_codes.push_back("_UNMATCHED");

    // Initialize country aggregates
    std::vector<CountryAggregate> countries(country_codes.size());
    for (size_t i = 0; i < country_codes.size(); ++i) {
        countries[i].name = country_codes[i];
    }

    // 2. Parse records.csv
    MappedFile records_file;
    if (!records_file.open_and_map(records_path)) {
        std::cerr << "Error opening " << records_path << std::endl;
        return 1;
    }

    const char* ptr = records_file.data;
    const char* end = records_file.data + records_file.size;

    // Check & skip initial BOM (0xEF 0xBB 0xBF)
    if (end - ptr >= 3 && static_cast<uint8_t>(ptr[0]) == 0xEF &&
        static_cast<uint8_t>(ptr[1]) == 0xBB && static_cast<uint8_t>(ptr[2]) == 0xBF) {
        ptr += 3;
    }

    // Skip header line: "id,name,note,region_id,amount" + CRLF/LF
    while (ptr < end && *ptr != '\n') ptr++;
    if (ptr < end && *ptr == '\n') ptr++;

    // Reusable stack buffer for unescaping names
    char unescaped_name[512];
    size_t record_count = 0;

    while (ptr < end) {
        // Check for mid-file BOM (0xEF 0xBB 0xBF) at record start
        if (end - ptr >= 3 && static_cast<uint8_t>(ptr[0]) == 0xEF &&
            static_cast<uint8_t>(ptr[1]) == 0xBB && static_cast<uint8_t>(ptr[2]) == 0xBF) {
            ptr += 3;
        }

        if (ptr >= end) break;

        record_count++;

        // 1. Parse id
        uint64_t id = 0;
        while (ptr < end && *ptr >= '0' && *ptr <= '9') {
            id = id * 10 + (*ptr - '0');
            ptr++;
        }
        if (ptr < end && *ptr == ',') ptr++;

        // 2. Parse name
        size_t name_len = 0;
        if (ptr < end && *ptr == '"') {
            ptr++; // skip opening quote
            while (ptr < end) {
                if (*ptr == '"') {
                    if (ptr + 1 < end && *(ptr + 1) == '"') {
                        if (name_len < sizeof(unescaped_name)) {
                            unescaped_name[name_len++] = '"';
                        }
                        ptr += 2;
                    } else {
                        ptr++; // skip closing quote
                        break;
                    }
                } else {
                    if (name_len < sizeof(unescaped_name)) {
                        unescaped_name[name_len++] = *ptr;
                    }
                    ptr++;
                }
            }
        } else {
            // unquoted (empty or characters until ',')
            while (ptr < end && *ptr != ',') {
                if (name_len < sizeof(unescaped_name)) {
                    unescaped_name[name_len++] = *ptr;
                }
                ptr++;
            }
        }
        if (ptr < end && *ptr == ',') ptr++;

        // 3. Parse note (we only need the byte length of unescaped text)
        size_t note_bytes = 0;
        if (ptr < end && *ptr == '"') {
            ptr++;
            while (ptr < end) {
                if (*ptr == '"') {
                    if (ptr + 1 < end && *(ptr + 1) == '"') {
                        note_bytes++;
                        ptr += 2;
                    } else {
                        ptr++;
                        break;
                    }
                } else {
                    note_bytes++;
                    ptr++;
                }
            }
        } else {
            while (ptr < end && *ptr != ',') {
                note_bytes++;
                ptr++;
            }
        }
        if (ptr < end && *ptr == ',') ptr++;

        // 4. Parse region_id
        uint32_t region_id = 0;
        while (ptr < end && *ptr >= '0' && *ptr <= '9') {
            region_id = region_id * 10 + (*ptr - '0');
            ptr++;
        }
        if (ptr < end && *ptr == ',') ptr++;

        // 5. Parse amount in exact integer cents
        bool negative = false;
        if (ptr < end && *ptr == '-') {
            negative = true;
            ptr++;
        }
        int64_t whole = 0;
        while (ptr < end && *ptr >= '0' && *ptr <= '9') {
            whole = whole * 10 + (*ptr - '0');
            ptr++;
        }
        if (ptr < end && *ptr == '.') ptr++;

        int64_t frac = 0;
        if (ptr + 1 < end && ptr[0] >= '0' && ptr[0] <= '9' && ptr[1] >= '0' && ptr[1] <= '9') {
            frac = (ptr[0] - '0') * 10 + (ptr[1] - '0');
            ptr += 2;
        }

        int64_t cents = whole * 100 + frac;
        if (negative) cents = -cents;

        // Skip record terminator: \r\n or \n
        if (ptr < end && *ptr == '\r') ptr++;
        if (ptr < end && *ptr == '\n') ptr++;

        // 6. Aggregate into Country
        uint8_t cid = (region_id < MAX_REGIONS) ? region_to_country[region_id] : unmatched_cid;
        if (cid == 255) cid = unmatched_cid;

        CountryAggregate& c = countries[cid];
        c.count++;
        c.id_sum += id;
        c.amount_sum += cents;
        if (c.count == 1 || cents < c.amount_min) c.amount_min = cents;
        if (c.count == 1 || cents > c.amount_max) c.amount_max = cents;
        c.name_bytes += name_len;
        c.note_bytes += note_bytes;
        c.names.emplace(unescaped_name, name_len);
    }

    // 3. Sort countries alphabetically (spec specifies bytewise ascending; _UNMATCHED sorts after all [A-Z]{3})
    std::vector<uint8_t> sorted_country_ids(countries.size());
    for (size_t i = 0; i < countries.size(); ++i) {
        sorted_country_ids[i] = static_cast<uint8_t>(i);
    }

    std::sort(sorted_country_ids.begin(), sorted_country_ids.end(),
        [&countries](uint8_t a, uint8_t b) {
            return countries[a].name < countries[b].name;
        }
    );

    // 4. Output to aggregates.txt
    FILE* out = fopen(out_path.c_str(), "w");
    if (!out) {
        std::cerr << "Error opening output file: " << out_path << std::endl;
        return 1;
    }

    std::vector<char> write_buf(1024 * 1024);
    setvbuf(out, write_buf.data(), _IOFBF, write_buf.size());

    size_t output_lines = 0;

    for (uint8_t cid : sorted_country_ids) {
        const auto& c = countries[cid];
        if (c.count == 0) continue;

        output_lines++;
        std::string sum_str = format_cents(c.amount_sum);
        std::string min_str = format_cents(c.amount_min);
        std::string max_str = format_cents(c.amount_max);

        std::fprintf(out, "%s %u %llu %s %s %s %zu %llu %llu\n",
                     c.name.c_str(),
                     c.count,
                     static_cast<unsigned long long>(c.id_sum),
                     sum_str.c_str(),
                     min_str.c_str(),
                     max_str.c_str(),
                     c.names.size(),
                     static_cast<unsigned long long>(c.name_bytes),
                     static_cast<unsigned long long>(c.note_bytes));
    }

    std::fclose(out);

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> total_elapsed = end_time - start_time;

    std::cout << "\n=== Track C Complete Pipeline Results ===" << std::endl;
    std::cout << "Records Parsed:     " << record_count << std::endl;
    std::cout << "Output Groups:      " << output_lines << std::endl;
    std::cout << "Total Elapsed Time: " << total_elapsed.count() << " ms (" 
              << (total_elapsed.count() / 1000.0) << " s)" << std::endl;
    std::cout << "Output File:        " << out_path << std::endl;

    return 0;
}
