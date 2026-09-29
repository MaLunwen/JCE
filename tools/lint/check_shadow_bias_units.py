#!/usr/bin/env python3
"""check_shadow_bias_units.py -- the shadow depth bias is a distance, in texels.

It used to be a constant in NORMALIZED shadow-map depth.  A shadow map stores
depth normalised over its cascade's orthographic range, so the same authored
number was a different WORLD distance in every cascade -- 89 mm in cascade 0
and 8969 mm in cascade 3 of one measured frame, a 10.4x span nobody chose --
and it moved when a project changed its Shadow Distance (89 mm at 300 m,
17 mm at 40 m).  Two hand-tuned ladders had grown on top of it to compensate,
one constant at a time.

Both sites now go through jce_shadow_depth_bias() in shadow_bias.sh, which
converts a number of shadow TEXELS into normalised depth using the cascade's
own scale (u_csmPenumbra, already uploaded for the contact-hardening search).

WHY A LINT AND NOT ONLY A TEST.  tests/renderer/test_jce_csm_bias_units.c can
check that the bridge means what it claims -- it reads jce_csm_compute's own
outputs -- but it cannot check that the SHADER still divides by it.  Nothing
about a picture says which formula produced it, and the failure this guards is
somebody reinstating `inv_map_size * ...` in one of the two sites while the
other keeps the texel form: the terrain and the meshes would then disagree
about what a bias means, in the same frame, on the same ground.  That is this
tree's recurring shape -- a working half that hides a broken one.

WHAT IS CHECKED
  1. Both bias sites call jce_shadow_depth_bias(, on COMMENT-STRIPPED source.
  2. Neither derives a depth bias from inv_map_size any more.
  3. shadow_bias.sh divides by pcss_scale_for() -- the bridge itself.
  4. The sites are found at all: a file list that silently matches nothing is
     a gate that cannot fail, which this repository has shipped before.

EXIT CODES
    0  both sites express the bias in texels
    1  one does not, or the bridge is gone
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PBR = ROOT / "engine" / "shaders" / "pbr"
HEADER = PBR / "shadow_bias.sh"

# The two places a cascade depth bias is computed: the terrain/water sampler
# and the mesh one.  They are listed rather than globbed because a THIRD site
# appearing is itself the thing to notice -- a new copy of this decision is how
# the two existing ones came to disagree about their multipliers in the first
# place.
SITES = ["csm_shadow.sh", "fs_pbr_decl.sh"]

CALL = "jce_shadow_depth_bias("


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def main():
    failures = []

    if not HEADER.is_file():
        print("FAIL: %s is missing" % HEADER.relative_to(ROOT))
        return 1
    header = strip_comments(HEADER.read_text(encoding="utf-8"))
    if "pcss_scale_for(" not in header:
        failures.append(
            "%s no longer divides by pcss_scale_for(): that quotient is the "
            "only thing converting shadow texels into normalised depth, and "
            "without it the number is back to meaning whatever the cascade "
            "fit produced" % HEADER.relative_to(ROOT))

    seen = 0
    for name in SITES:
        p = PBR / name
        rel = p.relative_to(ROOT).as_posix()
        if not p.is_file():
            failures.append("%s is missing -- this check names its sites, so a "
                            "moved file must move here too" % rel)
            continue
        seen += 1
        code = strip_comments(p.read_text(encoding="utf-8"))
        if CALL not in code:
            failures.append(
                "%s does not call %s -- its depth bias is not expressed in "
                "shadow texels, so it means a different distance in every "
                "cascade" % (rel, CALL))
        # The old form, in any spelling that still multiplies a bias by the
        # inverse map size.  `inv_map_size` legitimately survives for the PCF
        # tap offsets; what may not come back is a BIAS built from it.
        for m in re.finditer(r"(\w*depth_bias\w*)\s*=\s*([^;]*)", code):
            if "inv_map_size" in m.group(2):
                failures.append(
                    "%s computes %s from inv_map_size: that is the normalized "
                    "form this check exists to keep out"
                    % (rel, m.group(1)))

    if seen == 0:
        failures.append(
            "none of the named bias sites (%s) were found under %s -- this "
            "check is not checking anything"
            % (", ".join(SITES), PBR.relative_to(ROOT)))

    if failures:
        for f in failures:
            print("FAIL: %s" % f)
        return 1
    print("OK: %d cascade depth-bias site(s) express the bias in shadow texels "
          "(%s)" % (seen, ", ".join(SITES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
