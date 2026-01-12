# Hybrid Search with Inverted Index and DiskANN

## 概述

这个混合搜索系统结合了倒排索引（Inverted Index）和 DiskANN 的优势，通过自动选择最优的搜索策略来提升带标签过滤的向量搜索性能。

## 核心思路

### 问题背景

在向量搜索中，当需要根据标签进行过滤时：
- **稀疏标签场景**（命中率低）：满足条件的向量很少，DiskANN 需要遍历大量不相关的向量
- **密集标签场景**（命中率高）：满足条件的向量很多，倒排索引的暴力搜索成本过高

### 解决方案

通过**命中率阈值（hit_rate_threshold）**来动态选择搜索策略：

1. **命中率 < threshold**：使用**倒排索引 + 暴力搜索**
   - 先用倒排索引快速获取所有满足标签的向量 ID
   - 直接对这些候选向量进行距离计算（使用 AVX2 加速）
   - 适合稀疏标签场景

2. **命中率 ≥ threshold**：使用 **DiskANN 搜索**（待实现）
   - 使用多入口点策略（从倒排索引随机选择 K 个满足标签的点）
   - 分离导航集（search_ctx）和结果集（result_heap）
   - 如果结果不足，用倒排索引填充
   - 适合密集标签场景

## 功能特点

- ✅ **标签表达式支持**：支持复杂的布尔表达式，如 `(3&5)|7`、`(1&2)&(3|4)` 等
- ✅ **自动策略选择**：根据命中率自动选择最优搜索策略
- ✅ **AVX2 加速**：倒排索引搜索使用 AVX2 SIMD 指令加速距离计算
- ✅ **OpenMP 并行**：多线程并行搜索，充分利用多核 CPU
- ✅ **通用标签支持**：支持通用向量（universal vector）的概念
- ✅ **DiskANN 多入口点**：改进的 DiskANN 搜索逻辑，使用多入口点和分离的导航/结果集
- ✅ **智能后填充**：当结果不足时自动使用倒排索引补充

## 系统架构

```
                    ┌─────────────────────┐
                    │   Query + Filter    │
                    │    Expression       │
                    └──────────┬──────────┘
                               │
                    ┌──────────▼──────────┐
                    │  Parse Expression   │
                    │  Get Candidate IDs  │
                    └──────────┬──────────┘
                               │
                    ┌──────────▼──────────┐
                    │ Calculate Hit Rate  │
                    └──────────┬──────────┘
                               │
                ┌──────────────┴──────────────┐
                │                             │
      ┌─────────▼────────┐        ┌──────────▼─────────┐
      │  Hit Rate < θ    │        │  Hit Rate ≥ θ      │
      └─────────┬────────┘        └──────────┬─────────┘
                │                             │
      ┌─────────▼────────┐        ┌──────────▼─────────┐
      │ IVF Brute Force  │        │ DiskANN Search     │
      │  + AVX2 + OMP    │        │ (Multi-Entry)      │
      └─────────┬────────┘        └──────────┬─────────┘
                │                             │
                └──────────────┬──────────────┘
                               │
                    ┌──────────▼──────────┐
                    │   Top-K Results     │
                    └─────────────────────┘
```

## 构建步骤

### 0. 初始化子模块

首次克隆仓库后，需要初始化并更新 Git 子模块：

```bash
# 克隆主仓库
git clone <repository-url>
cd DiskANN-Kingbase

# 初始化并更新子模块（包括 CRoaring）
git submodule init
git submodule update

# 或者使用一条命令
git submodule update --init --recursive
```

**说明**：CRoaring 库位于 `third_party/CRoaring` 目录，作为 Git 子模块管理。

### 1. 构建倒排索引

首先需要构建倒排索引。倒排索引只需要标签文件。

```bash
cd test_ivf
mkdir build && cd build
cmake ..
make

# 构建倒排索引
./build_ivf_index \
    <label_file.txt> \
    <output_index.bin> \
    <universal_tag_id>
```

**参数说明**：
- `label_file.txt`: 标签文件，每行一个向量的标签，格式为逗号分隔的标签 ID，例如：
  ```
  1,2,3
  4,5
  11
  1,3
  ```
- `output_index.bin`: 输出的倒排索引文件
- `universal_tag_id`: 通用标签 ID（例如 11），拥有此标签的向量会被所有查询匹配

**示例**：
```bash
./build_ivf_index labels.txt inverted_index.bin 11
```

### 2. 构建 DiskANN 索引

按照标准的 DiskANN 流程构建索引：

```bash
# 构建内存索引
./build_memory_index --data_type float --dist_fn l2 --data_file data.bin \
    --index_path_prefix memory_index -R 64 -L 128 --alpha 1.2

# 构建磁盘索引
./build_disk_index --data_type float --dist_fn l2 --data_file data.bin \
    --index_path_prefix disk_index -R 64 -L 128 --build_PQ_bytes 16
```

### 3. 构建混合搜索程序

在主项目目录下构建：

```bash
mkdir build && cd build
cmake ..
make hybrid_search_disk_index
```

**注意**：在构建之前，需要先构建 `test_ivf` 以生成 Roaring 库。

## 使用方法

### 基本用法

```bash
./hybrid_search_disk_index \
    --data_type float \
    --dist_fn l2 \
    --index_path_prefix disk_index \
    --query_file queries.bin \
    --gt_file ground_truth.bin \
    --result_path results \
    --recall_at 10 \
    --search_list 50 100 150 \
    --inverted_index_path inverted_index.bin \
    --raw_vector_path data.bin \
    --filter_expression "(3&5)|7" \
    --hit_rate_threshold 0.1 \
    --num_threads 16 \
    --beamwidth 4
```

### 参数说明

#### 必需参数

| 参数 | 说明 | 示例 |
|------|------|------|
| `--data_type` | 数据类型 | `float`, `int8`, `uint8` |
| `--dist_fn` | 距离函数 | `l2`, `mips`, `cosine` |
| `--index_path_prefix` | DiskANN 索引路径前缀 | `disk_index` |
| `--query_file` | 查询向量文件 | `queries.bin` |
| `--result_path` | 结果输出路径 | `results` |
| `--recall_at` / `-K` | 返回的最近邻数量 | `10` |
| `--search_list` / `-L` | 搜索列表大小（可多个值） | `50 100 150` |
| `--inverted_index_path` | 倒排索引文件路径 | `inverted_index.bin` |
| `--raw_vector_path` | 原始向量文件路径（用于 IVF 搜索） | `data.bin` |
| `--filter_expression` | 标签过滤表达式 | `"(3&5)\|7"` |

#### 可选参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--gt_file` | Ground truth 文件（用于计算召回率） | `null` |
| `--beamwidth` / `-W` | Beam width | `2` |
| `--num_nodes_to_cache` | 缓存的节点数量 | `0` |
| `--search_io_limit` | 搜索的最大 IO 次数 | `uint32::max()` |
| `--num_threads` / `-T` | 线程数 | 系统核心数 |
| `--hit_rate_threshold` | 命中率阈值 | `0.1` |
| `--use_reorder_data` | 使用重排序数据 | `false` |
| `--fail_if_recall_below` | 如果召回率低于此值则失败 | `0.0` |

### 标签表达式语法

支持以下运算符：

- `&`: 与（AND）- 必须同时满足
- `|`: 或（OR）- 满足任一即可
- `-`: 差（NOT）- 排除
- `()`: 括号 - 控制优先级

**示例**：

1. **简单表达式**：
   ```bash
   --filter_expression "5"           # 只查询标签为 5 的向量
   ```

2. **与运算**：
   ```bash
   --filter_expression "3&5"         # 查询同时有标签 3 和 5 的向量
   ```

3. **或运算**：
   ```bash
   --filter_expression "3|5|7"       # 查询有标签 3 或 5 或 7 的向量
   ```

4. **复杂表达式**：
   ```bash
   --filter_expression "(3&5)|7"     # (标签3 且 标签5) 或 标签7
   --filter_expression "(1|2)&(3|4)" # (标签1 或 标签2) 且 (标签3 或 标签4)
   --filter_expression "3&5-7"       # (标签3 且 标签5) 但不是 标签7
   ```

### 命中率阈值（hit_rate_threshold）调优

命中率阈值是选择搜索策略的关键参数：

- **阈值太低**（如 0.01）：更多查询会使用 DiskANN，但在稀疏场景下效率不高
- **阈值太高**（如 0.5）：更多查询会使用倒排索引，但在密集场景下成本过高
- **推荐值**：`0.05` - `0.15`

**如何选择**：

1. 运行测试，观察不同查询的命中率分布
2. 如果大部分查询的命中率 < 0.1，使用较低阈值（0.05）
3. 如果命中率分布均匀，使用中等阈值（0.1）
4. 可以根据性能测试结果微调

## 性能优化建议

### 1. 编译优化

确保使用 Release 模式编译：
```bash
cmake -DCMAKE_BUILD_TYPE=Release ..
```

### 2. AVX2 支持

确保 CPU 支持 AVX2，并且编译器启用了 AVX2 指令：
- GCC/Clang: `-march=native` 或 `-mavx2`
- MSVC: `/arch:AVX2`

### 3. 线程数优化

根据 CPU 核心数和查询模式调整线程数：
```bash
--num_threads 16    # 对于 16 核 CPU
```

### 4. 缓存优化

对于频繁访问的节点，可以增加缓存：
```bash
--num_nodes_to_cache 10000
```

## 当前状态和未来工作

### 当前版本（v2.0 - 完整实现）

✅ **已完整实现**：
- 倒排索引构建和加载
- 标签表达式解析（支持 `&`, `|`, `-` 操作符）
- 命中率计算和策略分支
- IVF 暴力搜索（AVX2 优化）
- **DiskANN 多入口点搜索**
  - 从倒排索引随机选择多个入口点
  - 分离的 search_ctx（导航集）和 result_heap（结果集）
  - 结果集只包含满足标签的点
  - 导航集不考虑标签，纯粹按距离导航
- **智能后填充机制**
  - 当 DiskANN 搜索结果不足 K 个时
  - 自动从倒排索引候选集中补充
  - 确保始终返回 K 个结果
- 完整的参数系统和统计信息

### 未来版本计划

**v2.1 - 性能优化**：
- 支持 AVX-512 指令集
- GPU 加速的倒排索引搜索
- 更智能的阈值自适应算法
- 动态调整入口点数量

**v2.2 - 功能扩展**：
- 支持更复杂的过滤条件（范围查询、正则表达式）
- 支持动态更新标签（无需重建索引）
- 支持多级索引结构
- 支持增量更新

**v2.3 - 分布式支持**：
- 分布式倒排索引
- 分片查询和聚合
- 负载均衡

## 示例工作流

### 完整示例

假设你有以下数据：
- `vectors.bin`: 100万个128维向量
- `labels.txt`: 每个向量的标签
- `queries.bin`: 1000个查询向量
- `ground_truth.bin`: Ground truth

**步骤 1：构建倒排索引**
```bash
cd test_ivf/build
./build_ivf_index ../../data/labels.txt inverted_index.bin 11
```

**步骤 2：构建 DiskANN 索引**
```bash
cd ../../build
./build_disk_index --data_type float --dist_fn l2 \
    --data_file ../data/vectors.bin \
    --index_path_prefix ../data/disk_index \
    -R 64 -L 128 --build_PQ_bytes 16
```

**步骤 3：运行混合搜索**
```bash
./hybrid_search_disk_index \
    --data_type float \
    --dist_fn l2 \
    --index_path_prefix ../data/disk_index \
    --query_file ../data/queries.bin \
    --gt_file ../data/ground_truth.bin \
    --result_path ../results/hybrid \
    --recall_at 10 \
    --search_list 50 100 150 \
    --inverted_index_path ../test_ivf/build/inverted_index.bin \
    --raw_vector_path ../data/vectors.bin \
    --filter_expression "(3&5)|7" \
    --hit_rate_threshold 0.1 \
    --num_threads 16 \
    --beamwidth 4
```

## 故障排除

### 问题 1: "Cannot find Roaring library"

**原因**：CRoaring 子模块未初始化或未构建

**解决方案**：
```bash
# 确保子模块已初始化
git submodule update --init --recursive

# 构建主项目（会自动构建 CRoaring）
mkdir build && cd build
cmake ..
make
```

### 问题 2: "No such file or directory: third_party/CRoaring"

**原因**：子模块未正确克隆

**解决方案**：
```bash
# 手动初始化子模块
cd DiskANN-Kingbase
git submodule init
git submodule update

# 或者重新克隆仓库时使用 --recursive
git clone --recursive <repository-url>
```

### 问题 3: "Invalid expression logic"

**原因**：标签表达式语法错误

**解决方案**：检查表达式格式：
- 使用正确的运算符：`&`, `|`, `-`
- 括号必须匹配
- 标签 ID 必须是数字

### 问题 3: 性能不如预期

**检查清单**：
1. ✓ 使用 Release 模式编译
2. ✓ CPU 支持 AVX2
3. ✓ 命中率阈值设置合理
4. ✓ 线程数设置合理
5. ✓ 原始向量文件和索引文件匹配

## 技术细节

### 改进的 DiskANN 搜索算法

当命中率高于阈值时，系统使用改进的 DiskANN 搜索算法，核心改进包括：

#### 1. 多入口点策略

**原始 DiskANN**：
- 使用单个全局 medoid 作为搜索入口点
- 对于稀疏标签，可能需要遍历大量不相关节点才能找到满足标签的结果

**改进算法**：
```cpp
// 从倒排索引的候选集中随机选择 beam_width 个点作为入口点
std::vector<uint32_t> entry_points;
if (candidate_ids.size() <= beam_width) {
    entry_points = candidate_ids;  // 使用所有候选
} else {
    // 随机采样
    std::shuffle(candidate_ids.begin(), candidate_ids.end(), rng);
    entry_points.assign(candidate_ids.begin(), candidate_ids.begin() + beam_width);
}
```

**优势**：
- 利用语义局部性假设：相似标签的向量在向量空间中也可能接近
- 多个入口点增加搜索覆盖面
- 减少不必要的图遍历

#### 2. 分离的导航集和结果集

**原始 DiskANN**：
- 使用单一优先队列 `retset`
- 同时作为导航队列和结果队列
- 在过滤搜索中，不满足标签的点会占用队列空间，影响搜索效率

**改进算法**：

```cpp
// search_ctx: 导航优先队列（小顶堆，按距离排序）
NeighborPriorityQueue search_ctx;  // 用于图遍历
search_ctx.reserve(l_search);

// result_heap: 结果优先队列（大顶堆，按距离排序）
std::priority_queue<Neighbor> result_heap;  // 只存满足标签的结果
```

**处理逻辑**：

```cpp
for (uint64_t m = 0; m < nnbrs; ++m) {
    uint32_t id = node_nbrs[m];
    if (visited.insert(id).second) {
        float dist = dist_scratch[m];

        // 分支 A：维护结果集（只看标签）
        if (valid_label_set.find(id) != valid_label_set.end()) {
            if (result_heap.size() < k_search) {
                result_heap.push(Neighbor(id, dist));
            } else if (dist < result_heap.top().distance) {
                result_heap.pop();
                result_heap.push(Neighbor(id, dist));
            }
        }

        // 分支 B：维护导航集（只看距离，忽略标签）
        if (search_ctx.size() < l_search || dist < search_ctx.furthest().distance) {
            search_ctx.insert(Neighbor(id, dist));
        }
    }
}
```

**优势**：
- `search_ctx` 专注于图导航，不受标签限制
- `result_heap` 只收集满足标签的最终结果
- 避免了"导航好但标签不匹配"和"标签匹配但导航差"的矛盾

#### 3. 改进的剪枝策略

```cpp
// 选择下一个要扩展的节点
auto nbr = search_ctx.closest_unexpanded();

// 剪枝检查
if (search_ctx.size() >= l_search) {
    auto furthest_in_ctx = search_ctx.furthest();
    if (nbr.distance > furthest_in_ctx.distance) {
        break;  // 当前节点太远，停止搜索
    }
}
```

**优势**：
- 防止在几何空间上偏离太远
- 即使节点是"路标"，但如果距离过远也会被剪枝
- 平衡搜索质量和效率

#### 4. 智能后填充机制

```cpp
// 步骤 3：从 result_heap 提取结果
std::vector<Neighbor> final_results;
while (!result_heap.empty()) {
    final_results.push_back(result_heap.top());
    result_heap.pop();
}
std::sort(final_results.begin(), final_results.end());

// 步骤 4：如果结果不足，使用倒排索引填充
if (final_results.size() < k_search) {
    // 计算所有候选点的距离
    std::vector<Neighbor> all_candidates;
    for (uint32_t cand_id : candidate_ids) {
        if (!already_in_results(cand_id)) {
            compute_dists(&cand_id, 1, dist_scratch);
            all_candidates.push_back(Neighbor(cand_id, dist_scratch[0]));
        }
    }

    // 排序并填充
    std::sort(all_candidates.begin(), all_candidates.end());
    for (const auto &cand : all_candidates) {
        if (final_results.size() >= k_search) break;
        final_results.push_back(cand);
    }
}
```

**优势**：
- 确保始终返回 K 个结果
- 对于极其稀疏的标签，DiskANN 图遍历可能找不到足够结果
- 后填充保证了搜索的鲁棒性

### 倒排索引格式

二进制格式：
```
[Magic: uint32_t]                    // 0xAA55AA55
[Universal Bitmap Size: uint32_t]
[Universal Bitmap Data: bytes]
[Map Size: uint32_t]                 // 标签数量
For each tag:
    [Tag ID: int]
    [Bitmap Size: uint32_t]
    [Bitmap Data: bytes]
```

### AVX2 距离计算

使用 FMA（Fused Multiply-Add）指令：
```cpp
__m256 sum = _mm256_setzero_ps();
for (i = 0; i + 7 < dim; i += 8) {
    __m256 va = _mm256_loadu_ps(a + i);
    __m256 vb = _mm256_loadu_ps(b + i);
    __m256 diff = _mm256_sub_ps(va, vb);
    sum = _mm256_fmadd_ps(diff, diff, sum);
}
```

### 表达式解析

使用 Shunting-yard 算法将中缀表达式转换为 RPN（逆波兰表达式）：
1. `"(3&5)|7"` → `"3 5 & 7 |"`
2. 栈式求值，使用 Roaring Bitmap 操作

## 参考资料

- [DiskANN 论文](https://arxiv.org/abs/1912.05384)
- [CRoaring 文档](https://github.com/RoaringBitmap/CRoaring)
- [AVX2 指令集参考](https://www.intel.com/content/www/us/en/docs/intrinsics-guide/)

## 贡献者

如有问题或建议，请提交 Issue 或 Pull Request。

## 许可证

本项目继承 DiskANN 的 MIT 许可证。
