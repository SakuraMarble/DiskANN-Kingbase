cmake_minimum_required(VERSION 3.14)
project(VectorSearchEngine)

# ==========================================
# 1. 全局设置
# ==========================================
# 使用 C++17 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 默认使用 Release 模式（这对性能至关重要，否则 AVX2 和 Bitmap 计算会很慢）
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
    message(STATUS "Build type not specified: defaulting to Release.")
endif()

# ==========================================
# 2. 依赖项配置
# ==========================================

# A. CRoaring (从 third_party 目录)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/CRoaring ${CMAKE_CURRENT_BINARY_DIR}/CRoaring)

# B. OpenMP (用于并行搜索)
find_package(OpenMP REQUIRED)

# ==========================================
# 3. 目标：build_ivf_index (构建倒排索引)
# ==========================================
add_executable(build_ivf_index 
    build_ivf_index.cpp 
    InvertedIndex.cpp
)

target_link_libraries(build_ivf_index PRIVATE roaring::roaring)

# 即使是构建程序，开启优化也能加快大文件的处理速度
if(MSVC)
    target_compile_options(build_ivf_index PRIVATE /O2)
else()
    target_compile_options(build_ivf_index PRIVATE -O3 -march=native)
endif()


# ==========================================
# 4. 目标：search_ivf_index (暴力搜索 + SIMD)
# ==========================================
add_executable(search_ivf_index 
    search_ivf_index.cpp 
    InvertedIndex.cpp
)

# 链接 Roaring 和 OpenMP
target_link_libraries(search_ivf_index PRIVATE 
    roaring::roaring 
    OpenMP::OpenMP_CXX
)

# 关键优化选项 (AVX2 + OpenMP)
if(MSVC)
    # Windows/MSVC
    target_compile_options(search_ivf_index PRIVATE /arch:AVX2 /openmp /O2)
else()
    # Linux/Mac/GCC/Clang
    # -march=native: 自动启用当前 CPU 支持的所有指令集 (AVX2/AVX-512)
    # -fopenmp: 启用多线程
    # -O3: 最高级别优化
    target_compile_options(search_ivf_index PRIVATE -march=native -O3 -fopenmp)
endif()