#!/bin/bash

# 配置
VEC_BIN="vec_data.bin"
LABEL_TXT="labels.txt"
INDEX_BIN="index.bin"
CPP_RESULT="ivf_result.txt"
PY_RESULT="py_result.txt"
FILTER_EXPR="(3&5)|7"
TOP_K=10
UNIVERSAL_TAG=11

# 颜色
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

echo "=========================================="
echo "      Build & Search Verification         "
echo "=========================================="

# 1. 编译 C++
if [ ! -d "build" ]; then
    mkdir build
fi
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4
if [ $? -ne 0 ]; then
    echo -e "${RED}Compilation Failed!${NC}"
    exit 1
fi
cd ..
echo -e "${GREEN}Compilation Success.${NC}"

# 函数: 提取结果文件中的 ID 列表用于对比
extract_ids() {
    infile=$1
    # 提取 "ID=xxx" 中的数字
    grep "Rank " $infile | sed -E 's/.*ID=([0-9]+).*/\1/' > "${infile}.ids"
}

# 2. 循环测试不同规模
SIZES=(10000 100000)

for NUM in "${SIZES[@]}"; do
    echo ""
    echo "------------------------------------------"
    echo "TESTING SCALE: $NUM vectors"
    echo "------------------------------------------"

    # A. 生成数据
    python3 generate_data.py --num $NUM --dim 128 --vec_out $VEC_BIN --label_out $LABEL_TXT

    # B. 构建倒排索引 (C++)
    echo ">>> [C++] Building Index..."
    ./build/build_ivf_index $LABEL_TXT $INDEX_BIN $UNIVERSAL_TAG
    
    # C. 执行 C++ 搜索
    echo ">>> [C++] Searching..."
    # 存全部输出到 log，提取结果行到结果文件
    ./build/search_ivf_index $INDEX_BIN $VEC_BIN "$FILTER_EXPR" $TOP_K > cpp_log.txt
    
    # 从日志中提取结果部分 (假设 C++ 代码输出以 Rank 开头的行)
    grep "Rank " cpp_log.txt > $CPP_RESULT
    cat $CPP_RESULT
    
    # 获取 C++ 耗时 (从日志里 grep)
    CPP_TIME=$(grep "Scan done in" cpp_log.txt | grep -oE "[0-9]+\.[0-9]+")
    echo "C++ Time: ${CPP_TIME} ms"

    # D. 执行 Python 暴力搜索 (Ground Truth)
    echo ">>> [Python] Ground Truth Searching..."
    python3 ground_truth_search.py $VEC_BIN $LABEL_TXT "$FILTER_EXPR" $TOP_K $PY_RESULT
    cat $PY_RESULT

    # E. 结果对比
    extract_ids $CPP_RESULT
    extract_ids $PY_RESULT

    # 对比 ID 是否一致
    DIFF=$(diff ${CPP_RESULT}.ids ${PY_RESULT}.ids)
    
    if [ -z "$DIFF" ]; then
        echo -e "${GREEN}[PASS] Results Match!${NC}"
    else
        echo -e "${RED}[FAIL] Results Mismatch!${NC}"
        echo "Diff:"
        echo "$DIFF"
        exit 1
    fi
    
    # 清理临时 ID 文件
    rm *.ids
done

echo ""
echo "=========================================="
echo -e "${GREEN}All Tests Passed!${NC}"
echo "=========================================="