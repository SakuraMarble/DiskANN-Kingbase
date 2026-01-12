# Build Fix for CRoaring Compiler Issue

## Problem

When building DiskANN-Kingbase, you may encounter this error:

```
CMake Error at third_party/CRoaring/CMakeLists.txt:2 (project):
  The CMAKE_CXX_COMPILER:
    g++
  is not a full path and was not found in the PATH.
```

## Root Cause

CMake passes relative compiler names (like "g++") to subdirectories, but CRoaring's CMakeLists.txt requires absolute paths.

## Solution Applied

The main `CMakeLists.txt` now:
1. Detects if compiler paths are relative
2. Converts them to absolute paths using `find_program`
3. Adds CRoaring subdirectory with corrected paths

## How to Build

### Method 1: Use the build script (Recommended)

```bash
cd /root/mbj/DiskANN-Kingbase
chmod +x build_with_fix.sh
./build_with_fix.sh

# If successful, build the project
cd build
make -j$(nproc)
```

### Method 2: Manual CMake with absolute paths

```bash
cd /root/mbj/DiskANN-Kingbase
rm -rf build && mkdir build && cd build

cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=$(which gcc) \
      -DCMAKE_CXX_COMPILER=$(which g++) \
      -DOMP_PATH=/opt/intel/oneapi/compiler/2025.3/lib \
      ..

make -j$(nproc)
```

### Method 3: Build only hybrid search

```bash
cd /root/mbj/DiskANN-Kingbase/build
make hybrid_search_disk_index -j$(nproc)
```

## Verification

After CMake succeeds, you should see:

```
-- CRoaring found and will be built
-- Configuring done
-- Generating done
```

## If Still Failing

1. **Check submodule initialization:**
   ```bash
   git submodule update --init --recursive
   ls -la third_party/CRoaring/
   ```

2. **Verify compilers exist:**
   ```bash
   which gcc
   which g++
   gcc --version
   g++ --version
   ```

3. **Try alternative compiler specification:**
   ```bash
   export CC=/usr/bin/gcc
   export CXX=/usr/bin/g++
   cmake -DCMAKE_BUILD_TYPE=Release ..
   ```

4. **Check CMake version:**
   ```bash
   cmake --version  # Should be >= 3.14
   ```

## Changes Made

### CMakeLists.txt (main)
- Added compiler path resolution before `add_subdirectory(third_party/CRoaring)`
- Added check for CRoaring existence

### apps/CMakeLists.txt
- Removed duplicate `add_subdirectory(CRoaring)`
- Added conditional linking to roaring::roaring target

## Notes

- The fix is automatic and does not require manual intervention
- CRoaring is built as part of the main project (not separately)
- The EXCLUDE_FROM_ALL flag prevents building unused CRoaring targets
