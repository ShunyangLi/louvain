#!/bin/bash
# Test script for Louvain extension
#
# Usage:
#   ./run_test.sh                         # Run install test (small inline data)
#   ./run_test.sh --ldbc <data_path>      # Run LDBC SF01 test
#   ./run_test.sh --ldbc <data_path> --no-reload  # Reuse existing DB

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

print_header() {
    echo "============================================"
    echo "  $1"
    echo "============================================"
}

print_header "Louvain Extension Test Script"
echo "Project root: ${PROJECT_ROOT}"
echo "Build directory: ${BUILD_DIR}"
echo ""

# Parse arguments
RUN_LDBC=false
LDBC_DATA_PATH=""
NO_RELOAD=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --ldbc)
            RUN_LDBC=true
            LDBC_DATA_PATH="$2"
            shift 2
            ;;
        --no-reload)
            NO_RELOAD="--no-reload"
            shift
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--ldbc <data_path>] [--no-reload]"
            exit 1
            ;;
    esac
done

# Step 1: Build
if [ ! -d "${BUILD_DIR}" ]; then
    echo "Build directory does not exist. Creating..."
    mkdir -p "${BUILD_DIR}"
fi

print_header "Step 1: Building the project"
cd "${BUILD_DIR}"

if [ ! -f "CMakeCache.txt" ]; then
    echo "Configuring CMake..."
    cmake .. -DCMAKE_BUILD_TYPE=Release \
             -DBUILD_TEST=ON \
             -DBUILD_EXTENSIONS="louvain"
fi

if [ "$RUN_LDBC" = true ]; then
    echo "Building test_louvain_ldbc..."
    make -j$(nproc) test_louvain_ldbc 2>&1 | tail -10
else
    echo "Building test_louvain_install..."
    make -j$(nproc) test_louvain_install 2>&1 | tail -10
fi

echo ""
echo "Build completed."
echo ""

# Tell LOAD where to find the locally-built extension
export NEUG_EXTENSION_HOME_PYENV="${BUILD_DIR}"

# Step 2: Run tests
if [ "$RUN_LDBC" = true ]; then
    print_header "Step 2: Running LDBC Louvain Test"

    if [ -z "$LDBC_DATA_PATH" ]; then
        echo "ERROR: --ldbc requires a data path argument"
        exit 1
    fi

    TEST_EXECUTABLE="${BUILD_DIR}/extension/louvain/test/test_louvain_ldbc"
    if [ ! -f "${TEST_EXECUTABLE}" ]; then
        echo "ERROR: Test executable not found at: ${TEST_EXECUTABLE}"
        exit 1
    fi

    echo "Running: ${TEST_EXECUTABLE} ${LDBC_DATA_PATH} ${NO_RELOAD}"
    echo ""
    "${TEST_EXECUTABLE}" "${LDBC_DATA_PATH}" ${NO_RELOAD}
else
    print_header "Step 2: Running Install Test"

    TEST_EXECUTABLE="${BUILD_DIR}/extension/louvain/test/test_louvain_install"
    if [ ! -f "${TEST_EXECUTABLE}" ]; then
        echo "ERROR: Test executable not found at: ${TEST_EXECUTABLE}"
        exit 1
    fi

    echo "Running: ${TEST_EXECUTABLE}"
    echo ""
    "${TEST_EXECUTABLE}"
fi

echo ""
print_header "Done"
