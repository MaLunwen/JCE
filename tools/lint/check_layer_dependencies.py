#!/usr/bin/env python3
"""
check_layer_dependencies.py — CI guard enforcing JCE engine layer
hierarchy.  Catches reverse-direction includes between layers.

Layer rules:
  * `engine/src/renderer/`  must NOT include `middleware/` (or
    `<jce/middleware/...>`).  Renderer is GPU primitives + frame
    orchestration only; high-level subsystems live in middleware.
  * `engine/src/middleware/` must NOT include `application/` or
    reach into `examples/caged_kingdom/`.  Middleware is application-agnostic.
  * `engine/src/os/`        must NOT include `renderer/`,
    `middleware/`, or `application/`.  OS layer is the lowest tier.
  * Vendored `third_party/**` is skipped entirely.

ALLOW_FILES is for sanctioned exceptions documented in the source
(e.g. layered headers that bridge two tiers behind opaque types).

Usage:
  python tools/lint/check_layer_dependencies.py
  # exits 0 if clean, 1 with file:line:reason on violation.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
ENGINE_SRC = REPO_ROOT / "engine" / "src"

# Sanctioned cross-layer includes (file:include-suffix to ignore).
# Listed as set of (relpath_posix, banned_include_suffix) tuples.
ALLOW = {
    # gltf loader still constructs animation clips inline.  Tracked
    # for a future split (jce_gltf_anim_loader.c).
    ("engine/src/renderer/jce_gltf_loader.c",
     "middleware/animation/jce_animation.h"),
    ("engine/src/renderer/jce_model.c",
     "middleware/animation/jce_animation.h"),
    ("engine/src/renderer/jce_model.h",
     "middleware/animation/jce_animation.h"),
    # Mesh loaders consume skeleton (now an animation type); future
    # split will isolate mesh-only loader from skeleton/animation.
    ("engine/src/renderer/jce_gltf_loader.c",
     "middleware/animation/jce_skeleton.h"),
    ("engine/src/renderer/jce_model.c",
     "middleware/animation/jce_skeleton.h"),
    ("engine/src/renderer/jce_model.h",
     "middleware/animation/jce_skeleton.h"),
    # Mesh loaders also construct morph targets / read compressed-anim types
    # inline — same sanctioned inline-load exception as the clip/skeleton
    # includes above (tracked for the future jce_gltf_anim_loader.c split).
    ("engine/src/renderer/jce_gltf_loader.c",
     "middleware/animation/jce_morph.h"),
    ("engine/src/renderer/jce_gltf_loader.c",
     "middleware/animation/jce_anim_compress.h"),
    ("engine/src/renderer/jce_model.h",
     "middleware/animation/jce_morph.h"),
    ("engine/src/renderer/jce_model_internal.h",
     "middleware/animation/jce_morph.h"),
    # JNI bridge is a platform shim that intentionally bridges to the
    # application API to dispatch lifecycle events from Android.
    ("engine/src/os/platform/jce_jni_bridge.c",
     "application/jce_app_interface.h"),
    ("engine/src/os/platform/jce_jni_bridge.c",
     "application/jce_engine.h"),

    # ── engine/src/resource/ (L3) -> middleware (L4), 13 edges ──────────
    # PRE-EXISTING when the resource rule was added on 2026-08-31, and
    # recorded here so the rule could be turned on at all.  Each is a real
    # upward include under §3, not a false positive: the cooker/loader/
    # manager/async-pool reach into the audio subsystem to size and decode
    # clips, the model importer builds skeletons and clips inline (the same
    # shape jce_gltf_loader.c is allow-listed for above), and the three
    # scene/world files call the scene serializer that
    # engine/src/resource/AGENTS.md rule 50 explicitly tells them to call.
    # NOT a blanket exemption for the directory: anything new fails.
    ("engine/src/resource/jce_asset_cooker.c",
     "middleware/audio/jce_audio.h"),
    ("engine/src/resource/jce_asset_loaders.c",
     "middleware/audio/jce_audio.h"),
    ("engine/src/resource/jce_asset_manager.c",
     "middleware/audio/jce_audio.h"),
    ("engine/src/resource/jce_async_pool.c",
     "middleware/audio/jce_audio.h"),
    ("engine/src/resource/jce_model_importer.cpp",
     "middleware/animation/jce_skeleton.h"),
    ("engine/src/resource/jce_model_importer.cpp",
     "middleware/animation/jce_animation.h"),
    ("engine/src/resource/jce_model_importer.cpp",
     "middleware/animation/jce_anim_compress.h"),
    ("engine/src/resource/jce_scene_serial.c",
     "middleware/scene/jce_scene.h"),
    ("engine/src/resource/jce_scene_serial.c",
     "middleware/scene/jce_scene_components_json.h"),
    ("engine/src/resource/jce_world_partition.c",
     "middleware/scene/jce_scene.h"),
    ("engine/src/resource/jce_world_partition.c",
     "middleware/scene/jce_scene_components_json.h"),
    ("engine/src/resource/jce_world_streamer.c",
     "middleware/scene/jce_scene.h"),
    ("engine/src/resource/jce_world_streamer.c",
     "middleware/scene/jce_scene_components_json.h"),
}


# Each rule: (source-tree-prefix, banned-include-substring, reason)
RULES: list[tuple[str, re.Pattern[str], str]] = [
    ("engine/src/renderer/",
     re.compile(r"#include\s*[<\"](?:jce/)?(?:middleware|application)/([^\">]+)[>\"]"),
     "renderer must not include middleware/application (layer rule)"),
    ("engine/src/middleware/",
     re.compile(r"#include\s*[<\"](?:jce/)?application/([^\">]+)[>\"]"),
     "middleware must not include application"),
    ("engine/src/os/",
     re.compile(r"#include\s*[<\"](?:jce/)?(renderer|middleware|application|resource)/([^\">]+)[>\"]"),
     "os layer must not include higher tiers"),
    # ADDED 2026-08-31.  engine/src has SEVEN directories and this gate scanned
    # THREE of them; §3 calls the rule 铁律 and "评审强制检查", and a reader of
    # this file's docstring -- which lists only the three -- would reasonably
    # conclude §3 was covered.  It was not: engine/src/resource/ (L3) carried
    # 13 live upward includes into L4 middleware, in 8 files, invisible to
    # every run.  They are ALLOW-listed below rather than fixed here, because
    # unpicking them is a design change and a gate that cannot be adopted is a
    # gate that gets deleted -- but nothing NEW can be added now.
    ("engine/src/resource/",
     re.compile(r"#include\s*[<\"](?:jce/)?(middleware|runtime|application)/([^\">]+)[>\"]"),
     "resource (L3) must not include middleware/runtime/application (higher tiers)"),
    ("engine/src/runtime/",
     re.compile(r"#include\s*[<\"](?:jce/)?application/([^\">]+)[>\"]"),
     "runtime (L5) must not include application (L6)"),
]


def rel(p: Path) -> str:
    return p.relative_to(REPO_ROOT).as_posix()


def is_source_file(name: str) -> bool:
    return name.endswith((".c", ".cpp", ".h", ".hpp"))


def is_vendored(name: str) -> bool:
    return name.startswith(("stb_", "miniaudio", "dr_"))


def scan() -> list[tuple[str, int, str, str]]:
    violations: list[tuple[str, int, str, str]] = []
    for r, dirs, files in os.walk(ENGINE_SRC):
        dirs[:] = [d for d in dirs if d != "third_party"]
        for fn in files:
            if not is_source_file(fn) or is_vendored(fn):
                continue
            p = Path(r) / fn
            rp = rel(p)
            try:
                lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
            except OSError as e:
                print(f"warn: cannot read {rp}: {e}", file=sys.stderr)
                continue
            for prefix, pat, reason in RULES:
                if not rp.startswith(prefix):
                    continue
                for lineno, line in enumerate(lines, start=1):
                    code = line.split("//", 1)[0]
                    m = pat.search(code)
                    if not m:
                        continue
                    suffix = m.group(0)
                    # Build an allow-key matching the include path tail.
                    inc_match = re.search(r"[<\"]([^\">]+)[>\"]", suffix)
                    if inc_match:
                        inc_path = inc_match.group(1)
                        # Normalize jce/ prefix for ALLOW lookup.
                        norm = inc_path[len("jce/"):] if inc_path.startswith("jce/") else inc_path
                        if (rp, norm) in ALLOW:
                            continue
                    violations.append((rp, lineno, reason, line.rstrip()))
    return violations


def main() -> int:
    v = scan()
    if not v:
        print("layer-dependency check: OK (engine layers respect tier hierarchy)")
        return 0
    print("layer-dependency check: FAILED — reverse / cross-tier includes found")
    print()
    for rp, lineno, reason, line in v:
        print(f"  {rp}:{lineno}: {reason}")
        print(f"      {line.strip()}")
    print()
    print(f"{len(v)} violation(s).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
