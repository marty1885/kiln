#!/usr/bin/env bash
set -e

KILN=$1

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

# Build all targets
$KILN test_basic test_modules test_lib

# Run the basic test
./build/debug/test_basic

# Run the modules test
./build/debug/test_modules

# Verify library was built
if [ ! -f build/debug/libtest_lib.a ]; then
    echo "ERROR: Library not built"
    exit 1
fi

echo "All tests passed!"
