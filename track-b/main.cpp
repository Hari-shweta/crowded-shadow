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
#include <unordered_map>

/**
 * SPEED DEMON 2026 - Track B: Index (Milestone 2 - Inverted Index & Query Engine)
 * 
 * 1. Parse queries.txt first -> collect query vocabulary (only terms that matter!).
 * 2. Flat open-addressing vocabulary hash table (maps term string -> term_id).
 * 3. Compact 64-bit document bitsets per term.
 * 4. Scan docs.txt -> only index tokens in query vocabulary.
 * 5. Boolean queries: Hardware-accelerated bitwise operations (&, |, & ~) + popcount / ctz.
 * 6. Phrase queries: Intersect term bitsets to get candidates, then verify exact token sequence.
 * 7. Results emitted directly into /out/results.txt in exact query order.
 */

alignas(64) static uint8_t TOKEN_LOWER[256];
alignas(64) static bool IS_TOKEN_CHAR[256];

void init_token_tables() {
    std::memset(TOKEN_LOWER, 0, sizeof(TOKEN_LOWER));
    std::memset(IS_TOKEN_CHAR, 0, sizeof(IS_TOKEN_CHAR));

    for (int c = '0'; c <= '9'; ++c) {
        TOKEN_LOWER[c] = static_cast<uint8_t>(c);
        IS_TOKEN_CHAR[c] = true;
    }
    for (int c = 'a'; c <= 'z'; ++c) {
        TOKEN_LOWER[c] = static_cast<uint8_t>(c);
        IS_TOKEN_CHAR[c] = true;
    }
    for (int c = 'A'; c <= 'Z'; ++c) {
        TOKEN_LOWER[c] = static_cast<uint8_t>(c + 32);
        IS_TOKEN_CHAR[c] = true;
    }
}

struct StringView {
    const char* data;
    uint16_t len;

    bool operator==(const StringView& other) const {
        return len == other.len && std::memcmp(data, other.data, len) == 0;
    }
};

inline uint64_t hash_term(const char* data, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint8_t>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

// Fixed-capacity hash table for query vocabulary
static constexpr size_t VOCAB_CAPACITY = 131072;

struct VocabEntry {
    std::string str;
    int32_t id = -1;
    bool occupied = false;
};

class Vocabulary {
public:
    VocabEntry table[VOCAB_CAPACITY];
    std::vector<std::string> terms;

    Vocabulary() {
        terms.reserve(32768);
    }

    int32_t get_or_insert(const char* data, size_t len) {
        uint64_t h = hash_term(data, len);
        size_t idx = h & (VOCAB_CAPACITY - 1);

        while (table[idx].occupied) {
            if (table[idx].str.length() == len && std::memcmp(table[idx].str.data(), data, len) == 0) {
                return table[idx].id;
            }
            idx = (idx + 1) & (VOCAB_CAPACITY - 1);
        }

        int32_t new_id = static_cast<int32_t>(terms.size());
        table[idx].str.assign(data, len);
        table[idx].id = new_id;
        table[idx].occupied = true;
        terms.emplace_back(data, len);
        return new_id;
    }

    int32_t lookup(const char* data, size_t len) const {
        uint64_t h = hash_term(data, len);
        size_t idx = h & (VOCAB_CAPACITY - 1);

        while (table[idx].occupied) {
            if (table[idx].str.length() == len && std::memcmp(table[idx].str.data(), data, len) == 0) {
                return table[idx].id;
            }
            idx = (idx + 1) & (VOCAB_CAPACITY - 1);
        }
        return -1;
    }

    size_t size() const {
        return terms.size();
    }
};

enum class QueryType : uint8_t {
    BOOL_QUERY,
    PHRASE_QUERY
};

enum class BoolOp : uint8_t {
    AND,
    OR,
    NOT
};

struct BoolQuery {
    std::vector<int32_t> term_ids;
    std::vector<BoolOp> ops;
};

struct PhraseQuery {
    std::vector<int32_t> term_ids;
    std::vector<std::string> raw_terms;
};

struct Query {
    QueryType type;
    BoolQuery bool_q;
    PhraseQuery phrase_q;
};

struct DocRef {
    const char* text_start = nullptr;
    uint32_t text_len = 0;
};

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

std::string resolve_input_path(const std::string& data_dir, const std::string& filename) {
    return data_dir + "/" + filename;
}

std::string resolve_output_path(const std::string& out_dir) {
    struct stat s;
    if (stat(out_dir.c_str(), &s) == 0 && S_ISDIR(s.st_mode)) {
        return out_dir + "/results.txt";
    }
    return out_dir;
}

int main(int argc, char** argv) {
    init_token_tables();

    std::string data_dir = (argc > 1) ? argv[1] : "/data";
    std::string out_arg = (argc > 2) ? argv[2] : "/out/results.txt";
    std::string out_path = resolve_output_path(out_arg);

    std::string docs_path = resolve_input_path(data_dir, "docs.txt");
    std::string queries_path = resolve_input_path(data_dir, "queries.txt");

    auto start_time = std::chrono::high_resolution_clock::now();

    // 1. Map queries.txt and extract vocabulary
    MappedFile queries_file;
    if (!queries_file.open_and_map(queries_path)) {
        std::cerr << "Error opening " << queries_path << std::endl;
        return 1;
    }

    Vocabulary vocab;
    std::vector<Query> queries;
    queries.reserve(65536);

    const char* q_ptr = queries_file.data;
    const char* q_end = queries_file.data + queries_file.size;

    while (q_ptr < q_end) {
        const char* line_start = q_ptr;
        while (q_ptr < q_end && *q_ptr != '\n') q_ptr++;
        const char* line_end = q_ptr;
        if (q_ptr < q_end && *q_ptr == '\n') q_ptr++;

        while (line_end > line_start && (*(line_end - 1) == '\r' || *(line_end - 1) == ' ')) line_end--;
        if (line_end <= line_start) continue;

        Query q;
        const char* p = line_start;

        if (line_end - p >= 4 && std::memcmp(p, "BOOL", 4) == 0) {
            q.type = QueryType::BOOL_QUERY;
            p += 4;
            if (p < line_end && *p == ' ') p++;

            while (p < line_end) {
                const char* t_start = p;
                while (p < line_end && *p != ' ') p++;
                int32_t tid = vocab.get_or_insert(t_start, p - t_start);
                q.bool_q.term_ids.push_back(tid);

                if (p >= line_end) break;
                p++; // skip space

                if (line_end - p >= 3 && std::memcmp(p, "AND", 3) == 0) {
                    q.bool_q.ops.push_back(BoolOp::AND);
                    p += 3;
                } else if (line_end - p >= 2 && std::memcmp(p, "OR", 2) == 0) {
                    q.bool_q.ops.push_back(BoolOp::OR);
                    p += 2;
                } else if (line_end - p >= 3 && std::memcmp(p, "NOT", 3) == 0) {
                    q.bool_q.ops.push_back(BoolOp::NOT);
                    p += 3;
                }
                if (p < line_end && *p == ' ') p++;
            }
        } else if (line_end - p >= 6 && std::memcmp(p, "PHRASE", 6) == 0) {
            q.type = QueryType::PHRASE_QUERY;
            p += 6;
            while (p < line_end && *p != '"') p++;
            if (p < line_end && *p == '"') p++;

            while (p < line_end && *p != '"') {
                while (p < line_end && *p == ' ') p++;
                if (p >= line_end || *p == '"') break;

                const char* t_start = p;
                while (p < line_end && *p != ' ' && *p != '"') p++;
                int32_t tid = vocab.get_or_insert(t_start, p - t_start);
                q.phrase_q.term_ids.push_back(tid);
                q.phrase_q.raw_terms.emplace_back(t_start, p - t_start);
            }
        }
        queries.push_back(std::move(q));
    }

    size_t num_terms = vocab.size();

    // 2. Map docs.txt and count documents
    MappedFile docs_file;
    if (!docs_file.open_and_map(docs_path)) {
        std::cerr << "Error opening " << docs_path << std::endl;
        return 1;
    }

    std::vector<DocRef> docs;
    docs.reserve(262144);

    const char* d_ptr = docs_file.data;
    const char* d_end = docs_file.data + docs_file.size;

    while (d_ptr < d_end) {
        while (d_ptr < d_end && *d_ptr != '\t' && *d_ptr != '\n') d_ptr++;
        if (d_ptr >= d_end) break;
        if (*d_ptr == '\n') {
            docs.push_back({nullptr, 0});
            d_ptr++;
            continue;
        }
        d_ptr++; // skip '\t'

        const char* text_start = d_ptr;
        while (d_ptr < d_end && *d_ptr != '\n') d_ptr++;
        const char* text_end = d_ptr;
        if (d_ptr < d_end && *d_ptr == '\n') d_ptr++;

        docs.push_back({text_start, static_cast<uint32_t>(text_end - text_start)});
    }

    size_t num_docs = docs.size();
    size_t num_words = (num_docs + 63) / 64;

    // 3. Allocate bitset inverted index: num_terms x num_words
    // Each term gets a flat bitset of length num_words uint64_t
    std::vector<uint64_t> postings(num_terms * num_words, 0);

    // Ingest corpus: tokenize and set bits for terms in vocabulary
    char token_buf[64];

    for (uint32_t doc_id = 0; doc_id < num_docs; ++doc_id) {
        const char* ptr = docs[doc_id].text_start;
        const char* end = ptr + docs[doc_id].text_len;
        if (!ptr) continue;

        size_t word_idx = doc_id / 64;
        uint64_t bit_mask = 1ULL << (doc_id % 64);

        while (ptr < end) {
            while (ptr < end && !IS_TOKEN_CHAR[static_cast<uint8_t>(*ptr)]) {
                ptr++;
            }
            if (ptr >= end) break;

            size_t t_len = 0;
            while (ptr < end && IS_TOKEN_CHAR[static_cast<uint8_t>(*ptr)]) {
                if (t_len < sizeof(token_buf)) {
                    token_buf[t_len++] = TOKEN_LOWER[static_cast<uint8_t>(*ptr)];
                }
                ptr++;
            }

            int32_t tid = vocab.lookup(token_buf, t_len);
            if (tid >= 0) {
                postings[static_cast<size_t>(tid) * num_words + word_idx] |= bit_mask;
            }
        }
    }

    // 4. Answer all queries
    FILE* out = fopen(out_path.c_str(), "w");
    if (!out) {
        std::cerr << "Error opening " << out_path << std::endl;
        return 1;
    }

    std::vector<char> write_buf(1024 * 1024);
    setvbuf(out, write_buf.data(), _IOFBF, write_buf.size());

    std::vector<uint64_t> running_set(num_words);
    std::vector<std::string> doc_tokens;
    doc_tokens.reserve(1024);

    for (const auto& q : queries) {
        if (q.type == QueryType::BOOL_QUERY) {
            const auto& terms = q.bool_q.term_ids;
            const auto& ops = q.bool_q.ops;

            if (terms.empty()) {
                std::fputs("0 0\n", out);
                continue;
            }

            // Initialize running_set with first term
            int32_t t0 = terms[0];
            const uint64_t* t0_bits = (t0 >= 0) ? &postings[static_cast<size_t>(t0) * num_words] : nullptr;

            if (t0_bits) {
                std::memcpy(running_set.data(), t0_bits, num_words * sizeof(uint64_t));
            } else {
                std::memset(running_set.data(), 0, num_words * sizeof(uint64_t));
            }

            // Apply operations strictly left-to-right
            for (size_t i = 0; i < ops.size(); ++i) {
                BoolOp op = ops[i];
                int32_t ti = terms[i + 1];
                const uint64_t* ti_bits = (ti >= 0) ? &postings[static_cast<size_t>(ti) * num_words] : nullptr;

                if (op == BoolOp::AND) {
                    if (ti_bits) {
                        for (size_t w = 0; w < num_words; ++w) {
                            running_set[w] &= ti_bits[w];
                        }
                    } else {
                        std::memset(running_set.data(), 0, num_words * sizeof(uint64_t));
                    }
                } else if (op == BoolOp::OR) {
                    if (ti_bits) {
                        for (size_t w = 0; w < num_words; ++w) {
                            running_set[w] |= ti_bits[w];
                        }
                    }
                } else if (op == BoolOp::NOT) {
                    if (ti_bits) {
                        for (size_t w = 0; w < num_words; ++w) {
                            running_set[w] &= ~ti_bits[w];
                        }
                    }
                }
            }

            // Extract count, sum, first 10 IDs
            uint32_t count = 0;
            uint64_t sum = 0;
            uint32_t first10[10];
            uint32_t num_first10 = 0;

            for (size_t w = 0; w < num_words; ++w) {
                uint64_t word = running_set[w];
                if (word == 0) continue;
                count += __builtin_popcountll(word);

                uint32_t base_id = static_cast<uint32_t>(w * 64);
                while (word != 0) {
                    int ctz = __builtin_ctzll(word);
                    uint32_t doc_id = base_id + static_cast<uint32_t>(ctz);
                    sum += doc_id;
                    if (num_first10 < 10) {
                        first10[num_first10++] = doc_id;
                    }
                    word &= word - 1;
                }
            }

            if (count == 0) {
                std::fputs("0 0\n", out);
            } else {
                std::fprintf(out, "%u %llu", count, static_cast<unsigned long long>(sum));
                for (uint32_t j = 0; j < num_first10; ++j) {
                    std::fprintf(out, " %u", first10[j]);
                }
                std::fputc('\n', out);
            }
        } else {
            // PHRASE query
            const auto& term_ids = q.phrase_q.term_ids;
            const auto& raw_terms = q.phrase_q.raw_terms;
            size_t k = raw_terms.size();

            bool impossible = false;
            for (int32_t tid : term_ids) {
                if (tid < 0) {
                    impossible = true;
                    break;
                }
            }

            if (impossible || k == 0) {
                std::fputs("0 0\n", out);
                continue;
            }

            // Intersect all phrase terms to get candidates
            const uint64_t* t0_bits = &postings[static_cast<size_t>(term_ids[0]) * num_words];
            std::memcpy(running_set.data(), t0_bits, num_words * sizeof(uint64_t));

            for (size_t i = 1; i < k; ++i) {
                const uint64_t* ti_bits = &postings[static_cast<size_t>(term_ids[i]) * num_words];
                for (size_t w = 0; w < num_words; ++w) {
                    running_set[w] &= ti_bits[w];
                }
            }

            // Verify candidates by scanning document token sequence
            uint32_t count = 0;
            uint64_t sum = 0;
            uint32_t first10[10];
            uint32_t num_first10 = 0;

            for (size_t w = 0; w < num_words; ++w) {
                uint64_t word = running_set[w];
                if (word == 0) continue;

                uint32_t base_id = static_cast<uint32_t>(w * 64);
                while (word != 0) {
                    int ctz = __builtin_ctzll(word);
                    uint32_t doc_id = base_id + static_cast<uint32_t>(ctz);

                    // Scan doc_id for exact consecutive tokens
                    const char* p = docs[doc_id].text_start;
                    const char* p_end = p + docs[doc_id].text_len;

                    doc_tokens.clear();
                    while (p < p_end) {
                        while (p < p_end && !IS_TOKEN_CHAR[static_cast<uint8_t>(*p)]) p++;
                        if (p >= p_end) break;

                        size_t t_len = 0;
                        while (p < p_end && IS_TOKEN_CHAR[static_cast<uint8_t>(*p)]) {
                            if (t_len < sizeof(token_buf)) {
                                token_buf[t_len++] = TOKEN_LOWER[static_cast<uint8_t>(*p)];
                            }
                            p++;
                        }
                        doc_tokens.emplace_back(token_buf, t_len);
                    }

                    // Check for consecutive match
                    bool matched = false;
                    if (doc_tokens.size() >= k) {
                        for (size_t i = 0; i <= doc_tokens.size() - k; ++i) {
                            bool seq_ok = true;
                            for (size_t m = 0; m < k; ++m) {
                                if (doc_tokens[i + m] != raw_terms[m]) {
                                    seq_ok = false;
                                    break;
                                }
                            }
                            if (seq_ok) {
                                matched = true;
                                break;
                            }
                        }
                    }

                    if (matched) {
                        count++;
                        sum += doc_id;
                        if (num_first10 < 10) {
                            first10[num_first10++] = doc_id;
                        }
                    }

                    word &= word - 1;
                }
            }

            if (count == 0) {
                std::fputs("0 0\n", out);
            } else {
                std::fprintf(out, "%u %llu", count, static_cast<unsigned long long>(sum));
                for (uint32_t j = 0; j < num_first10; ++j) {
                    std::fprintf(out, " %u", first10[j]);
                }
                std::fputc('\n', out);
            }
        }
    }

    std::fclose(out);

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> total_elapsed = end_time - start_time;

    std::cout << "\n=== Track B Complete Pipeline Results ===" << std::endl;
    std::cout << "Documents:          " << num_docs << std::endl;
    std::cout << "Query Terms Indexed:" << num_terms << std::endl;
    std::cout << "Queries Answered:   " << queries.size() << std::endl;
    std::cout << "Total Elapsed Time: " << total_elapsed.count() << " ms (" 
              << (total_elapsed.count() / 1000.0) << " s)" << std::endl;
    std::cout << "Output File:        " << out_path << std::endl;

    return 0;
}
