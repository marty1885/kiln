#!/bin/bash
set -e

KILN=$1
TEST_DIR=$(cd "$(dirname "$0")" && pwd)

cd "$TEST_DIR"

# C++20 modules via -fmodules-ts: GCC 11+ only.
CXX_BIN=${CXX:-g++}
if ! command -v "$CXX_BIN" >/dev/null 2>&1; then
    echo "$CXX_BIN not found; skipping"
    exit 0
fi
if ! "$CXX_BIN" --version 2>/dev/null | head -n1 | grep -qi 'g++\|gcc'; then
    echo "$CXX_BIN is not GCC; -fmodules-ts is GCC-specific; skipping"
    exit 0
fi
gcc_major=$("$CXX_BIN" -dumpfullversion -dumpversion 2>/dev/null | cut -d. -f1)
if [ -z "$gcc_major" ] || [ "$gcc_major" -lt 11 ]; then
    echo "GCC $gcc_major lacks usable -fmodules-ts (need 11+); skipping"
    exit 0
fi

# Clean any previous build
rm -rf build

# Build with kiln
echo "Building modules_basic with kiln..."
$KILN -j4

# Check that output exists
if [ ! -f build/debug/modules_basic_app ]; then
    echo "Error: modules_basic_app not built"
    exit 1
fi

# Run the executable
echo "Running modules_basic_app..."
./build/debug/modules_basic_app

echo "Test passed!"
