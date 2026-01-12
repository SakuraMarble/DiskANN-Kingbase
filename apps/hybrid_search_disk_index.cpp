// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.
// Modified to support hybrid search with inverted index

#include "common_includes.h"
#include <boost/program_options.hpp>
#include <immintrin.h> // AVX2
#include <stack>
#include <sstream>
#include <cctype>

#include "index.h"
#include "disk_utils.h"
#include "math_utils.h"
#include "memory_mapper.h"
#include "partition.h"
#include "pq_flash_index.h"
#include "timer.h"
#include "percentile_stats.h"
#include "program_options_utils.hpp"

// Include inverted index
#include "../test_ivf/InvertedIndex.hpp"

#ifndef _WINDOWS
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "linux_aligned_file_reader.h"
#else
#ifdef USE_BING_INFRA
#include "bing_aligned_file_reader.h"
#else
#include "windows_aligned_file_reader.h"
#endif
#endif

#define WARMUP false

namespace po = boost::program_options;

// ==========================================
// Expression Parser for label filters
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
        roaring::Roaring result = stack.empty() ? roaring::Roaring() : stack.top();
        result |= _index.query_bitmap({});  // Include universal set
        return result;
    }
};

// ==========================================
// AVX2 Distance Calculation (L2 Euclidean)
// ==========================================
inline float L2_AVX2(const float* a, const float* b, size_t dim) {
    __m256 sum = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 7 < dim; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        __m256 diff = _mm256_sub_ps(va, vb);
        sum = _mm256_fmadd_ps(diff, diff, sum);
    }
    __m128 sum_high = _mm256_extractf128_ps(sum, 1);
    __m128 sum_low = _mm256_castps256_ps128(sum);
    __m128 sum128 = _mm_add_ps(sum_low, sum_high);
    __m128 shuf = _mm_movehdup_ps(sum128);
    __m128 sums = _mm_add_ps(sum128, shuf);
    shuf = _mm_movehl_ps(shuf, sums);
    sums = _mm_add_ps(sums, shuf);
    float total = _mm_cvtss_f32(sums);
    for (; i < dim; ++i) {
        float diff = a[i] - b[i];
        total += diff * diff;
    }
    return total;
}

struct SearchResult {
    uint32_t id;
    float dist;
    bool operator<(const SearchResult& other) const { return dist < other.dist; }
};

// ==========================================
// Load raw vectors from binary file
// ==========================================
std::vector<float> load_raw_vectors(const std::string& bin_path, uint32_t& dim, uint32_t& num) {
    std::ifstream in(bin_path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open raw vector file: " + bin_path);
    in.read(reinterpret_cast<char*>(&num), sizeof(num));
    in.read(reinterpret_cast<char*>(&dim), sizeof(dim));
    diskann::cout << "[RawVectors] Loaded " << num << " vectors, Dim = " << dim << std::endl;
    std::vector<float> data((size_t)num * dim);
    in.read(reinterpret_cast<char*>(data.data()), data.size() * sizeof(float));
    return data;
}

// ==========================================
// IVF-based Brute Force Search
// ==========================================
template <typename T>
void ivf_brute_force_search(const T* query, uint32_t query_dim,
                           const std::vector<uint32_t>& candidate_ids,
                           const std::vector<float>& raw_vectors, uint32_t vec_dim,
                           uint32_t k_search, std::vector<uint32_t>& result_ids,
                           std::vector<float>& result_dists) {
    if (candidate_ids.empty()) {
        result_ids.clear();
        result_dists.clear();
        return;
    }

    std::vector<SearchResult> results(candidate_ids.size());

    // Convert query to float if needed
    std::vector<float> query_float(query_dim);
    for (uint32_t i = 0; i < query_dim; i++) {
        query_float[i] = static_cast<float>(query[i]);
    }

    #pragma omp parallel for schedule(dynamic, 256)
    for (size_t i = 0; i < candidate_ids.size(); ++i) {
        uint32_t vid = candidate_ids[i];
        if (vid < raw_vectors.size() / vec_dim) {
            float d = L2_AVX2(query_float.data(), raw_vectors.data() + (size_t)vid * vec_dim, vec_dim);
            results[i] = {vid, d};
        } else {
            results[i] = {vid, std::numeric_limits<float>::max()};
        }
    }

    size_t final_k = std::min((size_t)k_search, results.size());
    std::partial_sort(results.begin(), results.begin() + final_k, results.end());

    result_ids.resize(final_k);
    result_dists.resize(final_k);
    for (size_t i = 0; i < final_k; ++i) {
        result_ids[i] = results[i].id;
        result_dists[i] = results[i].dist;
    }
}

void print_stats(std::string category, std::vector<float> percentiles, std::vector<float> results)
{
    diskann::cout << std::setw(20) << category << ": " << std::flush;
    for (uint32_t s = 0; s < percentiles.size(); s++)
    {
        diskann::cout << std::setw(8) << percentiles[s] << "%";
    }
    diskann::cout << std::endl;
    diskann::cout << std::setw(22) << " " << std::flush;
    for (uint32_t s = 0; s < percentiles.size(); s++)
    {
        diskann::cout << std::setw(9) << results[s];
    }
    diskann::cout << std::endl;
}

// ==========================================
// Main Hybrid Search Function
// ==========================================
template <typename T, typename LabelT = uint32_t>
int hybrid_search_disk_index(diskann::Metric &metric, const std::string &index_path_prefix,
                      const std::string &result_output_prefix, const std::string &query_file,
                      std::string &gt_file, const uint32_t num_threads, const uint32_t recall_at,
                      const uint32_t beamwidth, const uint32_t num_nodes_to_cache,
                      const uint32_t search_io_limit, const std::vector<uint32_t> &Lvec,
                      const float fail_if_recall_below, const std::string &filter_expression,
                      const std::string &inverted_index_path, const std::string &raw_vector_path,
                      const float hit_rate_threshold, const bool use_reorder_data = false)
{
    diskann::cout << "=== Hybrid Search Parameters ===" << std::endl;
    diskann::cout << "Threads: " << num_threads << ", Beamwidth: " << beamwidth << std::endl;
    diskann::cout << "Filter Expression: " << filter_expression << std::endl;
    diskann::cout << "Inverted Index: " << inverted_index_path << std::endl;
    diskann::cout << "Raw Vectors: " << raw_vector_path << std::endl;
    diskann::cout << "Hit Rate Threshold: " << hit_rate_threshold << std::endl;
    diskann::cout << "================================" << std::endl;

    // Load query data
    T *query = nullptr;
    uint32_t *gt_ids = nullptr;
    float *gt_dists = nullptr;
    size_t query_num, query_dim, query_aligned_dim, gt_num, gt_dim;
    diskann::load_aligned_bin<T>(query_file, query, query_num, query_dim, query_aligned_dim);

    bool calc_recall_flag = false;
    if (gt_file != std::string("null") && gt_file != std::string("NULL") && file_exists(gt_file))
    {
        diskann::load_truthset(gt_file, gt_ids, gt_dists, gt_num, gt_dim);
        if (gt_num != query_num)
        {
            diskann::cout << "Error. Mismatch in number of queries and ground truth data" << std::endl;
        }
        calc_recall_flag = true;
    }

    // Load inverted index
    InvertedIndexSearcher ivf_index;
    ivf_index.load(inverted_index_path);
    diskann::cout << "Inverted index loaded successfully." << std::endl;

    // Load raw vectors for brute force search
    uint32_t raw_vec_dim = 0, raw_vec_num = 0;
    std::vector<float> raw_vectors = load_raw_vectors(raw_vector_path, raw_vec_dim, raw_vec_num);

    // Parse filter expression to get candidates
    ExpressionParser parser(ivf_index);
    roaring::Roaring candidates = parser.parse(filter_expression);
    std::vector<uint32_t> candidate_ids(candidates.cardinality());
    candidates.toUint32Array(candidate_ids.data());

    float hit_rate = (float)candidate_ids.size() / (float)raw_vec_num;
    diskann::cout << "Filter candidates: " << candidate_ids.size() << " / " << raw_vec_num
                  << " (hit rate: " << hit_rate << ")" << std::endl;

    // Decide search strategy
    bool use_ivf_search = (hit_rate < hit_rate_threshold);
    diskann::cout << "Search strategy: " << (use_ivf_search ? "IVF Brute Force" : "DiskANN") << std::endl;

    std::vector<std::vector<uint32_t>> query_result_ids(Lvec.size());
    std::vector<std::vector<float>> query_result_dists(Lvec.size());

    if (use_ivf_search)
    {
        // Use IVF brute force search
        diskann::cout << "Using IVF-based brute force search..." << std::endl;

        for (uint32_t test_id = 0; test_id < Lvec.size(); test_id++)
        {
            uint32_t L = Lvec[test_id];
            if (L < recall_at)
            {
                diskann::cout << "Ignoring search with L:" << L << " since it's smaller than K:" << recall_at << std::endl;
                continue;
            }

            query_result_ids[test_id].resize(recall_at * query_num);
            query_result_dists[test_id].resize(recall_at * query_num);

            auto s = std::chrono::high_resolution_clock::now();

            #pragma omp parallel for schedule(dynamic, 1) num_threads(num_threads)
            for (int64_t i = 0; i < (int64_t)query_num; i++)
            {
                std::vector<uint32_t> res_ids;
                std::vector<float> res_dists;
                ivf_brute_force_search(query + (i * query_aligned_dim), query_dim,
                                     candidate_ids, raw_vectors, raw_vec_dim,
                                     recall_at, res_ids, res_dists);

                for (uint32_t j = 0; j < res_ids.size(); j++)
                {
                    query_result_ids[test_id][i * recall_at + j] = res_ids[j];
                    query_result_dists[test_id][i * recall_at + j] = res_dists[j];
                }
            }

            auto e = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> diff = e - s;
            double qps = (1.0 * query_num) / (1.0 * diff.count());
            double mean_latency = (diff.count() * 1000000.0) / query_num;

            diskann::cout << "L=" << L << ", QPS=" << qps << ", Mean Latency=" << mean_latency << " us";

            if (calc_recall_flag)
            {
                double recall = diskann::calculate_recall((uint32_t)query_num, gt_ids, gt_dists, (uint32_t)gt_dim,
                                                   query_result_ids[test_id].data(), recall_at, recall_at);
                diskann::cout << ", Recall@" << recall_at << "=" << recall;
            }
            diskann::cout << std::endl;
        }
    }
    else
    {
        // Use DiskANN search with multi-entry points optimization
        diskann::cout << "Using optimized DiskANN search with multi-entry points..." << std::endl;

        // Load DiskANN index
        std::shared_ptr<AlignedFileReader> reader = nullptr;
#ifdef _WINDOWS
#ifndef USE_BING_INFRA
        reader.reset(new WindowsAlignedFileReader());
#else
        reader.reset(new diskann::BingAlignedFileReader());
#endif
#else
        reader.reset(new LinuxAlignedFileReader());
#endif

        std::unique_ptr<diskann::PQFlashIndex<T, LabelT>> _pFlashIndex(
            new diskann::PQFlashIndex<T, LabelT>(reader, metric));

        int res = _pFlashIndex->load(num_threads, index_path_prefix.c_str());
        if (res != 0) {
            return res;
        }

        std::vector<uint32_t> node_list;
        diskann::cout << "Caching " << num_nodes_to_cache << " nodes around medoid(s)" << std::endl;
        _pFlashIndex->cache_bfs_levels(num_nodes_to_cache, node_list);
        _pFlashIndex->load_cache_list(node_list);
        node_list.clear();
        node_list.shrink_to_fit();

        omp_set_num_threads(num_threads);

        uint32_t optimized_beamwidth = beamwidth;

        double best_recall = 0.0;

        for (uint32_t test_id = 0; test_id < Lvec.size(); test_id++)
        {
            uint32_t L = Lvec[test_id];

            if (L < recall_at)
            {
                diskann::cout << "Ignoring search with L:" << L << " since it's smaller than K:" << recall_at << std::endl;
                continue;
            }

            query_result_ids[test_id].resize(recall_at * query_num);
            query_result_dists[test_id].resize(recall_at * query_num);

            auto stats = new diskann::QueryStats[query_num];

            std::vector<uint64_t> query_result_ids_64(recall_at * query_num);
            auto s = std::chrono::high_resolution_clock::now();

            #pragma omp parallel for schedule(dynamic, 1) num_threads(num_threads)
            for (int64_t i = 0; i < (int64_t)query_num; i++)
            {
                // Call the optimized search with multi-entry points
                _pFlashIndex->cached_beam_search_with_multi_entry(
                    query + (i * query_aligned_dim), recall_at, L,
                    query_result_ids_64.data() + (i * recall_at),
                    query_result_dists[test_id].data() + (i * recall_at),
                    optimized_beamwidth,
                    candidate_ids,      // Candidates from inverted index
                    candidate_ids,      // Valid label IDs (same as candidates)
                    use_reorder_data,
                    stats + i);
            }

            auto e = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> diff = e - s;
            double qps = (1.0 * query_num) / (1.0 * diff.count());

            diskann::convert_types<uint64_t, uint32_t>(query_result_ids_64.data(), query_result_ids[test_id].data(),
                                                       query_num, recall_at);

            auto mean_latency = diskann::get_mean_stats<float>(
                stats, query_num, [](const diskann::QueryStats &stats) { return stats.total_us; });

            auto latency_999 = diskann::get_percentile_stats<float>(
                stats, query_num, 0.999, [](const diskann::QueryStats &stats) { return stats.total_us; });

            auto mean_ios = diskann::get_mean_stats<uint32_t>(stats, query_num,
                                                              [](const diskann::QueryStats &stats) { return stats.n_ios; });

            auto mean_cpuus = diskann::get_mean_stats<float>(stats, query_num,
                                                             [](const diskann::QueryStats &stats) { return stats.cpu_us; });

            auto mean_io_us = diskann::get_mean_stats<float>(stats, query_num,
                                                             [](const diskann::QueryStats &stats) { return stats.io_us; });

            double recall = 0;
            if (calc_recall_flag)
            {
                recall = diskann::calculate_recall((uint32_t)query_num, gt_ids, gt_dists, (uint32_t)gt_dim,
                                                   query_result_ids[test_id].data(), recall_at, recall_at);
                best_recall = std::max(recall, best_recall);
            }

            diskann::cout << "L=" << L << ", BW=" << optimized_beamwidth
                          << ", QPS=" << qps << ", Mean Latency=" << mean_latency << " us"
                          << ", 99.9 Latency=" << latency_999 << " us"
                          << ", Mean IOs=" << mean_ios
                          << ", Mean IO time=" << mean_io_us << " us"
                          << ", Mean CPU time=" << mean_cpuus << " us";

            if (calc_recall_flag)
            {
                diskann::cout << ", Recall@" << recall_at << "=" << recall;
            }
            diskann::cout << std::endl;

            delete[] stats;
        }
    }

    // Save results
    diskann::cout << "Saving results..." << std::endl;
    uint64_t test_id = 0;
    for (auto L : Lvec)
    {
        if (L < recall_at)
            continue;

        std::string cur_result_path = result_output_prefix + "_" + std::to_string(L) + "_idx_uint32.bin";
        diskann::save_bin<uint32_t>(cur_result_path, query_result_ids[test_id].data(), query_num, recall_at);

        cur_result_path = result_output_prefix + "_" + std::to_string(L) + "_dists_float.bin";
        diskann::save_bin<float>(cur_result_path, query_result_dists[test_id++].data(), query_num, recall_at);
    }

    diskann::aligned_free(query);
    return 0;
}

int main(int argc, char **argv)
{
    std::string data_type, dist_fn, index_path_prefix, result_path_prefix, query_file, gt_file;
    std::string filter_expression, inverted_index_path, raw_vector_path;
    uint32_t num_threads, K, W, num_nodes_to_cache, search_io_limit;
    std::vector<uint32_t> Lvec;
    float hit_rate_threshold = 0.1f;
    bool use_reorder_data = false;
    float fail_if_recall_below = 0.0f;

    po::options_description desc{
        program_options_utils::make_program_description("hybrid_search_disk_index",
            "Hybrid search with inverted index and DiskANN")};
    try
    {
        desc.add_options()("help,h", "Print information on arguments");

        // Required parameters
        po::options_description required_configs("Required");
        required_configs.add_options()("data_type", po::value<std::string>(&data_type)->required(),
                                       program_options_utils::DATA_TYPE_DESCRIPTION);
        required_configs.add_options()("dist_fn", po::value<std::string>(&dist_fn)->required(),
                                       program_options_utils::DISTANCE_FUNCTION_DESCRIPTION);
        required_configs.add_options()("index_path_prefix", po::value<std::string>(&index_path_prefix)->required(),
                                       "DiskANN index path prefix");
        required_configs.add_options()("result_path", po::value<std::string>(&result_path_prefix)->required(),
                                       program_options_utils::RESULT_PATH_DESCRIPTION);
        required_configs.add_options()("query_file", po::value<std::string>(&query_file)->required(),
                                       program_options_utils::QUERY_FILE_DESCRIPTION);
        required_configs.add_options()("recall_at,K", po::value<uint32_t>(&K)->required(),
                                       program_options_utils::NUMBER_OF_RESULTS_DESCRIPTION);
        required_configs.add_options()("search_list,L",
                                       po::value<std::vector<uint32_t>>(&Lvec)->multitoken()->required(),
                                       program_options_utils::SEARCH_LIST_DESCRIPTION);
        required_configs.add_options()("inverted_index_path",
                                       po::value<std::string>(&inverted_index_path)->required(),
                                       "Path to inverted index binary file");
        required_configs.add_options()("raw_vector_path",
                                       po::value<std::string>(&raw_vector_path)->required(),
                                       "Path to raw vector binary file (for IVF search)");
        required_configs.add_options()("filter_expression",
                                       po::value<std::string>(&filter_expression)->required(),
                                       "Filter expression (e.g., \"(3&5)|7\" or single tag like \"5\")");

        // Optional parameters
        po::options_description optional_configs("Optional");
        optional_configs.add_options()("gt_file", po::value<std::string>(&gt_file)->default_value(std::string("null")),
                                       program_options_utils::GROUND_TRUTH_FILE_DESCRIPTION);
        optional_configs.add_options()("beamwidth,W", po::value<uint32_t>(&W)->default_value(2),
                                       program_options_utils::BEAMWIDTH);
        optional_configs.add_options()("num_nodes_to_cache", po::value<uint32_t>(&num_nodes_to_cache)->default_value(0),
                                       "Number of nodes to cache");
        optional_configs.add_options()(
            "search_io_limit",
            po::value<uint32_t>(&search_io_limit)->default_value(std::numeric_limits<uint32_t>::max()),
            "Max #IOs for search");
        optional_configs.add_options()("num_threads,T",
                                       po::value<uint32_t>(&num_threads)->default_value(omp_get_num_procs()),
                                       program_options_utils::NUMBER_THREADS_DESCRIPTION);
        optional_configs.add_options()("hit_rate_threshold",
                                       po::value<float>(&hit_rate_threshold)->default_value(0.1f),
                                       "Hit rate threshold for choosing search strategy (default: 0.1)");
        optional_configs.add_options()("use_reorder_data", po::bool_switch()->default_value(false),
                                       "Use full precision reorder data");
        optional_configs.add_options()("fail_if_recall_below",
                                       po::value<float>(&fail_if_recall_below)->default_value(0.0f),
                                       "Fail if recall is below this value");

        desc.add(required_configs).add(optional_configs);

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);
        if (vm.count("help"))
        {
            std::cout << desc;
            return 0;
        }
        po::notify(vm);
        if (vm["use_reorder_data"].as<bool>())
            use_reorder_data = true;
    }
    catch (const std::exception &ex)
    {
        std::cerr << ex.what() << '\n';
        return -1;
    }

    diskann::Metric metric;
    if (dist_fn == std::string("mips"))
    {
        metric = diskann::Metric::INNER_PRODUCT;
    }
    else if (dist_fn == std::string("l2"))
    {
        metric = diskann::Metric::L2;
    }
    else if (dist_fn == std::string("cosine"))
    {
        metric = diskann::Metric::COSINE;
    }
    else
    {
        std::cout << "Unsupported distance function." << std::endl;
        return -1;
    }

    try
    {
        if (data_type == std::string("float"))
            return hybrid_search_disk_index<float>(metric, index_path_prefix, result_path_prefix, query_file, gt_file,
                                            num_threads, K, W, num_nodes_to_cache, search_io_limit, Lvec,
                                            fail_if_recall_below, filter_expression, inverted_index_path,
                                            raw_vector_path, hit_rate_threshold, use_reorder_data);
        else if (data_type == std::string("int8"))
            return hybrid_search_disk_index<int8_t>(metric, index_path_prefix, result_path_prefix, query_file, gt_file,
                                             num_threads, K, W, num_nodes_to_cache, search_io_limit, Lvec,
                                             fail_if_recall_below, filter_expression, inverted_index_path,
                                             raw_vector_path, hit_rate_threshold, use_reorder_data);
        else if (data_type == std::string("uint8"))
            return hybrid_search_disk_index<uint8_t>(metric, index_path_prefix, result_path_prefix, query_file, gt_file,
                                              num_threads, K, W, num_nodes_to_cache, search_io_limit, Lvec,
                                              fail_if_recall_below, filter_expression, inverted_index_path,
                                              raw_vector_path, hit_rate_threshold, use_reorder_data);
        else
        {
            std::cerr << "Unsupported data type. Use float or int8 or uint8" << std::endl;
            return -1;
        }
    }
    catch (const std::exception &e)
    {
        std::cout << std::string(e.what()) << std::endl;
        diskann::cerr << "Hybrid index search failed." << std::endl;
        return -1;
    }
}
