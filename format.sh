#!/usr/bin/env bash
# Format (or check) all C++ source with clang-format.
#
# Usage:
#   ./format.sh                 Format files in place.
#   ./format.sh --check         Report unformatted files without modifying (exit 1 if any).
#   ./format.sh install-hooks   Point git at githooks/ so the pre-commit hook runs.
#
# The file set and exclusions mirror .github/workflows/format.yml exactly.
# Keep the clang-format version in sync with that workflow (clang-format 22.1.5).
set -euo pipefail

cd "$(dirname "$0")"

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

if [[ "${1:-}" == "install-hooks" ]]; then
    git config core.hooksPath githooks
    echo "Installed git hooks (core.hooksPath -> githooks/)."
    exit 0
fi

if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
    echo "error: '$CLANG_FORMAT' not found. Install clang-format (CI pins 22.1.5)." >&2
    exit 1
fi

# Excludes vendored code: linenoise-ng, kiln/inner (third-party hashes),
# and tests/integration fixtures (sample projects, not our source).
find_sources() {
    find kiln kiln-cli tests benchmark \
        -path '*/linenoise-ng/*' -prune -o \
        -path 'kiln/inner/*' -prune -o \
        -path 'tests/integration/*' -prune -o \
        \( -name '*.cpp' -o -name '*.hpp' -o -name '*.cc' \) -print0
}

if [[ "${1:-}" == "--check" ]]; then
    find_sources | xargs -0 "$CLANG_FORMAT" --dry-run --Werror --verbose
else
    find_sources | xargs -0 "$CLANG_FORMAT" -i
    echo "Formatted all C++ sources."
fi
