#!/usr/bin/env python3
"""
check_public_abi.py — type-level purity guard for the public C ABI.

tools/lint/check_public_api_purity.py already rejects third-party
`#include`s in engine/include/jce/**.  This complementary checker catches
what an include scan cannot: third-party / C++ / platform TYPE NAMES that
appear in public declarations even without an include (via forward
declarations like `typedef struct ecs_world_t ecs_world_t;` or
`struct bgfx_memory_s;`).  That was the exact hole the dependency audit
found — flecs `ecs_world_t*` and bgfx `bgfx_memory_s*` were reachable in
public signatures while the include guard stayed green.

Scope: every header under engine/include/jce/ (the ABI surface).  Comments
and strings are stripped before matching.  A curated allow-list carries the
handful of SDL value-compatibility comments and sanctioned tokens.

Rule of thumb enforced:
  * No `namespace`, `template`, `class`, or `std::` in public headers.
  * No third-party type name (ecs_*, bgfx_*, bt*, b2*, ma_*, ENet*,
    lua_State, JNIEnv, FT_*, hb_*, PHYSFS_*, cJSON, RmlUi, ozz::, dt*/rc*).
  * No non-portable platform type (HWND, HANDLE, Vk*, ID3D*, NSWindow).
Forward-declaring a JCE-owned opaque struct (`typedef struct JceX JceX;`)
is fine — that is exactly how handles are meant to cross the ABI.

Usage:
  python tools/audit/check_public_abi.py [--json]
  # exit 0 clean, 1 violations.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PUBLIC_ROOT = REPO_ROOT / "engine" / "include" / "jce"

# (compiled pattern, human reason).  Patterns run against comment-stripped
# code only.  Word boundaries keep JCE-owned names (JceEntity) clear.
BANNED_TOKENS: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"\bstd::"), "C++ std:: in public C ABI"),
    (re.compile(r"\bnamespace\b"), "C++ namespace in public C ABI"),
    (re.compile(r"\btemplate\s*<"), "C++ template in public C ABI"),
    # third-party ECS / graphics / physics / audio / net / script / font types
    (re.compile(r"\becs_world_t\b|\becs_entity_t\b|\becs_query_t\b|\becs_id_t\b"),
     "flecs type in public ABI (ECS world is private; pass opaque void*/handle)"),
    (re.compile(r"\bbgfx_\w+_(?:handle|t)\b|\bbgfx_memory_s\b|\bbgfx::"),
     "bgfx type in public ABI (use a JCE handle)"),
    (re.compile(r"\bbtVector3\b|\bbtRigidBody\b|\bbtCollisionObject\b|\bbtScalar\b|\bbtTransform\b"),
     "Bullet type in public ABI"),
    (re.compile(r"\bb2World\b|\bb2Body\b|\bb2Fixture\b|\bb2Vec2\b"),
     "Box2D type in public ABI"),
    (re.compile(r"\bma_(?:engine|sound|device|decoder|node)\b"),
     "miniaudio type in public ABI"),
    (re.compile(r"\bENetPeer\b|\bENetPacket\b|\bENetHost\b|\bENetEvent\b"),
     "ENet type in public ABI"),
    (re.compile(r"\blua_State\b|\blua_CFunction\b"),
     "Lua type in public ABI (script bridge is private)"),
    (re.compile(r"\bJNIEnv\b|\bjobject\b|\bjclass\b"),
     "JNI type in public ABI (JNI bridge is a consumer)"),
    (re.compile(r"\bFT_(?:Face|Library|GlyphSlot)\b|\bhb_(?:font|buffer|face)_t\b"),
     "FreeType/HarfBuzz type in public ABI"),
    (re.compile(r"\bPHYSFS_\w+\b"), "PhysFS type in public ABI"),
    (re.compile(r"\bcJSON\b"), "cJSON type in public ABI (use JceJson opaque)"),
    (re.compile(r"\bRml(?:Ui)?::|\bRml_\w+"), "RmlUi type in public ABI"),
    (re.compile(r"\bozz::"), "ozz-animation type in public ABI"),
    (re.compile(r"\bdtNavMesh\b|\bdtNavMeshQuery\b|\brcConfig\b|\brcContext\b"),
     "Recast/Detour type in public ABI"),
    # non-portable platform types
    (re.compile(r"\bHWND\b|\bHANDLE\b|\bHINSTANCE\b|\bHDC\b"),
     "Win32 handle type in public ABI"),
    (re.compile(r"\bVk[A-Z]\w+\b|\bID3D1[012]\w*\b|\bNSWindow\b|\bCAMetalLayer\b"),
     "native graphics type in public ABI"),
]

# Allow-list: substrings that neutralise a match on a given line (sanctioned).
# The JceJson opaque alias is the one intentional exception — cJSON appears
# only inside jce_json.h's documented opaque typedef, tracked separately.
LINE_ALLOW = [
    # jce_json.h documents the opaque alias in prose/typedef; that specific
    # file is exempted below by name.
]
# Files exempted entirely (documented ADR-level exceptions).
FILE_ALLOW = {
    # jce_json.h intentionally aliases cJSON as an opaque handle; the alias is
    # the sanctioned facade seam. Tracked as an accepted exception in the
    # dependency audit (JSON facade). No third-party FIELD is ever exposed.
    "engine/include/jce/os/core/jce_json.h",
}

INCLUDE_RE = re.compile(r'^\s*#\s*(include|error|pragma|warning)\b')


def strip_comments(text: str) -> list[str]:
    """Return per-line code with /*...*/ and // comments blanked."""
    out = []
    in_block = False
    for line in text.splitlines():
        buf = []
        i = 0
        while i < len(line):
            if in_block:
                end = line.find("*/", i)
                if end == -1:
                    i = len(line); break
                in_block = False; i = end + 2
            else:
                start = line.find("/*", i)
                cpp = line.find("//", i)
                if cpp != -1 and (start == -1 or cpp < start):
                    buf.append(line[i:cpp]); i = len(line); break
                if start == -1:
                    buf.append(line[i:]); i = len(line)
                else:
                    buf.append(line[i:start]); in_block = True; i = start + 2
        out.append("".join(buf))
    return out


def scan():
    violations = []
    for r, _dirs, files in os.walk(PUBLIC_ROOT):
        for fn in files:
            if not fn.endswith((".h", ".hpp", ".hh", ".hxx")):
                continue
            p = Path(r) / fn
            rel = p.relative_to(REPO_ROOT).as_posix()
            if rel in FILE_ALLOW:
                continue
            try:
                code_lines = strip_comments(p.read_text(encoding="utf-8", errors="replace"))
            except OSError:
                continue
            for lineno, code in enumerate(code_lines, 1):
                if INCLUDE_RE.match(code):
                    continue  # includes are the other linter's job
                if any(a in code for a in LINE_ALLOW):
                    continue
                for pat, reason in BANNED_TOKENS:
                    if pat.search(code):
                        violations.append({
                            "file": rel, "line": lineno,
                            "reason": reason, "code": code.strip()[:120],
                        })
                        break
    return violations


# --------------------------------------------------------------------------
# Raw-handle ratchet (audit: rend-08-raw-idx-passthrough)
#
# The renderer's public headers pass bgfx handle *indices* around as bare
# uint16_t (`prog_idx`, `bone_tex_idx`, `texture_idx`, ...).  That keeps the
# headers bgfx-free, which is the right goal, but it throws away type safety
# to get there: nothing stops a caller handing a shader index to a parameter
# that wants a texture index, and it compiles.  jce_gfx_types.h already
# defines the correct mechanism — JceShaderHandle / JceTextureHandle /
# JceUniformHandle, each a struct{uint16_t} — which is equally bgfx-free AND
# distinct at compile time.
#
# Migrating the existing declarations means changing public signatures, i.e.
# a source-breaking change for SDK consumers; that is a deliberate release
# decision, not something to slip into an audit batch.  So this is a RATCHET:
# the current count is frozen, and any NEW bare-handle parameter fails the
# gate.  Lower the number as declarations migrate; it must never rise.
# --------------------------------------------------------------------------

RAW_HANDLE_ROOT = REPO_ROOT / "engine" / "include" / "jce" / "renderer"
RAW_HANDLE_RE = re.compile(
    r"\buint16_t\s+\w*(?:idx|handle)\b", re.IGNORECASE)
# 54 -> 53: the frozen number was one ABOVE the actual count, so exactly one
# new bare-handle parameter could have been added with the gate still green --
# a ratchet with slack in it admits the next declaration silently, which is the
# one thing it exists to prevent.  Measured at HEAD before and after the soft-
# particle API landed: 53 both times (JceGpuParticleSoft.depth_texture and
# jce_gpu_particles_render_soft's texture parameter are JceTextureHandle,
# because this gate asked for that and it is the better signature).
RAW_HANDLE_BASELINE = 53


def scan_raw_handles():
    """Count bare uint16_t handle/index parameters in public renderer headers."""
    hits = []
    if not RAW_HANDLE_ROOT.is_dir():
        return hits
    for r, _dirs, files in os.walk(RAW_HANDLE_ROOT):
        for fn in sorted(files):
            if not fn.endswith((".h", ".hpp")):
                continue
            p = Path(r) / fn
            rel = p.relative_to(REPO_ROOT).as_posix()
            try:
                code_lines = strip_comments(p.read_text(encoding="utf-8", errors="replace"))
            except OSError:
                continue
            for lineno, code in enumerate(code_lines, 1):
                # `typedef struct { uint16_t idx; } JceXHandle;` IS the fix,
                # not the problem — skip the wrapper definitions themselves.
                if "typedef" in code:
                    continue
                for m in RAW_HANDLE_RE.finditer(code):
                    hits.append({"file": rel, "line": lineno,
                                 "token": m.group(0), "code": code.strip()[:110]})
    return hits


# --------------------------------------------------------------------------
# Unexported public prototypes
#
# A jce_* prototype in engine/include/jce/** without JCE_API is not, today, a
# link failure: JCE_API expands to nothing unless JCE_SHARED is defined, and
# nothing in this build system defines it.  The finding that raised this
# claimed consumers "cannot link", which is not true of a static SDK — so that
# is not why this gate exists.
#
# The real consequence is next door.  check_abi_snapshot.py keys on JCE_API,
# so an unexported prototype is INVISIBLE to the ABI freeze: it can be deleted
# or resignatured with the gate staying green.  178 public functions across 50
# headers were in that blind spot; annotating them took the snapshot from 4067
# to 4245 declarations with zero removals and zero signature changes — the
# delta matching the count exactly is what proves nothing else moved.
#
# Holding this at zero is what stops the blind spot growing back.
# --------------------------------------------------------------------------

PROTO_RE = re.compile(
    r"^(?P<ret>[A-Za-z_][\w \t\*]*?)[ \t\*]+(?P<name>jce_\w+)[ \t]*"
    r"\((?P<args>[^;{}]*)\)[ \t]*;",
    re.M)


def _mask_comments(t: str) -> str:
    """Blank comments while preserving offsets, so matches map to the source."""
    out = list(t)
    i, n = 0, len(t)
    while i < n:
        if t.startswith("/*", i):
            e = t.find("*/", i + 2)
            e = n if e < 0 else e + 2
            for k in range(i, e):
                if out[k] != "\n":
                    out[k] = " "
            i = e
        elif t.startswith("//", i):
            e = t.find("\n", i)
            e = n if e < 0 else e
            for k in range(i, e):
                out[k] = " "
            i = e
        else:
            i += 1
    return "".join(out)


def scan_unexported():
    """Public jce_* prototypes missing JCE_API."""
    hits = []
    for r, _dirs, files in os.walk(PUBLIC_ROOT):
        for fn in sorted(files):
            if not fn.endswith((".h", ".hpp")):
                continue
            p = Path(r) / fn
            rel = p.relative_to(REPO_ROOT).as_posix()
            try:
                raw = p.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            masked = _mask_comments(raw)
            for m in PROTO_RE.finditer(masked):
                ret = m.group("ret")
                if "typedef" in ret or "JCE_API" in ret or "#" in ret:
                    continue
                line = raw[:m.start("name")].count("\n") + 1
                hits.append({"file": rel, "line": line, "name": m.group("name")})
    return hits


# --------------------------------------------------------------------------
# UI convergence guard (ADR-0002, audit UI-01)
#
# The engine ships TWO retained-mode UI systems and that is deliberate: the
# ECS Canvas owns scene UI (entities, prefabs, serialization, per-entity
# script access) and RmlUI owns document UI (flow/flex layout, CSS cascade).
# Neither can take the other's job without becoming it, which is exactly why
# both exist — see docs/architecture/adr-0002-ui-ownership-split.md.
#
# The risk of keeping two is not that there are two.  It is that each grows
# into the other, at which point they ARE duplicate implementations.  Two
# machine-checkable invariants keep the split honest:
#
#   RmlUI's public API must stay free of JceEntity / JceScene.  The moment it
#   binds to entities it is a second scene-UI system.
#
#   The ECS Canvas's public API must stay free of document/stylesheet
#   concepts.  The moment it grows a cascade it is a second layout engine.
# --------------------------------------------------------------------------

UI_CONVERGENCE = [
    ("engine/include/jce/middleware/ui/jce_ui.h",
     re.compile(r"\bJceEntity\b|\bJceScene\b"),
     "RmlUI facade referencing a scene entity — that is the ECS Canvas's job "
     "(ADR-0002); binding documents to entities makes this a second scene UI"),
    ("engine/include/jce/middleware/scene/jce_ui_canvas.h",
     re.compile(r"\bRml\w*|\bcss\b|\bstylesheet\b|\bJceUIDocument\b", re.I),
     "ECS Canvas referencing a document/stylesheet concept — that is RmlUI's "
     "job (ADR-0002); growing a cascade here makes this a second layout engine"),
]


def scan_ui_convergence():
    """The two UI systems must not grow into each other."""
    hits = []
    for rel, pat, reason in UI_CONVERGENCE:
        p = REPO_ROOT / rel
        if not p.is_file():
            continue
        try:
            code_lines = strip_comments(p.read_text(encoding="utf-8",
                                                    errors="replace"))
        except OSError:
            continue
        for lineno, code in enumerate(code_lines, 1):
            if INCLUDE_RE.match(code):
                continue
            if pat.search(code):
                hits.append({"file": rel, "line": lineno, "reason": reason,
                             "code": code.strip()[:110]})
    return hits


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--show-raw-handles", action="store_true",
                    help="list every frozen bare-handle declaration and exit")
    args = ap.parse_args()
    if not PUBLIC_ROOT.is_dir():
        print(f"error: public ABI root missing: {PUBLIC_ROOT}", file=sys.stderr)
        return 2
    unexported = scan_unexported()
    ui_conv = scan_ui_convergence()
    raw = scan_raw_handles()
    if args.show_raw_handles:
        for x in raw:
            print(f"  {x['file']}:{x['line']}: {x['token']}")
            print(f"      {x['code']}")
        print(f"\n{len(raw)} bare-handle declaration(s); baseline {RAW_HANDLE_BASELINE}.")
        return 0

    v = scan()
    if args.json:
        print(json.dumps({"type_violations": v,
                          "raw_handles": len(raw),
                          "raw_handle_baseline": RAW_HANDLE_BASELINE}, indent=1))
        return 1 if (v or len(raw) > RAW_HANDLE_BASELINE) else 0

    if unexported:
        print("public-abi export check: FAILED")
        print("  %d public prototype(s) lack JCE_API, so the ABI snapshot"
              % len(unexported))
        print("  gate cannot see them — they can change or vanish unnoticed:")
        for u in unexported[:25]:
            print("    %s:%d: %s" % (u["file"], u["line"], u["name"]))
        if len(unexported) > 25:
            print("    ... and %d more" % (len(unexported) - 25))
        print()

    if ui_conv:
        print("ui-ownership check: FAILED (ADR-0002)")
        for u in ui_conv:
            print("    %s:%d: %s" % (u["file"], u["line"], u["reason"]))
            print("        %s" % u["code"])
        print()

    ratchet_broken = len(raw) > RAW_HANDLE_BASELINE
    if ratchet_broken:
        print("public-abi raw-handle ratchet: FAILED")
        print(f"  bare uint16_t handle/index parameters in public renderer headers:"
              f" {len(raw)} (baseline {RAW_HANDLE_BASELINE})")
        print("  Public headers must not GROW new raw bgfx handle indices. Use the")
        print("  struct handles in jce_gfx_types.h (JceTextureHandle / JceShaderHandle")
        print("  / JceUniformHandle) — equally bgfx-free, but distinct at compile time.")
        print("  Run with --show-raw-handles to see the full list.")
        print()

    if not v and not ratchet_broken and not unexported and not ui_conv:
        print("public-abi (type-level) check: OK — no third-party/C++/platform types in <jce/...> headers")
        print(f"  raw-handle ratchet: {len(raw)}/{RAW_HANDLE_BASELINE} (not growing)")
        return 0
    if not v and (unexported or ui_conv or ratchet_broken):
        return 1
    print("public-abi (type-level) check: FAILED — third-party/C++/platform type in public ABI")
    print("(complements check_public_api_purity.py, which only scans #include lines)")
    print()
    for x in v:
        print(f"  {x['file']}:{x['line']}: {x['reason']}")
        print(f"      {x['code']}")
    print()
    print(f"{len(v)} violation(s).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
