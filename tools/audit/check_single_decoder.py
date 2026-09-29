#!/usr/bin/env python3
"""
check_single_decoder.py — one image-decode service, enforced.

Background (dedup audit DUP-003).  JCE shipped two general-purpose image
loaders: SDL3_image behind the `jce_image` dispatcher, and a second stb_image
path in `jce_image_decode.c` used by the editor previews, the thumbnail cache,
the grass density-mask loader and the PAK decode helper.  Two loaders means two
format sets, two JPEG IDCTs and — the part that actually bit — two places that
have to remember the 16-bit-greyscale-PNG bypass.  SDL3_image's libpng path
heap-overruns on IHDR bit-depth 16 / colour-type 0 (STATUS_HEAP_CORRUPTION).
The synchronous loader and the cooker sniffed for it; `jce_async_pool.c` did
not, so a game that streamed such a height map in asynchronously corrupted the
heap on a worker thread.

Deleting the second decoder fixes it once.  This gate is what keeps it fixed:
the decode entry points of either vendor library may be called only from the
service itself.  `contracts/dependency-ownership.yml` already names
`jce_image` as the facade, but its whitelists are directory-scoped and cannot
say "this one file"; this check can.

ENCODE is not decode: `IMG_SavePNG` (screenshots, impostor atlas bakes) is not
a codec choice at a call site and is deliberately not flagged.

Usage:
  python tools/audit/check_single_decoder.py          # exit 1 on violation
  python tools/audit/check_single_decoder.py --list   # show the owner set
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# The ONE service, plus the two places whose exception is documented in-tree.
OWNERS = {
    # The dispatcher itself: LDR -> SDL_image, HDR + 16-bit grey -> stb.
    "engine/src/renderer/jce_image.c",
    # STB_IMAGE_IMPLEMENTATION translation unit (the library build, not a call site).
    "engine/src/renderer/jce_stb_image_impl.c",
    # Window icon: the DESTINATION is SDL_SetWindowIcon, so an SDL_Surface is
    # the required output type rather than an incidental one.  Narrow, and it
    # never sees game content.  See jce_window.c.
    "engine/src/os/platform/jce_window.c",
}

# Vendored third-party trees are not first-party call sites.
SKIP_DIR_PARTS = (
    "/internal/",
    "/third_party/",
    "/external/",
    "/vendor/",
)

# User projects are OUT of this gate's scan surface (owner decision, 2026-08-27).
# The general engine and editor are the product; a user project is a downstream
# dogfooding consumer.  Folding the consumer in means a defect inside
# examples/caged_kingdom/ can turn the ENGINE's architecture gate red -- and the question
# this gate answers is whether the engine and the editor held their boundaries.
# Whether consumers deserve a gate of their own is a separate question.
SCAN_ROOTS = ("engine/", "editor/", "tools/", "sdk/")
SCAN_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx")

# Decode entry points only.  Word-bounded so `IMG_SavePNG` and comments
# mentioning `stbi_image_free` do not trip it.
DECODE_CALLS = re.compile(
    r"\b("
    r"IMG_Load|IMG_Load_IO|IMG_LoadTyped_IO|IMG_Load_RW|IMG_LoadTyped_RW|"
    r"IMG_LoadTexture|IMG_LoadTexture_IO|IMG_LoadTexture_RW|IMG_LoadAnimation|"
    r"IMG_LoadAnimation_IO|IMG_LoadAnimationTyped_IO|"
    r"stbi_load|stbi_loadf|stbi_load_from_memory|stbi_loadf_from_memory|"
    r"stbi_load_from_callbacks|stbi_loadf_from_callbacks|stbi_load_16|"
    r"stbi_load_16_from_memory"
    r")\s*\("
)


def tracked_sources() -> list[str]:
    out = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=REPO_ROOT, capture_output=True, text=True,
        encoding="utf-8", errors="replace", check=False,
    ).stdout
    files = []
    for rel in out.split("\0"):
        if not rel or not rel.endswith(SCAN_SUFFIXES):
            continue
        if not rel.startswith(SCAN_ROOTS):
            continue
        if any(part in "/" + rel for part in SKIP_DIR_PARTS):
            continue
        files.append(rel)
    return files


def strip_block_comments(text: str) -> str:
    """Blank out /* ... */ and // ... so prose about the ban is not a violation.

    Newlines are preserved so reported line numbers stay accurate."""
    def blank(m: re.Match) -> str:
        return re.sub(r"[^\n]", " ", m.group(0))
    text = re.sub(r"/\*.*?\*/", blank, text, flags=re.S)
    text = re.sub(r"//[^\n]*", blank, text)
    return text


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true",
                    help="print the owner set and exit")
    args = ap.parse_args()

    if args.list:
        print("image-decode owners (may call SDL_image / stb_image decode):")
        for o in sorted(OWNERS):
            print(f"  {o}")
        return 0

    violations: list[tuple[str, int, str]] = []
    scanned = 0
    for rel in tracked_sources():
        path = REPO_ROOT / rel
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        scanned += 1
        if "IMG_Load" not in text and "stbi_load" not in text:
            continue
        code = strip_block_comments(text)
        for n, line in enumerate(code.splitlines(), 1):
            m = DECODE_CALLS.search(line)
            if not m:
                continue
            if rel in OWNERS:
                continue
            violations.append((rel, n, m.group(1)))

    print(f"single-decoder: scanned {scanned} first-party sources; "
          f"{len(OWNERS)} owner(s)")
    if not violations:
        print("OK — every image decode goes through the jce_image service.")
        return 0

    print(f"\nFAIL — {len(violations)} decode call(s) outside the service:\n")
    for rel, n, sym in violations:
        print(f"  {rel}:{n}: {sym}(")
    print(
        "\nDecode through <jce/renderer/jce_image.h> "
        "(jce_image_load_rgba8_from_memory) or, for the {pixels,w,h} struct, "
        "<jce/resource/jce_image_decode.h>.  A second decoder re-opens the "
        "16-bit-greyscale-PNG heap overrun: only the service sniffs for it.\n"
        "If a new owner is genuinely warranted, add it to OWNERS here WITH the "
        "reason, so the exception is reviewable."
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
