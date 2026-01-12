# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT license.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

add_executable(build_memory_index build_memory_index.cpp)
target_link_libraries(build_memory_index ${PROJECT_NAME} ${DISKANN_TOOLS_TCMALLOC_LINK_OPTIONS} Boost::program_options)

add_executable(build_stitched_index build_stitched_index.cpp)
target_link_libraries(build_stitched_index ${PROJECT_NAME} ${DISKANN_TOOLS_TCMALLOC_LINK_OPTIONS} Boost::program_options)

add_executable(search_memory_index search_memory_index.cpp)
target_link_libraries(search_memory_index ${PROJECT_NAME} ${DISKANN_ASYNC_LIB} ${DISKANN_TOOLS_TCMALLOC_LINK_OPTIONS} Boost::program_options)

add_executable(build_disk_index build_disk_index.cpp)
target_link_libraries(build_disk_index ${PROJECT_NAME} ${DISKANN_TOOLS_TCMALLOC_LINK_OPTIONS} ${DISKANN_ASYNC_LIB} Boost::program_options)

add_executable(search_disk_index search_disk_index.cpp)
target_link_libraries(search_disk_index ${PROJECT_NAME} ${DISKANN_ASYNC_LIB} ${DISKANN_TOOLS_TCMALLOC_LINK_OPTIONS} Boost::program_options)

add_executable(range_search_disk_index range_search_disk_index.cpp)
target_link_libraries(range_search_disk_index ${PROJECT_NAME} ${DISKANN_ASYNC_LIB} ${DISKANN_TOOLS_TCMALLOC_LINK_OPTIONS} Boost::program_options)

add_executable(test_streaming_scenario test_streaming_scenario.cpp)
target_link_libraries(test_streaming_scenario ${PROJECT_NAME} ${DISKANN_TOOLS_TCMALLOC_LINK_OPTIONS} Boost::program_options)

add_executable(test_insert_deletes_consolidate test_insert_deletes_consolidate.cpp)
target_link_libraries(test_insert_deletes_consolidate ${PROJECT_NAME} ${DISKANN_TOOLS_TCMALLOC_LINK_OPTIONS} Boost::program_options)

# Hybrid search with inverted index
# Note: This requires the InvertedIndex implementation from test_ivf/
add_executable(hybrid_search_disk_index
    hybrid_search_disk_index.cpp
    ${CMAKE_SOURCE_DIR}/test_ivf/InvertedIndex.cpp
)
target_include_directories(hybrid_search_disk_index PRIVATE ${CMAKE_SOURCE_DIR}/test_ivf)
target_include_directories(hybrid_search_disk_index PRIVATE ${CMAKE_SOURCE_DIR}/third_party/CRoaring/include)
target_link_libraries(hybrid_search_disk_index
    ${PROJECT_NAME}
    ${DISKANN_ASYNC_LIB}
    ${DISKANN_TOOLS_TCMALLOC_LINK_OPTIONS}
    Boost::program_options
)

# Add AVX2 support for hybrid search
if(MSVC)
    target_compile_options(hybrid_search_disk_index PRIVATE /arch:AVX2 /openmp /O2)
else()
    target_compile_options(hybrid_search_disk_index PRIVATE -march=native -O3 -fopenmp)
endif()

# Link Roaring library (already built in main CMakeLists.txt)
if(TARGET roaring::roaring)
    target_link_libraries(hybrid_search_disk_index roaring::roaring)
else()
    message(WARNING "roaring::roaring target not found. Make sure CRoaring submodule is initialized.")
endif()

if (NOT MSVC)
    install(TARGETS build_memory_index
            build_stitched_index
            search_memory_index
            build_disk_index
            search_disk_index
            range_search_disk_index
            test_streaming_scenario
            test_insert_deletes_consolidate
            hybrid_search_disk_index
            RUNTIME
    )
endif()
