#!/bin/bash
# Build script with compiler path fix

set -e  # Exit on error

echo "========================================"
echo "DiskANN-Kingbase Build Script"
echo "========================================"

# Get absolute paths for compilers
GCC_PATH=$(which gcc)
GXX_PATH=$(which g++)

echo "Using compilers:"
echo "  GCC: $GCC_PATH"
echo "  G++: $GXX_PATH"

# Clean build directory
echo ""
echo "Cleaning build directory..."
cd "$(dirname "$0")"
rm -rf build
mkdir -p build
cd build

# Run CMake with absolute paths
echo ""
echo "Running CMake..."
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER="$GCC_PATH" \
      -DCMAKE_CXX_COMPILER="$GXX_PATH" \
      -DOMP_PATH=/opt/intel/oneapi/compiler/2025.3/lib \
      ..

if [ $? -eq 0 ]; then
    echo ""
    echo "========================================"
    echo "CMake configuration successful!"
    echo "========================================"
    echo ""
    echo "To build, run:"
    echo "  cd build"
    echo "  make -j\$(nproc)"
    echo ""
    echo "To build only hybrid_search_disk_index:"
    echo "  make hybrid_search_disk_index -j\$(nproc)"
else
    echo ""
    echo "========================================"
    echo "CMake configuration failed!"
    echo "========================================"
    exit 1
fi
