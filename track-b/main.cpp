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
#include <cstdio>

// Speed Demon 2026 - Track B: Index
static uint8_t TOKEN_MAP[256];
void init_token_tables() {
    for (int i = 0; i < 256; ++i) {
        TOKEN_MAP[i] = (i >= 'a' && i <= 'z') ? (uint8_t)i :
                       (i >= 'A' && i <= 'Z') ? (uint8_t)(i + 32) :
                       (i >= '0' && i <= '9') ? (uint8_t)i : 0;
    }
}

static constexpr size_t VOCAB_CAPACITY = 1048576, BLOOM_SIZE = 4096;
struct VocabEntry { const char* str = nullptr; uint32_t len = 0, hash = 0; int32_t id = -1; };

class Vocabulary {
public:
    std::vector<VocabEntry> table;
    std::vector<uint64_t> bloom;
    uint32_t count = 0;
    Vocabulary() : table(VOCAB_CAPACITY), bloom(BLOOM_SIZE, 0) {}

    int32_t get_or_insert(const char* data, uint32_t len) {
        uint64_t h = 14695981039346656037ULL;
        for (uint32_t i = 0; i < len; ++i) { h ^= (uint8_t)data[i]; h *= 1099511628211ULL; }
        uint32_t hash32 = (uint32_t)h;
        bloom[(hash32 >> 6) & (BLOOM_SIZE - 1)] |= (1ULL << (hash32 & 63));
        size_t idx = h & (VOCAB_CAPACITY - 1);
        while (table[idx].str != nullptr) {
            if (table[idx].hash == hash32 && table[idx].len == len && memcmp(table[idx].str, data, len) == 0) return table[idx].id;
            idx = (idx + 1) & (VOCAB_CAPACITY - 1);
        }
        table[idx].str = data; table[idx].len = len; table[idx].hash = hash32; table[idx].id = count++;
        return table[idx].id;
    }

    int32_t lookup(const char* data, uint32_t len, uint32_t hash32) const {
        if (!((bloom[(hash32 >> 6) & (BLOOM_SIZE - 1)] >> (hash32 & 63)) & 1ULL)) return -1;
        size_t idx = hash32 & (VOCAB_CAPACITY - 1);
        while (table[idx].str != nullptr) {
            if (table[idx].hash == hash32 && table[idx].len == len && memcmp(table[idx].str, data, len) == 0) return table[idx].id;
            idx = (idx + 1) & (VOCAB_CAPACITY - 1);
        }
        return -1;
    }
};

struct DocRef { const char* text_start = nullptr; uint32_t text_len = 0; };
enum class QueryType : uint8_t { BOOL_QUERY, PHRASE_QUERY }; enum class BoolOp : uint8_t { AND, OR, NOT };
struct BoolQuery { std::vector<int32_t> term_ids; std::vector<BoolOp> ops; };
struct PhraseQuery { std::vector<int32_t> term_ids; std::vector<std::string> raw_terms; };
struct Query { QueryType type; BoolQuery bool_q; PhraseQuery phrase_q; };

struct FormattedResult { char buf[160]; uint16_t len; };
static inline char* write_u64(char* p, uint64_t val) {
    if (!val) { *p++ = '0'; return p; }
    char buf[24]; int i = 0; while (val > 0) { buf[i++] = '0' + (val % 10); val /= 10; }
    while (i > 0) *p++ = buf[--i];
    return p;
}
static inline void record_result(FormattedResult& r, uint32_t count, uint64_t sum, const uint32_t* first10, uint32_t num10) {
    if (!count) { memcpy(r.buf, "0 0\n", 4); r.len = 4; return; }
    char* p = r.buf; p = write_u64(p, count); *p++ = ' '; p = write_u64(p, sum);
    for (uint32_t i = 0; i < num10; ++i) { *p++ = ' '; p = write_u64(p, first10[i]); }
    *p++ = '\n'; r.len = (uint16_t)(p - r.buf);
}

static inline bool token_match(const char* s, size_t len, const std::string& t) {
    if (len != t.size()) return false;
    for (size_t i = 0; i < len; ++i) if (TOKEN_MAP[(uint8_t)s[i]] != (uint8_t)t[i]) return false;
    return true;
}
static inline bool doc_contains_phrase(const char* ptr, const char* end, const std::vector<std::string>& terms) {
    size_t k = terms.size(); if (!k) return true;
    while (ptr < end) {
        while (ptr < end && !TOKEN_MAP[(uint8_t)*ptr]) ptr++;
        if (ptr >= end) break;
        const char* t_start = ptr;
        while (ptr < end && TOKEN_MAP[(uint8_t)*ptr]) ptr++;
        if (token_match(t_start, ptr - t_start, terms[0])) {
            const char* sub = ptr; bool full = true;
            for (size_t m = 1; m < k; ++m) {
                while (sub < end && !TOKEN_MAP[(uint8_t)*sub]) sub++;
                if (sub >= end) { full = false; break; }
                const char* st = sub;
                while (sub < end && TOKEN_MAP[(uint8_t)*sub]) sub++;
                if (!token_match(st, sub - st, terms[m])) { full = false; break; }
            }
            if (full) return true;
        }
    }
    return false;
}

std::string resolve_output_path(const std::string& out_dir) {
    struct stat s;
    return (stat(out_dir.c_str(), &s) == 0 && S_ISDIR(s.st_mode)) ? out_dir + "/results.txt" : out_dir;
}

int main(int argc, char** argv) {
    std::string data_dir = (argc > 1) ? argv[1] : "/data";
    std::string out_path = resolve_output_path((argc > 2) ? argv[2] : "/out/results.txt");
    init_token_tables();

    // 1. Ingest queries.txt into Vocabulary
    int q_fd = open((data_dir + "/queries.txt").c_str(), O_RDONLY);
    struct stat q_st; fstat(q_fd, &q_st);
    size_t q_size = q_st.st_size;
    const char* q_data = (const char*)mmap(nullptr, q_size, PROT_READ, MAP_PRIVATE, q_fd, 0);

    Vocabulary vocab;
    std::vector<Query> queries; queries.reserve(65536);
    const char* q_ptr = q_data, *q_end = q_data + q_size;

    while (q_ptr < q_end) {
        const char* line_start = q_ptr;
        while (q_ptr < q_end && *q_ptr != '\n') q_ptr++;
        const char* line_end = q_ptr;
        if (q_ptr < q_end && *q_ptr == '\n') q_ptr++;
        while (line_end > line_start && (*(line_end - 1) == '\r' || *(line_end - 1) == ' ')) line_end--;
        if (line_end <= line_start) continue;

        Query q; const char* p = line_start;
        if (line_end - p >= 4 && memcmp(p, "BOOL", 4) == 0) {
            q.type = QueryType::BOOL_QUERY; p += 4;
            if (p < line_end && *p == ' ') p++;
            while (p < line_end) {
                const char* t_start = p;
                while (p < line_end && *p != ' ') p++;
                size_t t_len = p - t_start;
                if (p < line_end && *p == ' ') p++;
                if (t_len == 3 && memcmp(t_start, "AND", 3) == 0) q.bool_q.ops.push_back(BoolOp::AND);
                else if (t_len == 2 && memcmp(t_start, "OR", 2) == 0) q.bool_q.ops.push_back(BoolOp::OR);
                else if (t_len == 3 && memcmp(t_start, "NOT", 3) == 0) q.bool_q.ops.push_back(BoolOp::NOT);
                else q.bool_q.term_ids.push_back(vocab.get_or_insert(t_start, (uint32_t)t_len));
            }
        } else if (line_end - p >= 6 && memcmp(p, "PHRASE", 6) == 0) {
            q.type = QueryType::PHRASE_QUERY; p += 6;
            while (p < line_end && *p != '\"') p++;
            if (p < line_end && *p == '\"') p++;
            while (p < line_end && *p != '\"') {
                while (p < line_end && *p == ' ') p++;
                if (p >= line_end || *p == '\"') break;
                const char* t_start = p;
                while (p < line_end && *p != ' ' && *p != '\"') p++;
                size_t t_len = p - t_start;
                q.phrase_q.term_ids.push_back(vocab.get_or_insert(t_start, (uint32_t)t_len));
                std::string raw(t_len, ' '); for (size_t i = 0; i < t_len; ++i) raw[i] = (char)TOKEN_MAP[(uint8_t)t_start[i]];
                q.phrase_q.raw_terms.push_back(std::move(raw));
            }
        }
        queries.push_back(std::move(q));
    }

    // 2. Map docs.txt
    int d_fd = open((data_dir + "/docs.txt").c_str(), O_RDONLY);
    struct stat d_st; fstat(d_fd, &d_st);
    size_t d_size = d_st.st_size;
    const char* d_data = (const char*)mmap(nullptr, d_size, PROT_READ, MAP_PRIVATE, d_fd, 0);

    unsigned int num_threads = std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
    size_t max_docs = std::max((size_t)500000, (size_t)(d_size / 30));
    std::vector<DocRef> docs(max_docs);
    std::atomic<uint32_t> global_max_doc{0};

    // SINGLE-PASS PARALLEL FUSED INGESTION & INDEXING
    struct ThreadIndex {
        std::vector<uint32_t> term_counts;
        std::vector<uint64_t> entries; // (tid << 32) | doc_id
    };
    std::vector<ThreadIndex> thread_indexes(num_threads);
    for (unsigned int i = 0; i < num_threads; ++i) {
        thread_indexes[i].term_counts.resize(vocab.count, 0);
        thread_indexes[i].entries.reserve(262144);
    }

    std::vector<std::thread> workers;
    for (unsigned int i = 0; i < num_threads; ++i) {
        size_t b_start = i * (d_size / num_threads);
        size_t b_end = (i == num_threads - 1) ? d_size : (i + 1) * (d_size / num_threads);
        workers.emplace_back([&, i, b_start, b_end]() {
            const char* ptr = d_data + b_start, *chunk_end = d_data + b_end;
            if (i > 0 && d_data[b_start - 1] != '\n') {
                while (ptr < d_data + d_size && *ptr != '\n') ptr++;
                if (ptr < d_data + d_size && *ptr == '\n') ptr++;
            }
            std::vector<uint32_t> last_seen(vocab.count, UINT32_MAX);
            auto& counts = thread_indexes[i].term_counts; auto& entries = thread_indexes[i].entries;
            uint32_t local_max_doc = 0;
            while (ptr < chunk_end) {
                uint32_t d = 0;
                while (ptr < d_data + d_size && *ptr >= '0' && *ptr <= '9') d = d * 10 + (*ptr++ - '0');
                if (ptr < d_data + d_size && *ptr == '\t') ptr++;
                const char* t_start = ptr;
                while (ptr < d_data + d_size && *ptr != '\n') ptr++;
                const char* t_end = ptr;
                if (ptr < d_data + d_size && *ptr == '\n') ptr++;
                if (d < docs.size()) docs[d] = {t_start, (uint32_t)(t_end - t_start)};
                if (d > local_max_doc) local_max_doc = d;
                const char* w_ptr = t_start;
                while (w_ptr < t_end) {
                    while (w_ptr < t_end && !TOKEN_MAP[(uint8_t)*w_ptr]) w_ptr++;
                    if (w_ptr >= t_end) break;
                    char token[64]; uint32_t t_len = 0; uint64_t h = 14695981039346656037ULL;
                    while (w_ptr < t_end) {
                        uint8_t c = TOKEN_MAP[(uint8_t)*w_ptr]; if (!c) break;
                        w_ptr++; if (t_len < 64) token[t_len++] = (char)c;
                        h = (h ^ c) * 1099511628211ULL;
                    }
                    int32_t tid = vocab.lookup(token, t_len, (uint32_t)h);
                    if (tid >= 0 && last_seen[tid] != d) {
                        last_seen[tid] = d; counts[tid]++;
                        entries.push_back(((uint64_t)tid << 32) | (uint32_t)d);
                    }
                }
            }
            uint32_t cur_max = global_max_doc.load(std::memory_order_relaxed);
            while (local_max_doc > cur_max && !global_max_doc.compare_exchange_weak(cur_max, local_max_doc));
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    size_t num_docs = global_max_doc.load() + 1, num_words = (num_docs + 63) / 64;

    // CSR Head computation
    std::vector<uint32_t> head(vocab.count + 1, 0);
    for (uint32_t t = 0; t < vocab.count; ++t) {
        uint32_t total_t = 0;
        for (unsigned int th = 0; th < num_threads; ++th) total_t += thread_indexes[th].term_counts[t];
        head[t + 1] = head[t] + total_t;
    }
    std::vector<uint32_t> flat_postings(head[vocab.count]);

    // Fast scatter into flat_postings
    std::vector<uint32_t> current_offsets = head;
    for (unsigned int th = 0; th < num_threads; ++th) {
        for (uint64_t entry : thread_indexes[th].entries) {
            uint32_t tid = (uint32_t)(entry >> 32);
            uint32_t d = (uint32_t)(entry & 0xFFFFFFFFULL);
            flat_postings[current_offsets[tid]++] = d;
        }
        thread_indexes[th].entries.clear();
        thread_indexes[th].entries.shrink_to_fit();
    }
    thread_indexes.clear();
    thread_indexes.shrink_to_fit();

    // 3. Multi-threaded Query Answering
    std::vector<FormattedResult> query_results(queries.size());
    std::atomic<size_t> next_query{0};

    auto query_worker = [&]() {
        std::vector<uint64_t> running(num_words, 0);
        std::vector<uint64_t> temp_bits(num_words, 0);
        std::vector<uint32_t> surviving; surviving.reserve(4096);

        while (true) {
            size_t qi = next_query.fetch_add(1, std::memory_order_relaxed);
            if (qi >= queries.size()) break;
            const auto& q = queries[qi];
            if (q.type == QueryType::BOOL_QUERY) {
                const auto& terms = q.bool_q.term_ids; const auto& ops = q.bool_q.ops;
                if (terms.empty()) { record_result(query_results[qi], 0, 0, nullptr, 0); continue; }
                uint32_t max_w = 0;
                int32_t t0 = terms[0];
                if (t0 >= 0 && head[t0] < head[t0 + 1]) {
                    uint32_t s0 = head[t0], e0 = head[t0 + 1];
                    max_w = (flat_postings[e0 - 1] >> 6) + 1;
                    for (uint32_t i = s0; i < e0; ++i) { uint32_t d = flat_postings[i]; running[d >> 6] |= (1ULL << (d & 63)); }
                }
                for (size_t op_i = 0; op_i < ops.size(); ++op_i) {
                    BoolOp op = ops[op_i]; int32_t ti = terms[op_i + 1];
                    if (ti < 0 || head[ti] == head[ti + 1]) {
                        if (op == BoolOp::AND) { memset(running.data(), 0, max_w * sizeof(uint64_t)); max_w = 0; }
                        continue;
                    }
                    uint32_t si = head[ti], ei = head[ti + 1], term_df = ei - si;
                    if (op == BoolOp::OR) {
                        max_w = std::max(max_w, (flat_postings[ei - 1] >> 6) + 1);
                        for (uint32_t i = si; i < ei; ++i) { uint32_t d = flat_postings[i]; running[d >> 6] |= (1ULL << (d & 63)); }
                    } else if (op == BoolOp::NOT) {
                        for (uint32_t i = si; i < ei; ++i) { uint32_t d = flat_postings[i]; running[d >> 6] &= ~(1ULL << (d & 63)); }
                    } else if (op == BoolOp::AND) {
                        if (term_df < 1000) {
                            surviving.clear();
                            for (uint32_t i = si; i < ei; ++i) {
                                uint32_t d = flat_postings[i];
                                if ((running[d >> 6] >> (d & 63)) & 1ULL) surviving.push_back(d);
                            }
                            memset(running.data(), 0, max_w * sizeof(uint64_t));
                            max_w = 0;
                            for (uint32_t d : surviving) { running[d >> 6] |= (1ULL << (d & 63)); max_w = std::max(max_w, (d >> 6) + 1); }
                        } else {
                            memset(temp_bits.data(), 0, max_w * sizeof(uint64_t));
                            for (uint32_t i = si; i < ei; ++i) {
                                uint32_t d = flat_postings[i];
                                if ((d >> 6) < max_w) temp_bits[d >> 6] |= (1ULL << (d & 63));
                            }
                            for (size_t w = 0; w < max_w; ++w) running[w] &= temp_bits[w];
                        }
                    }
                }
                uint32_t count = 0; uint64_t sum = 0;
                uint32_t first10[10]; uint32_t num_first10 = 0;
                for (size_t w = 0; w < max_w; ++w) {
                    uint64_t word = running[w]; if (!word) continue;
                    count += __builtin_popcountll(word);
                    uint32_t base = w * 64;
                    while (word) {
                        int ctz = __builtin_ctzll(word);
                        uint32_t d = base + ctz;
                        sum += d;
                        if (num_first10 < 10) first10[num_first10++] = d;
                        word &= word - 1;
                    }
                }
                memset(running.data(), 0, max_w * sizeof(uint64_t));
                record_result(query_results[qi], count, sum, first10, num_first10);
            } else {
                // PHRASE QUERY (Anchor by min DF + In-place zero-allocation verification)
                const auto& phrase_terms = q.phrase_q.term_ids;
                const auto& raw_terms = q.phrase_q.raw_terms;
                size_t phrase_len = phrase_terms.size();
                bool possible = true;
                for (int32_t tid : phrase_terms) {
                    if (tid < 0 || head[tid] == head[tid + 1]) { possible = false; break; }
                }
                if (!possible) { record_result(query_results[qi], 0, 0, nullptr, 0); continue; }

                // Find anchor with min DF
                int32_t min_tid = phrase_terms[0]; uint32_t min_df = head[min_tid + 1] - head[min_tid];
                for (size_t i = 1; i < phrase_len; ++i) {
                    int32_t tid = phrase_terms[i]; uint32_t df_i = head[tid + 1] - head[tid];
                    if (df_i < min_df) { min_df = df_i; min_tid = tid; }
                }
                int32_t other_terms[16]; uint32_t num_other = 0;
                for (int32_t tid : phrase_terms) if (tid != min_tid) other_terms[num_other++] = tid;
                std::sort(other_terms, other_terms + num_other, [&](int32_t a, int32_t b) {
                    return (head[a + 1] - head[a]) < (head[b + 1] - head[b]);
                });
                uint32_t cursors[16]; for (size_t m = 0; m < num_other; ++m) cursors[m] = head[other_terms[m]];
                uint32_t s_min = head[min_tid], e_min = head[min_tid + 1], count = 0, num_first10 = 0;
                uint64_t sum = 0; uint32_t first10[10];

                for (uint32_t i = s_min; i < e_min; ++i) {
                    uint32_t d = flat_postings[i];
                    bool doc_has_all = true;
                    for (size_t m = 0; m < num_other; ++m) {
                        uint32_t p = cursors[m], lim = head[other_terms[m] + 1];
                        while (p < lim && flat_postings[p] < d) ++p;
                        cursors[m] = p;
                        if (p >= lim || flat_postings[p] != d) { doc_has_all = false; break; }
                    }
                    if (!doc_has_all) continue;
                    if (doc_contains_phrase(docs[d].text_start, docs[d].text_start + docs[d].text_len, raw_terms)) {
                        count++; sum += d;
                        if (num_first10 < 10) first10[num_first10++] = d;
                    }
                }
                record_result(query_results[qi], count, sum, first10, num_first10);
            }
        }
    };

    for (unsigned int i = 0; i < num_threads; ++i) workers.emplace_back(query_worker);
    for (auto& w : workers) w.join();

    // 4. Output write via batch buffered I/O
    int out_fd = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd < 0) out_fd = open("results.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd >= 0) {
        std::vector<char> io_buf(1024 * 1024); size_t io_pos = 0;
        for (const auto& r : query_results) {
            if (io_pos + r.len > io_buf.size()) { write(out_fd, io_buf.data(), io_pos); io_pos = 0; }
            memcpy(io_buf.data() + io_pos, r.buf, r.len);
            io_pos += r.len;
        }
        if (io_pos > 0) write(out_fd, io_buf.data(), io_pos);
        close(out_fd);
    }

    munmap((void*)q_data, q_size);
    munmap((void*)d_data, d_size);
    return 0;
}
