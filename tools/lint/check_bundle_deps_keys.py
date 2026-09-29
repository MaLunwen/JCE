#!/usr/bin/env python3
"""Every key a .mat.json can carry has been JUDGED: file, or not, with a reason.

WHY THIS EXISTS.  jce_bundle_deps.c's kAssetKeys[] is how the packer learns
that a string in a JSON document names a file that has to travel with it.  It
listed every texture alias a material can hold and did NOT list
customProgramVs / customProgramFs -- so a Shader Graph material shipped with
its .mat.json and without the two .bin blobs that make it a graph material.
The runtime then logged "custom shader blob(s) missing" and fell back to stock
PBR: the look an artist had authored was not in the build, the editor showed
it correctly, and nothing in the cook said a word.

That is the audit's signature shape -- right in the editor, different in the
shipped exe -- arriving through a table that nobody thinks to update.

WHY IT IS SHAPED LIKE THIS.  The first version asked "does this key LOOK like
a file?" (a name ending in Path, or one of the shapes already known) and said
nothing about anything else.  On 2026-09-07 the material gained a `parent`
key -- a path to another .mat.json, and the single most important one to
collect, because a variant without its parent renders its overrides on top of
the defaults.  The checker stayed green: `parent` does not look like a file,
so the regex never asked the question.  A gate that only fires on shapes it
already knows cannot catch the shape nobody thought of, which is the only kind
that ever ships.

So the rule is inverted.  Every key the material parser reads must appear in
JUDGED below with a verdict.  A NEW key is a FAILURE until somebody writes
down which it is -- the failure says "this checker has never judged this key",
not "this key is broken".  That is the point: the decision is forced at the
moment the key is added, by the person adding it.

FOUR SOURCES, because a .mat.json key can arrive by four routes: a literal
json_string() read, a literal jce_json_set_string() write, the saver's keep[]
preserve list, and the kTexKeys[] alias table that
jce_pbr_material_texture_keys() hands out.  The first version read only the
literal reads, so a new texture alias was invisible to it as well -- and
`shaderGraph` is WRITTEN and never read, so a read-only view calls the most
important graph key stale and invites its removal.

Exit 0 = every material key is judged, and every file-valued one is
collectable by the packer.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAT = ROOT / "engine" / "src" / "renderer" / "jce_pbr_material.c"
DEPS = ROOT / "engine" / "src" / "resource" / "jce_bundle_deps.c"

# key -> None if it names a FILE (and so must be in kAssetKeys), else the
# reason it does not.  Adding a key to the material parser without adding it
# here is the failure this checker exists to produce.
JUDGED = {
    # --- files -------------------------------------------------------
    "customProgramVs": None,
    "customProgramFs": None,
    "shaderGraph":     None,
    "parent":          None,
    "albedoMap":            None,
    "baseColorMap":         None,
    "diffuseMap":           None,
    "mainTexture":          None,
    "metallicRoughnessMap": None,
    "metallicMap":          None,
    "normalMap":            None,
    "aoMap":                None,
    "occlusionMap":         None,
    "emissiveMap":          None,
    "emissionMap":          None,
    # --- not files ---------------------------------------------------
    "alphaMode": "an enum spelled OPAQUE / MASK / BLEND, not a path",
    "type":      "the document discriminator, always the literal \"pbr\"",
}


def read(p):
    return p.read_text(encoding="utf-8", errors="replace")


def material_string_keys(src):
    """Every string key a .mat.json can carry, from all FOUR sources.

    Reading alone is not the question.  `shaderGraph` is written by the saver
    and never read by the loader, and it names a file the packer has to
    collect -- a read-only view of the parser judged it "no longer read" and
    would have had it deleted from this list.  So: literal json_string()
    reads, literal jce_json_set_string() writes, the keep[] preserve list, and
    the per-slot texture alias table (matched by its declaration, so a new
    slot or alias is picked up without touching this checker).
    """
    keys = set(re.findall(r'json_string\(\s*\w+\s*,\s*"([^"]+)"\s*\)', src))
    keys |= set(re.findall(
        r'jce_json_set_string\(\s*\w+\s*,\s*"([^"]+)"', src))
    for m in re.finditer(r"const char \*const keep\[\d+\]\s*=\s*\{(.*?)\};",
                         src, re.S):
        keys.update(re.findall(r'"([^"]+)"', m.group(1)))
    for m in re.finditer(r"static const char \*const k(\w*Keys)\[\]\s*=\s*\{"
                         r"(.*?)\};", src, re.S):
        keys.update(re.findall(r'"([^"]+)"', m.group(2)))
    return keys


def asset_keys(src):
    m = re.search(r"kAssetKeys\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not m:
        return None
    return set(re.findall(r'"([^"]+)"', m.group(1)))


def main():
    mat_src, deps_src = read(MAT), read(DEPS)
    keys = material_string_keys(mat_src)
    if not keys:
        print("check_bundle_deps_keys: FAIL - found no material keys in %s; "
              "the parser this check reads has changed shape and the check is "
              "now blind." % MAT.relative_to(ROOT))
        return 1
    known = asset_keys(deps_src)
    if known is None:
        print("check_bundle_deps_keys: FAIL - could not find kAssetKeys[] in "
              "%s." % DEPS.relative_to(ROOT))
        return 1

    unjudged = sorted(k for k in keys if k not in JUDGED)
    missing = sorted(k for k in keys
                     if k in JUDGED and JUDGED[k] is None and k not in known)
    stale = sorted(k for k in JUDGED if k not in keys)

    if unjudged:
        print("check_bundle_deps_keys: FAIL - this checker has never judged "
              "these .mat.json key(s):")
        for k in unjudged:
            print("    %s" % k)
        print("  Add each to JUDGED in %s: None if it names a FILE (and then "
              "also to kAssetKeys[] in %s), or a one-line reason if it does "
              "not." % (Path(__file__).name, DEPS.relative_to(ROOT)))
        print("  A material key the packer does not know is a look that "
              "renders in the editor and reverts to stock PBR when shipped.")
        return 1

    if missing:
        print("check_bundle_deps_keys: FAIL - a .mat.json key names a file "
              "the packer will not collect:")
        for k in missing:
            print("    %s" % k)
        print("  Add it to kAssetKeys[] in %s." % DEPS.relative_to(ROOT))
        return 1

    if stale:
        print("check_bundle_deps_keys: FAIL - JUDGED names key(s) the "
              "material parser no longer reads: %s" % ", ".join(stale))
        print("  Remove them, so this list cannot drift into a record of what "
              "the parser used to do.")
        return 1

    files = sorted(k for k in keys if JUDGED[k] is None)
    print("check_bundle_deps_keys: OK (%d material key(s) judged; %d name "
          "files, all collectable)" % (len(keys), len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
