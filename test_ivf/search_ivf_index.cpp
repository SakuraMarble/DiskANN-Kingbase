// search_ivf_index.cpp
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <stack>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <immintrin.h> // AVX2
#include <omp.h>       // OpenMP

#include "InvertedIndex.hpp"

// ==========================================
// 1. AVX2 距离计算核心 (L2 Euclidean)
// ==========================================
inline float L2_AVX2(const float* a, const float* b, size_t dim) {
    __m256 sum = _mm256_setzero_ps();
    size_t i = 0;
    // 主循环：每次处理 8 个 float
    for (; i + 7 < dim; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        __m256 diff = _mm256_sub_ps(va, vb);
        sum = _mm256_fmadd_ps(diff, diff, sum);
    }
    // 水平求和
    __m128 sum_high = _mm256_extractf128_ps(sum, 1);
    __m128 sum_low = _mm256_castps256_ps128(sum);
    __m128 sum128 = _mm_add_ps(sum_low, sum_high);
    __m128 shuf = _mm_movehdup_ps(sum128);
    __m128 sums = _mm_add_ps(sum128, shuf);
    shuf = _mm_movehl_ps(shuf, sums);
    sums = _mm_add_ps(sums, shuf);
    float total = _mm_cvtss_f32(sums);
    // 处理尾部
    for (; i < dim; ++i) {
        float diff = a[i] - b[i];
        total += diff * diff;
    }
    return total;
}

// ==========================================
// 2. 表达式解析器 (Shunting-yard)
// ==========================================
class ExpressionParser {
public:
    explicit ExpressionParser(InvertedIndexSearcher& index) : _index(index) {}

    roaring::Roaring parse(const std::string& expression) {
        std::string rpn = to_rpn(expression);
        return evaluate_rpn(rpn);
    }

private:
    InvertedIndexSearcher& _index;

    int precedence(char op) {
        if (op == '&') return 2;
        if (op == '|') return 1;
        if (op == '-') return 2;
        return 0;
    }

    std::string to_rpn(const std::string& exp) {
        std::string output;
        std::stack<char> ops;
        for (size_t i = 0; i < exp.length(); ++i) {
            char c = exp[i];
            if (isspace(c)) continue;
            if (isdigit(c)) {
                while (i < exp.length() && isdigit(exp[i])) {
                    output += exp[i]; i++;
                }
                output += ' '; i--;
            } else if (c == '(') {
                ops.push(c);
            } else if (c == ')') {
                while (!ops.empty() && ops.top() != '(') {
                    output += ops.top(); output += ' '; ops.pop();
                }
                if (!ops.empty()) ops.pop();
            } else if (c == '&' || c == '|' || c == '-') {
                while (!ops.empty() && precedence(ops.top()) >= precedence(c)) {
                    output += ops.top(); output += ' '; ops.pop();
                }
                ops.push(c);
            }
        }
        while (!ops.empty()) {
            output += ops.top(); output += ' '; ops.pop();
        }
        return output;
    }

    roaring::Roaring evaluate_rpn(const std::string& rpn) {
        std::stack<roaring::Roaring> stack;
        std::stringstream ss(rpn);
        std::string token;
        while (ss >> token) {
            if (isdigit(token[0])) {
                // 注意：query_bitmap 返回的是 (Tag U Universal)
                stack.push(_index.query_bitmap({std::stoi(token)}));
            } else {
                if (stack.size() < 2) throw std::runtime_error("Invalid expression logic");
                roaring::Roaring b = stack.top(); stack.pop();
                roaring::Roaring a = stack.top(); stack.pop();
                if (token == "&") a &= b;
                else if (token == "|") a |= b;
                else if (token == "-") a -= b;
                stack.push(a);
            }
        }
        // 如果表达式为空，或者栈里有结果，最后都需要确保 Universal 集合被包含
        // 这里的 query_bitmap({}) 返回的就是 Universal 集合
        roaring::Roaring result = stack.empty() ? roaring::Roaring() : stack.top();
        result |= _index.query_bitmap({}); 
        return result;
    }
};

// ==========================================
// 3. 辅助：向量加载器
// ==========================================
std::vector<float> load_vectors(const std::string& bin_path, uint32_t& dim, uint32_t& num) {
    std::ifstream in(bin_path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open vector file: " + bin_path);
    in.read(reinterpret_cast<char*>(&num), sizeof(num));
    in.read(reinterpret_cast<char*>(&dim), sizeof(dim));
    std::cout << "[Loader] Found " << num << " vectors, Dim = " << dim << std::endl;
    std::vector<float> data((size_t)num * dim);
    in.read(reinterpret_cast<char*>(data.data()), data.size() * sizeof(float));
    return data;
}

struct SearchResult {
    uint32_t id;
    float dist;
    bool operator<(const SearchResult& other) const { return dist < other.dist; }
};

// ==========================================
// 4. Main Search Logic
// ==========================================
int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0] << " <index_bin> <vector_bin> <filter_expr> <top_k>" << std::endl;
        std::cerr << "Example: " << argv[0] << " index.bin data.bin \"(3&5)|7\" 10" << std::endl;
        return 1;
    }

    std::string idx_path = argv[1];
    std::string vec_path = argv[2];
    std::string filter_expr = argv[3];
    int K = std::stoi(argv[4]);

    try {
        // 1. 加载数据
        InvertedIndexSearcher index_searcher;
        index_searcher.load(idx_path);

        uint32_t dim = 0, num_vecs = 0;
        std::vector<float> vectors = load_vectors(vec_path, dim, num_vecs);

        // 创建随机 Query 用于演示
        std::vector<float> query(dim, 0.1f);

        // 2. 解析 Filter
        std::cout << ">>> [Filter] Expr: " << filter_expr << std::endl;
        ExpressionParser parser(index_searcher);
        
        auto t1 = std::chrono::high_resolution_clock::now();
        roaring::Roaring candidates = parser.parse(filter_expr);
        
        // 转为数组供 OpenMP 使用
        std::vector<uint32_t> valid_ids(candidates.cardinality());
        candidates.toUint32Array(valid_ids.data());
        auto t2 = std::chrono::high_resolution_clock::now();
        
        std::cout << ">>> [Filter] Candidates: " << valid_ids.size() 
                  << " (Parsed in " << (t2-t1).count()/1000 << " us)" << std::endl;

        if (valid_ids.empty()) {
            std::cout << "No candidates found." << std::endl;
            return 0;
        }

        // 3. 并行搜索 (AVX2 + OpenMP)
        std::cout << ">>> [Search] Starting OMP+AVX2 scan..." << std::endl;
        std::vector<SearchResult> results(valid_ids.size());

        auto t3 = std::chrono::high_resolution_clock::now();

        #pragma omp parallel for schedule(dynamic, 256)
        for (size_t i = 0; i < valid_ids.size(); ++i) {
            uint32_t vid = valid_ids[i];
            if (vid < num_vecs) {
                float d = L2_AVX2(query.data(), vectors.data() + (size_t)vid * dim, dim);
                results[i] = {vid, d};
            } else {
                results[i] = {vid, 999999.0f}; // 越界保护
            }
        }

        auto t4 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration_cast<std::chrono::microseconds>(t4 - t3).count() / 1000.0;

        std::cout << ">>> [Search] Scan done in " << ms << " ms." << std::endl;

        // 4. Top K 排序
        size_t final_k = std::min((size_t)K, results.size());
        std::partial_sort(results.begin(), results.begin() + final_k, results.end());

        std::cout << ">>> [Result] Top " << final_k << ":" << std::endl;
        for (size_t i = 0; i < final_k; ++i) {
            std::cout << "  Rank " << i+1 << ": ID=" << results[i].id << " Dist=" << results[i].dist << std::endl;
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}