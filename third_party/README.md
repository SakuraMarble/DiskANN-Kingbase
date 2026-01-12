# Third-Party Dependencies

This directory contains external dependencies managed as Git submodules.

## CRoaring

**Repository**: https://github.com/RoaringBitmap/CRoaring

**Description**: Roaring Bitmaps library - compressed bitmap data structure for efficient set operations.

**Used by**:
- `test_ivf/` - Inverted index implementation
- `apps/hybrid_search_disk_index.cpp` - Hybrid search application

**License**: Apache License 2.0

## How to Update Submodules

### Initial Setup

When cloning the repository for the first time:

```bash
# Option 1: Clone with submodules
git clone --recursive <repository-url>

# Option 2: Clone then initialize submodules
git clone <repository-url>
cd DiskANN-Kingbase
git submodule init
git submodule update
```

### Update to Latest Version

To update CRoaring to the latest commit:

```bash
cd third_party/CRoaring
git pull origin master
cd ../..
git add third_party/CRoaring
git commit -m "Update CRoaring to latest version"
```

### Update All Submodules

To update all submodules to their latest commits:

```bash
git submodule update --remote --recursive
```

## Adding New Submodules

To add a new third-party library as a submodule:

```bash
git submodule add <repository-url> third_party/<library-name>
git commit -m "Add <library-name> as submodule"
```

## Build Integration

The `third_party/CRoaring` directory is automatically included in the CMake build process:

- **apps/CMakeLists.txt**: Links `roaring::roaring` to `hybrid_search_disk_index`
- **test_ivf/CMakeLists.txt**: Links `roaring::roaring` to IVF index tools

No manual build steps are required for CRoaring when building the main project.
