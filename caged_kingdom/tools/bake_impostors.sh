#!/usr/bin/env bash
# Bake the octahedral impostor atlases the Hidden Cove trees use as their
# terminal LOD.  Reproducible: the atlases are generated artefacts, not
# hand-made assets, and without this script they would be files in the tree
# with no source -- which is exactly the gap gen_cove_ground.py was written to
# close for the ground texture.
#
# Usage:  caged_kingdom/tools/bake_impostors.sh
#
# TWO NON-OBVIOUS REQUIREMENTS, both found the hard way and both silent:
#
#   1. JCE_KPI_FRAME_LOG must be set.  The bake hook is gated on
#      `frame_kpi_index >= 20`, and that counter ONLY advances when the
#      KPI frame log is enabled.  Without it the bake never fires, produces no
#      file, and logs nothing at all.
#
#   2. MSYS_NO_PATHCONV=1 is required under Git Bash.  Otherwise MSYS rewrites
#      the model path inside JCE_IMPOSTOR_BAKE to backslashes, the renderer's
#      model-cache lookup is an exact strcmp against the scene's forward-slash
#      meshPath, and it misses.  The bake then reports the model as "not in the
#      renderer cache" -- which is true, and says nothing about why.
#
# The model must also be one the loaded scene actually draws: the bake reads it
# out of the renderer's live model cache, so it has to have been rendered.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EXE="$ROOT/build/desktop/windows-x64/release/jce_editor.exe"
SCENE="$ROOT/caged_kingdom/resources/assets/scenes/hidden_cove.scene.json"
OUT="$ROOT/caged_kingdom/resources/assets/impostors"
TMP="${TMPDIR:-/tmp}/jce_impostor_kpi.csv"

MODELS=(CommonTree_1 CommonTree_2 CommonTree_3 CommonTree_4 CommonTree_5
        Pine_1 Pine_5)

[ -x "$EXE" ] || { echo "editor not built: $EXE" >&2; exit 1; }
mkdir -p "$OUT"

for m in "${MODELS[@]}"; do
    # commontree_1 / pine_1 -- the name gen_hidden_cove.py expects.
    n="$(echo "$m" | tr 'A-Z' 'a-z' | sed 's/commontree/commontree_/; s/pine/pine_/; s/__/_/')"
    printf '%-16s ' "$m"
    MSYS_NO_PATHCONV=1 env \
        JCE_BACKEND=d3d11 \
        JCE_SCENE="$SCENE" \
        JCE_KPI_FRAME_LOG="$TMP" \
        JCE_KPI_FRAME_COUNT=400 \
        JCE_IMPOSTOR_BAKE="models/nature/$m.gltf;8;$OUT/$n.png;$OUT/$n.impostor.json;impostors/$n.png" \
        timeout 120 "$EXE" >/dev/null 2>&1 || true
    if [ -f "$OUT/$n.impostor.json" ]; then echo "OK"; else echo "FAILED"; exit 1; fi
done

rm -f "$TMP"
echo "baked ${#MODELS[@]} atlases into $OUT"
