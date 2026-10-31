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

static bool IS_TOKEN_CHAR[256];
static char TOKEN_LOWER[256];

void init_token_tables() {
    for (int i = 0; i < 256; ++i) {
        IS_TOKEN_CHAR[i] = (i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z') || (i >= '0' && i <= '9');
        if (i >= 'A' && i <= 'Z') TOKEN_LOWER[i] = (char)(i + 32);
        else TOKEN_LOWER[i] = (char)i;
    }
}

// Flat open-addressing vocabulary table for query terms
static constexpr size_t VOCAB_CAPACITY = 131072;
struct VocabEntry {
    const char* str = nullptr;
    uint32_t len = 0;
    int32_t id = -1;
};

class Vocabulary {
public:
    VocabEntry table[VOCAB_CAPACITY];
    uint32_t count = 0;

    int32_t get_or_insert(const char* data, uint32_t len) {
        uint64_t h = 14695981039346656037ULL;
        for (uint32_t i = 0; i < len; ++i) {
            h ^= (uint8_t)data[i];
            h *= 1099511628211ULL;
        }
        size_t idx = h & (VOCAB_CAPACITY - 1);
        while (table[idx].str != nullptr) {
            if (table[idx].len == len && memcmp(table[idx].str, data, len) == 0) {
                return table[idx].id;
            }
            idx = (idx + 1) & (VOCAB_CAPACITY - 1);
        }
        table[idx].str = data;
        table[idx].len = len;
        table[idx].id = count++;
        return table[idx].id;
    }

    int32_t lookup(const char* data, uint32_t len) const {
        uint64_t h = 14695981039346656037ULL;
        for (uint32_t i = 0; i < len; ++i) {
            h ^= (uint8_t)data[i];
            h *= 1099511628211ULL;
        }
        size_t idx = h & (VOCAB_CAPACITY - 1);
        while (table[idx].str != nullptr) {
            if (table[idx].len == len && memcmp(table[idx].str, data, len) == 0) {
                return table[idx].id;
            }
            idx = (idx + 1) & (VOCAB_CAPACITY - 1);
        }
        return -1;
    }
};

struct DocRef {
    const char* text_start = nullptr;
    uint32_t text_len = 0;
};

enum class QueryType : uint8_t { BOOL_QUERY, PHRASE_QUERY };
enum class BoolOp : uint8_t { AND, OR, NOT };

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

static inline void append_u64(std::string& out, uint64_t val) {
    if (val == 0) {
        out.push_back('0');
        return;
    }
    char buf[24];
    int p = 0;
    while (val > 0) {
        buf[p++] = '0' + (val % 10);
        val /= 10;
    }
    for (int i = p - 1; i >= 0; --i) out.push_back(buf[i]);
}

int main(int argc, char** argv) {
    std::string data_dir = (argc > 1) ? argv[1] : "/data";
    std::string out_path = (argc > 2) ? argv[2] : "/out/results.txt";
    if (out_path.find(".txt") == std::string::npos) {
        out_path += "/results.txt";
    }

    std::string docs_path = data_dir + "/docs.txt";
    std::string queries_path = data_dir + "/queries.txt";

    init_token_tables();

    // 1. Memory-map queries.txt
    int q_fd = open(queries_path.c_str(), O_RDONLY);
    if (q_fd < 0) { perror("open queries"); return 1; }
    struct stat q_st;
    fstat(q_fd, &q_st);
    size_t q_size = q_st.st_size;
    const char* q_data = (const char*)mmap(nullptr, q_size, PROT_READ, MAP_PRIVATE, q_fd, 0);
    close(q_fd);

    Vocabulary vocab;
    std::vector<Query> queries;
    queries.reserve(65536);

    const char* q_ptr = q_data;
    const char* q_end = q_data + q_size;

    while (q_ptr < q_end) {
        const char* line_start = q_ptr;
        while (q_ptr < q_end && *q_ptr != '\n') q_ptr++;
        const char* line_end = q_ptr;
        if (q_ptr < q_end && *q_ptr == '\n') q_ptr++;

        while (line_end > line_start && (*(line_end - 1) == '\r' || *(line_end - 1) == ' ')) line_end--;
        if (line_end <= line_start) continue;

        Query q;
        const char* p = line_start;
        if (line_end - p >= 4 && memcmp(p, "BOOL", 4) == 0) {
            q.type = QueryType::BOOL_QUERY;
            p += 4;
            if (p < line_end && *p == ' ') p++;
            while (p < line_end) {
                const char* t_start = p;
                while (p < line_end && *p != ' ') p++;
                size_t t_len = p - t_start;
                if (p < line_end && *p == ' ') p++;

                if (t_len == 3 && memcmp(t_start, "AND", 3) == 0) {
                    q.bool_q.ops.push_back(BoolOp::AND);
                } else if (t_len == 2 && memcmp(t_start, "OR", 2) == 0) {
                    q.bool_q.ops.push_back(BoolOp::OR);
                } else if (t_len == 3 && memcmp(t_start, "NOT", 3) == 0) {
                    q.bool_q.ops.push_back(BoolOp::NOT);
                } else {
                    int32_t tid = vocab.get_or_insert(t_start, (uint32_t)t_len);
                    q.bool_q.term_ids.push_back(tid);
                }
            }
        } else if (line_end - p >= 6 && memcmp(p, "PHRASE", 6) == 0) {
            q.type = QueryType::PHRASE_QUERY;
            p += 6;
            while (p < line_end && *p != '\"') p++;
            if (p < line_end && *p == '\"') p++;
            while (p < line_end && *p != '\"') {
                while (p < line_end && *p == ' ') p++;
                if (p >= line_end || *p == '\"') break;
                const char* t_start = p;
                while (p < line_end && *p != ' ' && *p != '\"') p++;
                size_t t_len = p - t_start;
                int32_t tid = vocab.get_or_insert(t_start, (uint32_t)t_len);
                q.phrase_q.term_ids.push_back(tid);
                q.phrase_q.raw_terms.emplace_back(t_start, t_len);
            }
        }
        queries.push_back(std::move(q));
    }

    // 2. Memory-map docs.txt
    int d_fd = open(docs_path.c_str(), O_RDONLY);
    if (d_fd < 0) { perror("open docs"); return 1; }
    struct stat d_st;
    fstat(d_fd, &d_st);
    size_t d_size = d_st.st_size;
    const char* d_data = (const char*)mmap(nullptr, d_size, PROT_READ, MAP_PRIVATE, d_fd, 0);
    close(d_fd);

    std::vector<DocRef> docs;
    docs.reserve(262144);
    const char* d_ptr = d_data;
    const char* d_end = d_data + d_size;
    while (d_ptr < d_end) {
        while (d_ptr < d_end && *d_ptr != '\t' && *d_ptr != '\n') d_ptr++;
        if (d_ptr >= d_end) break;
        if (*d_ptr == '\n') {
            docs.push_back({nullptr, 0});
            d_ptr++;
            continue;
        }
        d_ptr++; // skip tab
        const char* t_start = d_ptr;
        while (d_ptr < d_end && *d_ptr != '\n') d_ptr++;
        const char* t_end = d_ptr;
        if (d_ptr < d_end && *d_ptr == '\n') d_ptr++;
        docs.push_back({t_start, (uint32_t)(t_end - t_start)});
    }
    size_t num_docs = docs.size();
    size_t num_words = (num_docs + 63) / 64;

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;

    // PASS 1: Count DF per term across threads (Zero dynamic allocations!)
    std::vector<std::vector<uint32_t>> thread_df(num_threads, std::vector<uint32_t>(vocab.count, 0));
    std::vector<std::thread> workers;
    size_t chunk_docs = (num_docs + num_threads - 1) / num_threads;

    auto pass1_worker = [&](unsigned int tid_worker, size_t start_doc, size_t end_doc) {
        std::vector<uint32_t> last_seen(vocab.count, UINT32_MAX);
        auto& my_df = thread_df[tid_worker];
        for (size_t d = start_doc; d < end_doc; ++d) {
            const char* ptr = docs[d].text_start;
            const char* end = ptr + docs[d].text_len;
            if (!ptr) continue;

            while (ptr < end) {
                while (ptr < end && !IS_TOKEN_CHAR[(uint8_t)*ptr]) ptr++;
                if (ptr >= end) break;
                char token[64];
                uint32_t t_len = 0;
                while (ptr < end && IS_TOKEN_CHAR[(uint8_t)*ptr]) {
                    if (t_len < 64) token[t_len++] = TOKEN_LOWER[(uint8_t)*ptr];
                    ptr++;
                }
                int32_t tid = vocab.lookup(token, t_len);
                if (tid >= 0 && last_seen[tid] != d) {
                    last_seen[tid] = (uint32_t)d;
                    my_df[tid]++;
                }
            }
        }
    };

    for (unsigned int i = 0; i < num_threads; ++i) {
        size_t s = i * chunk_docs;
        size_t e = std::min(s + chunk_docs, num_docs);
        workers.emplace_back(pass1_worker, i, s, e);
    }
    for (auto& w : workers) w.join();
    workers.clear();

    // Compute CSR offsets
    std::vector<uint32_t> head(vocab.count + 1, 0);
    for (uint32_t t = 0; t < vocab.count; ++t) {
        uint32_t total_t = 0;
        for (unsigned int th = 0; th < num_threads; ++th) total_t += thread_df[th][t];
        head[t + 1] = head[t] + total_t;
    }

    uint32_t total_postings = head[vocab.count];
    std::vector<uint32_t> flat_postings(total_postings);

    // Thread write cursors for parallel pass 2
    std::vector<std::vector<uint32_t>> thread_cur(num_threads, std::vector<uint32_t>(vocab.count, 0));
    for (uint32_t t = 0; t < vocab.count; ++t) {
        uint32_t pos = head[t];
        for (unsigned int th = 0; th < num_threads; ++th) {
            thread_cur[th][t] = pos;
            pos += thread_df[th][t];
        }
    }
    thread_df.clear();

    // PASS 2: Write doc IDs directly into contiguous flat_postings
    auto pass2_worker = [&](unsigned int tid_worker, size_t start_doc, size_t end_doc) {
        std::vector<uint32_t> last_seen(vocab.count, UINT32_MAX);
        auto& my_cur = thread_cur[tid_worker];
        for (size_t d = start_doc; d < end_doc; ++d) {
            const char* ptr = docs[d].text_start;
            const char* end = ptr + docs[d].text_len;
            if (!ptr) continue;

            while (ptr < end) {
                while (ptr < end && !IS_TOKEN_CHAR[(uint8_t)*ptr]) ptr++;
                if (ptr >= end) break;
                char token[64];
                uint32_t t_len = 0;
                while (ptr < end && IS_TOKEN_CHAR[(uint8_t)*ptr]) {
                    if (t_len < 64) token[t_len++] = TOKEN_LOWER[(uint8_t)*ptr];
                    ptr++;
                }
                int32_t tid = vocab.lookup(token, t_len);
                if (tid >= 0 && last_seen[tid] != d) {
                    last_seen[tid] = (uint32_t)d;
                    flat_postings[my_cur[tid]++] = (uint32_t)d;
                }
            }
        }
    };

    for (unsigned int i = 0; i < num_threads; ++i) {
        size_t s = i * chunk_docs;
        size_t e = std::min(s + chunk_docs, num_docs);
        workers.emplace_back(pass2_worker, i, s, e);
    }
    for (auto& w : workers) w.join();
    workers.clear();
    thread_cur.clear();

    // 3. Multi-threaded Query Answering with Fast Output Buffer
    std::vector<std::string> query_results(queries.size());
    std::atomic<size_t> next_query{0};

    auto query_worker = [&]() {
        std::vector<uint64_t> running(num_words, 0);
        std::vector<uint32_t> surviving;
        surviving.reserve(4096);
        std::vector<std::string> doc_tokens;
        doc_tokens.reserve(1024);

        while (true) {
            size_t qi = next_query.fetch_add(1, std::memory_order_relaxed);
            if (qi >= queries.size()) break;

            const auto& q = queries[qi];
            if (q.type == QueryType::BOOL_QUERY) {
                const auto& terms = q.bool_q.term_ids;
                const auto& ops = q.bool_q.ops;

                if (terms.empty()) {
                    query_results[qi] = "0 0\n";
                    continue;
                }

                // Initialize running with first term
                memset(running.data(), 0, num_words * sizeof(uint64_t));
                int32_t t0 = terms[0];
                if (t0 >= 0) {
                    uint32_t s0 = head[t0];
                    uint32_t e0 = head[t0 + 1];
                    for (uint32_t i = s0; i < e0; ++i) {
                        uint32_t d = flat_postings[i];
                        running[d >> 6] |= (1ULL << (d & 63));
                    }
                }

                // Apply operations left-to-right
                for (size_t op_i = 0; op_i < ops.size(); ++op_i) {
                    BoolOp op = ops[op_i];
                    int32_t ti = terms[op_i + 1];

                    if (ti < 0 || head[ti] == head[ti + 1]) {
                        if (op == BoolOp::AND) {
                            memset(running.data(), 0, num_words * sizeof(uint64_t));
                        }
                        continue;
                    }

                    uint32_t si = head[ti];
                    uint32_t ei = head[ti + 1];
                    uint32_t term_df = ei - si;

                    if (op == BoolOp::OR) {
                        for (uint32_t i = si; i < ei; ++i) {
                            uint32_t d = flat_postings[i];
                            running[d >> 6] |= (1ULL << (d & 63));
                        }
                    } else if (op == BoolOp::NOT) {
                        for (uint32_t i = si; i < ei; ++i) {
                            uint32_t d = flat_postings[i];
                            running[d >> 6] &= ~(1ULL << (d & 63));
                        }
                    } else if (op == BoolOp::AND) {
                        if (term_df < 1000) {
                            surviving.clear();
                            for (uint32_t i = si; i < ei; ++i) {
                                uint32_t d = flat_postings[i];
                                if ((running[d >> 6] >> (d & 63)) & 1ULL) {
                                    surviving.push_back(d);
                                }
                            }
                            memset(running.data(), 0, num_words * sizeof(uint64_t));
                            for (uint32_t d : surviving) {
                                running[d >> 6] |= (1ULL << (d & 63));
                            }
                        } else {
                            std::vector<uint64_t> term_bits(num_words, 0);
                            for (uint32_t i = si; i < ei; ++i) {
                                uint32_t d = flat_postings[i];
                                term_bits[d >> 6] |= (1ULL << (d & 63));
                            }
                            for (size_t w = 0; w < num_words; ++w) {
                                running[w] &= term_bits[w];
                            }
                        }
                    }
                }

                // Format count, sum, first 10
                uint32_t count = 0;
                uint64_t sum = 0;
                uint32_t first10[10];
                uint32_t num_first10 = 0;

                for (size_t w = 0; w < num_words; ++w) {
                    uint64_t word = running[w];
                    if (!word) continue;
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

                if (count == 0) {
                    query_results[qi] = "0 0\n";
                } else {
                    std::string res;
                    res.reserve(64);
                    append_u64(res, count);
                    res.push_back(' ');
                    append_u64(res, sum);
                    for (uint32_t i = 0; i < num_first10; ++i) {
                        res.push_back(' ');
                        append_u64(res, first10[i]);
                    }
                    res.push_back('\n');
                    query_results[qi] = std::move(res);
                }

            } else {
                // PHRASE QUERY
                const auto& phrase_terms = q.phrase_q.term_ids;
                const auto& raw_terms = q.phrase_q.raw_terms;
                size_t phrase_len = phrase_terms.size();

                bool possible = true;
                for (int32_t tid : phrase_terms) {
                    if (tid < 0 || head[tid] == head[tid + 1]) {
                        possible = false;
                        break;
                    }
                }

                if (!possible) {
                    query_results[qi] = "0 0\n";
                    continue;
                }

                // Find anchor with min DF
                int32_t min_tid = phrase_terms[0];
                uint32_t min_df = head[min_tid + 1] - head[min_tid];
                for (size_t i = 1; i < phrase_len; ++i) {
                    int32_t tid = phrase_terms[i];
                    uint32_t df_i = head[tid + 1] - head[tid];
                    if (df_i < min_df) {
                        min_df = df_i;
                        min_tid = tid;
                    }
                }

                uint32_t s_min = head[min_tid];
                uint32_t e_min = head[min_tid + 1];

                uint32_t count = 0;
                uint64_t sum = 0;
                uint32_t first10[10];
                uint32_t num_first10 = 0;

                for (uint32_t i = s_min; i < e_min; ++i) {
                    uint32_t d = flat_postings[i];
                    bool doc_has_all = true;
                    for (size_t t = 0; t < phrase_len; ++t) {
                        int32_t tid = phrase_terms[t];
                        if (tid == min_tid) continue;
                        auto start_it = flat_postings.begin() + head[tid];
                        auto end_it = flat_postings.begin() + head[tid + 1];
                        if (!std::binary_search(start_it, end_it, d)) {
                            doc_has_all = false;
                            break;
                        }
                    }
                    if (!doc_has_all) continue;

                    // Verify exact sequence in document
                    const char* ptr = docs[d].text_start;
                    const char* end = ptr + docs[d].text_len;
                    doc_tokens.clear();
                    while (ptr < end) {
                        while (ptr < end && !IS_TOKEN_CHAR[(uint8_t)*ptr]) ptr++;
                        if (ptr >= end) break;
                        const char* ts = ptr;
                        while (ptr < end && IS_TOKEN_CHAR[(uint8_t)*ptr]) ptr++;
                        doc_tokens.emplace_back(ts, ptr - ts);
                    }

                    bool matched = false;
                    if (doc_tokens.size() >= phrase_len) {
                        for (size_t pos = 0; pos <= doc_tokens.size() - phrase_len; ++pos) {
                            bool match_here = true;
                            for (size_t k = 0; k < phrase_len; ++k) {
                                const std::string& dt = doc_tokens[pos + k];
                                const std::string& pt = raw_terms[k];
                                if (dt.length() != pt.length() || strncasecmp(dt.data(), pt.data(), dt.length()) != 0) {
                                    match_here = false;
                                    break;
                                }
                            }
                            if (match_here) {
                                matched = true;
                                break;
                            }
                        }
                    }

                    if (matched) {
                        count++;
                        sum += d;
                        if (num_first10 < 10) first10[num_first10++] = d;
                    }
                }

                if (count == 0) {
                    query_results[qi] = "0 0\n";
                } else {
                    std::string res;
                    res.reserve(64);
                    append_u64(res, count);
                    res.push_back(' ');
                    append_u64(res, sum);
                    for (uint32_t i = 0; i < num_first10; ++i) {
                        res.push_back(' ');
                        append_u64(res, first10[i]);
                    }
                    res.push_back('\n');
                    query_results[qi] = std::move(res);
                }
            }
        }
    };

    workers.clear();
    for (unsigned int i = 0; i < num_threads; ++i) {
        workers.emplace_back(query_worker);
    }
    for (auto& w : workers) w.join();

    // 4. Output write
    int out_fd = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd >= 0) {
        for (const auto& line : query_results) {
            write(out_fd, line.data(), line.length());
        }
        close(out_fd);
    }

    munmap((void*)q_data, q_size);
    munmap((void*)d_data, d_size);
    return 0;
}
