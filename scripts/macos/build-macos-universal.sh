#!/usr/bin/env bash
# ================================================================
# build-macos-universal.sh -- Build JCE Universal Binary (x64 + arm64)
# Usage: build-macos-universal.sh [--clean] [--dist]
# Output: build/desktop/macos-universal/jce_editor
# ================================================================
source "$(dirname "$0")/../lib/jce_common.sh"
OUTPUT_DIR="build/desktop/macos-universal"

# -- Parse flags --
CLEAN_FLAG=""
DIST_FLAG=""
VARIANT="release"
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN_FLAG="--clean"; rm -rf "$OUTPUT_DIR" ;;
        --dist)  DIST_FLAG="--dist"; VARIANT="dist" ;;
    esac
done
X64_BIN="build/desktop/macos-x64/$VARIANT/jce_editor"
ARM64_BIN="build/desktop/macos-arm64/$VARIANT/jce_editor"

# -- Step 1: Build x64 --
echo "================================================================"
echo "  Building macOS x64..."
echo "================================================================"
bash "$SCRIPT_DIR/build-macos-x64.sh" $CLEAN_FLAG $DIST_FLAG

# -- Step 2: Build arm64 --
echo ""
echo "================================================================"
echo "  Building macOS ARM64..."
echo "================================================================"
bash "$SCRIPT_DIR/build-macos-arm64.sh" $CLEAN_FLAG $DIST_FLAG

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
lipo -create -output "$OUTPUT_DIR/jce_editor" "$X64_BIN" "$ARM64_BIN"

echo ""
echo "[SUCCESS] Universal Binary created ($VARIANT): $OUTPUT_DIR/jce_editor"
lipo -info "$OUTPUT_DIR/jce_editor"
_jce_success_wait
