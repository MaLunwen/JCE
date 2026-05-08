#!/usr/bin/env python3
"""
check_layer_dependencies.py — CI guard enforcing JCE engine layer
hierarchy.  Catches reverse-direction includes between layers.

Layer rules:
  * `engine/src/renderer/`  must NOT include `middleware/` (or
    `<jce/middleware/...>`).  Renderer is GPU primitives + frame
    orchestration only; high-level subsystems live in middleware.
  * `engine/src/middleware/` must NOT include `application/` or
    reach into `caged_kingdom/`.  Middleware is application-agnostic.
  * `engine/src/os/`        must NOT include `renderer/`,
    `middleware/`, or `application/`.  OS layer is the lowest tier.
  * Vendored `third_party/**` is skipped entirely.

ALLOW_FILES is for sanctioned exceptions documented in the source
(e.g. layered headers that bridge two tiers behind opaque types).

Usage:
  python scripts/lint/check_layer_dependencies.py
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
    # JNI bridge is a platform shim that intentionally bridges to the
    # application API to dispatch lifecycle events from Android.
    ("engine/src/os/platform/jce_jni_bridge.c",
     "application/jce_app_interface.h"),
    ("engine/src/os/platform/jce_jni_bridge.c",
     "application/jce_engine.h"),
}


# Each rule: (source-tree-prefix, banned-include-substring, reason)
RULES: list[tuple[str, re.Pattern[str], str]] = [
    ("engine/src/renderer/",
     re.compile(r"#include\s*[<\"](?:jce/)?middleware/([^\">]+)[>\"]"),
     "renderer must not include middleware (P2 layer rule)"),
    ("engine/src/middleware/",
     re.compile(r"#include\s*[<\"](?:jce/)?application/([^\">]+)[>\"]"),
     "middleware must not include application"),
    ("engine/src/os/",
     re.compile(r"#include\s*[<\"](?:jce/)?(renderer|middleware|application)/([^\">]+)[>\"]"),
     "os layer must not include higher tiers"),
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
