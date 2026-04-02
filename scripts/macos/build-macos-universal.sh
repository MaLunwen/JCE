#!/usr/bin/env bash
# ================================================================
# build-macos-universal.sh -- Build JCE Universal Binary (x64 + arm64)
# Usage: build-macos-universal.sh [--clean]
# Output: build/desktop/macos-universal/JCE
# ================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

OUTPUT_DIR="build/desktop/macos-universal"
X64_BIN="build/desktop/macos-x64/CagedKingdom"
ARM64_BIN="build/desktop/macos-arm64/CagedKingdom"

# -- Handle --clean flag --
CLEAN_FLAG=""
if [[ "${1:-}" == "--clean" ]]; then
    CLEAN_FLAG="--clean"
    rm -rf "$OUTPUT_DIR"
fi

# -- Step 1: Build x64 --
echo "================================================================"
echo "  Building macOS x64..."
echo "================================================================"
bash "$SCRIPT_DIR/build-macos-x64.sh" $CLEAN_FLAG

# -- Step 2: Build arm64 --
echo ""
echo "================================================================"
echo "  Building macOS ARM64..."
echo "================================================================"
bash "$SCRIPT_DIR/build-macos-arm64.sh" $CLEAN_FLAG

# -- Step 3: Create Universal Binary --
echo ""
echo "=== Step 3: Create Universal Binary (lipo) ==="
if [[ ! -f "$X64_BIN" ]]; then
    echo "ERROR: x64 binary not found: $X64_BIN"
    exit 1
fi
if [[ ! -f "$ARM64_BIN" ]]; then
    echo "ERROR: arm64 binary not found: $ARM64_BIN"
    exit 1
fi

mkdir -p "$OUTPUT_DIR"
lipo -create -output "$OUTPUT_DIR/JCE" "$X64_BIN" "$ARM64_BIN"

echo ""
echo "[SUCCESS] Universal Binary created: $OUTPUT_DIR/JCE"
lipo -info "$OUTPUT_DIR/JCE"
