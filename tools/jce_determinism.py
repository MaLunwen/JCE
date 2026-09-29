#!/usr/bin/env python3
"""
jce_determinism.py - the ONE capture-determinism recipe, and the ONE way to
prove which backend a run actually used.

Why this file exists: the recipe lived only inside visual_diff.py, so
render_parity.py -- the instrument the skill sanctions for cross-backend
comparison -- ran with none of it.  Its default 3.0% threshold then sat BELOW
the 3.2587% noise floor that unpinned TAA jitter produces, i.e. below its own
noise.  Two independent audits flagged it before this file existed.

Import it, do not copy it:

    from jce_determinism import DETERMINISM, actual_backend, assert_backend
"""

import re
import struct
import zlib
# The settings that make a capture repeat.  Anything a caller passes wins, so a
# comparison can deliberately vary one of them.
DETERMINISM = {
    "JCE_FRAME_DT_FIXED": "0.016666",
    "JCE_STREAM_SYNC": "1",
    # Pin the TAA jitter.  The Halton phase advances per viewport render, and
    # the editor spends a variable number of frames loading, so two runs of one
    # build photograph the scene at different sub-pixel offsets.  That is the
    # whole of one scene's bimodal floor: 3.2587% of pixels over threshold, max
    # delta 500, and a mean local gradient of 45.67 on the differing pixels
    # against 9.41 on the rest -- edges only.
    "JCE_TAA_JITTER_PHASE": "0",
    # DO NOT TAKE THE DESKTOP.  A measuring tool starts a real engine with a
    # real GPU device; it has no business also taking the user's window focus
    # and keyboard for the length of the run.  JCE_WINDOW_HIDDEN keeps the
    # whole windowed path -- device, swapchain, draw(), screenshot readback --
    # and only withholds the window from the desktop, so the picture is the
    # same one and nothing is stolen to get it.
    #
    # It belongs HERE rather than in each tool for the reason this file exists:
    # jce_accept.py set it and envshot.py did not, so the harness that takes
    # most of this repo's pictures grabbed focus on every single capture.
    #
    # A caller that genuinely needs a mapped window passes
    # JCE_WINDOW_HIDDEN=0 and wins, as with everything else here.
    #
    # A hidden window is 34 px TALLER than the same run made visible: a
    # maximised window's client area is the display's usable bounds minus the
    # title bar, and a hidden one is created at those bounds outright.  Measured
    # 2560x1494 vs 2560x1528.  Compare hidden captures with hidden captures.
    "JCE_WINDOW_HIDDEN": "1",
}

# The engine logs the ACTUAL renderer at jce_renderer.c:1771 as
#
#     jce_renderer: renderer: Direct3D 11
#
# MEASURED 2026-08-27, not assumed.  Two traps a naive parser walks into:
#
#   1. The module's LOG_TAG is "jce_renderer", so EVERY line from it begins
#      "jce_renderer: ".  Anchoring on a bare "renderer: " matches
#      "imgui_renderer: initialized (view 250)" first and parses "initialized"
#      as the backend name.
#   2. The line is emitted DURING bgfx init, before JCE_LOG_FILE takes over.
#      Measured: it is present in stdout (138 lines) and absent from the log
#      file (70 lines) of the same run.  A caller must capture STDOUT.
MARKER = "jce_renderer: renderer: "

# JCE_BACKEND value -> normalised bgfx renderer name.  Normalised = lowercase
# with spaces removed, so "Direct3D 11" and "Direct3D11" both match, and
# "OpenGL ES" and "OpenGLES" both match -- while "OpenGL" still does NOT match
# "OpenGLES" (which a substring test would wrongly accept).
#
# The key set is the engine's own accepted set (jce_renderer.c:1584-1600):
# auto, d3d11, d3d12, vulkan, opengl, gl, gles, opengles, metal, noop.
BGFX_NAME = {
    "d3d11":    "direct3d11",
    "d3d12":    "direct3d12",
    "vulkan":   "vulkan",
    "opengl":   "opengl",
    "gl":       "opengl",
    "gles":     "opengles",
    "opengles": "opengles",
    "metal":    "metal",
    "noop":     "noop",
}

# "auto" means "whatever bgfx picks".  There is nothing to assert, but the
# caller still gets told what it got -- silence would be indistinguishable from
# a failed probe.
NO_ASSERTION = ("auto", "")


def _norm(s):
    return "".join(s.split()).lower()


# Backends whose reported name carries the RUNTIME version, so an exact match
# can never succeed.  bgfx reports "Direct3D 11" -- where the 11 is part of the
# name and is in the table above -- but "OpenGL 3.1", where the 3.1 is what the
# driver happened to give.  Exact matching therefore rejected every OpenGL
# capture with "bgfx fell back", which is the opposite of what had happened,
# and made the one backend this tree has had the most backend-specific bugs on
# the one backend no capture could be verified for.
#
# Longest first: "opengles3.0" starts with "opengl", so a naive prefix test
# would call an ES capture a GL one -- which is the fallback this guard exists
# to catch, waved through by the fix for a different problem.
_FAMILIES = ("direct3d11", "direct3d12", "opengles", "opengl", "vulkan",
             "metal", "noop")


def backend_family(normalised):
    """The canonical backend name inside a reported string, or the input."""
    for fam in _FAMILIES:
        if normalised.startswith(fam):
            return fam
    return normalised


def actual_backend(text):
    """Return the renderer the engine reported, or None if it never said.

    None is itself a finding: the run did not reach renderer init, so whatever
    image it produced is not a capture of any backend.
    """
    for line in text.splitlines():
        idx = line.find(MARKER)
        if idx < 0:
            continue
        name = line[idx + len(MARKER):]
        # Trim the log's trailing "at file:line" decoration and any ANSI reset,
        # AND the engine's own annotation after the renderer name.
        #
        # jce_renderer.c logs one of two forms:
        #     renderer: <name> API 1.2.3 (stable floor 1.0.0)
        #     renderer: <name> (graphics tier stable)
        # and this function used to take everything after the marker, so the
        # name it compared was "Direct3D 11 (graphics tier stable)".  That never
        # equals "direct3d11", so assert_backend could not pass for ANY backend
        # in either form -- the one guard against a silent bgfx fallback was
        # unpassable, and every envshot capture printed "backend NOT verified"
        # instead of verifying anything.  (Found by using it: --log on a d3d11
        # capture raised "backend mismatch ... but the engine reported
        # 'Direct3D 11 (graphics tier stable)'".)
        #
        # Cutting at " (" and " API " and not at a general token boundary keeps
        # what the exactness is FOR: no bgfx renderer name contains either, and
        # "OpenGL" must still fail to match "OpenGL ES", which a substring test
        # would wrongly accept.
        for cut in (" at ", "\x1b", " API ", " ("):
            pos = name.find(cut)
            if pos >= 0:
                name = name[:pos]
        name = name.strip()
        if name:
            return name
    return None


def assert_backend(text, requested):
    """Raise unless the engine reported the backend that was asked for.

    An UNKNOWN request is rejected, not waved through: the engine's accepted
    set is finite and known, so a name outside it means the caller and this
    table have drifted -- and a drifted table cannot catch a fallback.

    Returns the reported name.
    """
    got = actual_backend(text)
    if got is None:
        raise RuntimeError(
            "no %r line in the captured output for requested backend %r -- the "
            "run never reached renderer init, so any image it produced is not a "
            "capture of that backend.  (The line is emitted before "
            "JCE_LOG_FILE takes over: capture STDOUT.)" % (MARKER, requested))
    key = requested.lower()
    if key in NO_ASSERTION:
        return got
    want = BGFX_NAME.get(key)
    if want is None:
        raise RuntimeError(
            "backend %r is not in this table (engine accepts %s).  Refusing to "
            "pass an unverifiable request: an unknown key cannot detect a bgfx "
            "fallback." % (requested, ", ".join(sorted(BGFX_NAME)) + ", auto"))
    if backend_family(_norm(got)) != want:
        raise RuntimeError(
            "backend mismatch: asked for %r (expects %r) but the engine reported "
            "%r -- bgfx fell back, and comparing this capture would compare a "
            "backend against itself" % (requested, want, got))
    return got


# ── minimal PNG reader ───────────────────────────────────────────────
#
# assert_has_content() takes the tuple this returns, so the two belong
# in the same module.  They were split: the guard moved here and its only
# input producer stayed in render_parity.py, which left the guard
# unusable by any third caller -- the same "correct shared implementation
# with zero reachable callers" shape this repo keeps rediscovering.
def read_png(path):
    """Return (width, height, channels, bytearray pixels). 8-bit only."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)

    pos = 8
    width = height = None
    bitdepth = ctype = None
    idat = bytearray()
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        tag = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            width, height, bitdepth, ctype, _comp, _filt, interlace = \
                struct.unpack(">IIBBBBB", chunk)
            if bitdepth != 8 or ctype not in (2, 6) or interlace != 0:
                raise ValueError(
                    "%s: unsupported PNG (bitdepth=%d colortype=%d "
                    "interlace=%d); expected 8-bit RGB/RGBA progressive"
                    % (path, bitdepth, ctype, interlace))
        elif tag == b"IDAT":
            idat += chunk
        elif tag == b"IEND":
            break

    channels = 3 if ctype == 2 else 4
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(width * height * channels)

    # Undo per-scanline filters (PNG spec types 0-4).
    src = 0
    for y in range(height):
        ftype = raw[src]
        src += 1
        line_off = y * stride
        prev_off = line_off - stride
        for x in range(stride):
            v = raw[src + x]
            a = out[line_off + x - channels] if x >= channels else 0
            b = out[prev_off + x] if y > 0 else 0
            c = (out[prev_off + x - channels]
                 if (y > 0 and x >= channels) else 0)
            if ftype == 0:
                r = v
            elif ftype == 1:
                r = (v + a) & 0xFF
            elif ftype == 2:
                r = (v + b) & 0xFF
            elif ftype == 3:
                r = (v + ((a + b) >> 1)) & 0xFF
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                r = (v + pred) & 0xFF
            else:
                raise ValueError("%s: bad filter %d" % (path, ftype))
            out[line_off + x] = r
        src += stride
    return width, height, channels, out


def assert_has_content(img, path, y0=300, y1=1100, x0=100, x1=1500, step=7):
    """Reject a capture that has nothing in it.

    A blank viewport compares EQUAL to another blank viewport, so "0.00000%
    difference" is not evidence of correctness -- it is also what you get when
    neither run drew anything.  visual_diff.py reported exactly that three
    times: the capture fired on spin step 250 while the scene needed ~320
    frames to come up, so both sides photographed an empty viewport at a
    uniform RGB(30,30,30) and the gate passed vacuously.

    A near-zero spread over the sampled 3D area means the frame is uniform --
    empty, or black.  `img` is the (w, h, channels, pixels) tuple the callers'
    read_png() returns.
    """
    w, h, c, px = img
    lo, hi, tot, n = 255, 0, 0, 0
    for y in range(y0, min(h, y1), step):
        row = y * w * c
        for x in range(x0, min(w, x1), step):
            i = row + x * c
            v = (px[i] + px[i + 1] + px[i + 2]) // 3
            lo = min(lo, v)
            hi = max(hi, v)
            tot += v
            n += 1
    if n == 0:
        raise RuntimeError("%s: sampled no pixels -- the crop window is outside "
                           "the image (%dx%d)" % (path, w, h))
    spread = hi - lo
    if spread < 6:
        raise RuntimeError(
            "%s: the sampled 3D area is uniform (spread=%d, mean=%d) -- this is "
            "a BLANK capture.  Two blank captures compare perfectly equal, so "
            "comparing it would pass vacuously." % (path, spread, tot // n))


SHOT_RE = re.compile(r"SHOT (\S+) visible=(\d+) total=(\d+) culled=(\d+)")


def assert_subject_in_frame(text, path):
    """Reject a capture that contains none of its subject.

    assert_has_content above rejects a BLANK frame.  This rejects the other
    one, which is the harder of the two: a frame with sky, ground grid and
    gizmos in it, and none of the thing being measured.  It is not blank, its
    spread is large, every guard passes, and every number read off it
    describes pixels the subject never touched.

    That is not hypothetical.  A finding -- "the editor's Scene view binds no
    per-draw PBR material state, so rendering layers do not apply there" --
    was raised, instrumented at the first line of the bind function, measured
    at a peak of 2/255, and written into the parity ledger as a `behind` row.
    Every step of it was done carefully.  The capture had been taken with the
    orbit rig's default target, and the two subject spheres were outside the
    frustum: the probe counted zero because there was nothing to draw.  Aiming
    the camera at them and re-running the identical probe gave 120 entries in
    the same function, and the layer mask swapped which sphere was lit.

    The renderer already knew.  `visible` is what survived culling, the editor
    now logs it beside the file it just wrote, and this reads it back.  Only
    callers that pass --log can be checked; without the engine's own line this
    says so rather than implying the frame was verified.
    """
    rows = SHOT_RE.findall(text or "")
    if not rows:
        return None                      # no --log, or an older editor
    name = str(path).replace("\\", "/").rsplit("/", 1)[-1]
    mine = [r for r in rows if r[0].replace("\\", "/").endswith(name)] or rows
    shot, visible, total, culled = mine[-1]
    if int(visible) == 0:
        raise RuntimeError(
            "%s: the renderer drew NOTHING -- visible=0 of total=%s collected "
            "(culled=%s).  The capture is not blank (sky, grid and gizmos are "
            "in it), so no blank-image guard catches this; it simply does not "
            "contain the subject.  Aim the camera: --target X,Y,Z with "
            "--dist/--pitch/--yaw." % (shot, total, culled))
    return int(visible)


# ── the engine's steady-state perf line ──────────────────────────────────
#
# One pattern, because there is one line.  jce_accept.py wrote a second,
# looser one ("frame ... ms ... fps") that matched nothing the engine emits,
# so its perf stage reported "the run never reached steady state" for runs
# that had printed a perfectly good sample.
PERF_RE = re.compile(
    r"engine: perf: ([\d.]+) ms avg \((\d+) FPS\) \| cpu ([\d.]+) / gpu ([\d.]+)"
    r" ms \| (\d+) draws \| gpu-mem (\d+) MB \(tex (\d+) \+ rt (\d+)\)"
    r" \| rss (\d+) MB \| commit (\d+)")


def parse_perf(text):
    """The LAST steady-state sample in `text`, or None.

    Last, not first: the early samples are the ones taken while streaming and
    shader compilation are still running, and they are not the frame time
    anyone means.
    """
    rows = PERF_RE.findall(text or "")
    if not rows:
        return None
    r = rows[-1]
    return {"frame_ms": float(r[0]), "fps": int(r[1]), "cpu_ms": float(r[2]),
            "gpu_ms": float(r[3]), "draws": int(r[4]), "gpu_mem_mb": int(r[5]),
            "rss_mb": int(r[8]), "commit_mb": int(r[9]), "samples": len(rows)}


# ── self-test ────────────────────────────────────────────────────────
#
# RUN BY tools/lint/check_determinism_contract.py, which is in run_all.py.
# A self-test with no caller is the shape this repository keeps rediscovering
# (seven jce_*_self_test functions, zero call sites, three of them red the
# first time anyone ran them), so the caller comes with the test.
#
# The cases that matter are the NEGATIVE ones: assert_backend exists to catch a
# silent bgfx fallback, so a version of it that accepts everything is worse
# than none.  "OpenGL" must fail against "OpenGL ES" in both directions -- that
# is the property the annotation-trim could plausibly have destroyed.

_BACKEND_CASES = [
    ("jce_renderer: renderer: Direct3D 11 (graphics tier stable) "
     "at jce_renderer.c:1055", "d3d11", True,
     "the unverified log form, with the engine's tier annotation"),
    ("jce_renderer: renderer: Direct3D 11 API 11.0.0 (stable floor 11.0.0)",
     "d3d11", True, "the verified log form, with the API/floor annotation"),
    ("jce_renderer: renderer: Vulkan (graphics tier stable)", "vulkan", True,
     "another backend, same annotation"),
    ("jce_renderer: renderer: OpenGL ES (graphics tier stable)", "opengl", False,
     "OpenGL must NOT match OpenGL ES"),
    ("jce_renderer: renderer: OpenGL (graphics tier stable)", "gles", False,
     "...nor the other way round"),
    ("jce_renderer: renderer: OpenGL ES (graphics tier stable)", "gles", True,
     "OpenGL ES asked for and got"),
    ("jce_renderer: renderer: Direct3D 12 (graphics tier stable)", "d3d11", False,
     "a real fallback must still be caught"),
    ("no renderer line at all", "d3d11", False,
     "a run that never reached renderer init is not a capture of any backend"),
]


def self_test():
    """Return a list of failure strings; empty means OK."""
    fails = []
    for line, requested, want_ok, why in _BACKEND_CASES:
        try:
            assert_backend(line, requested)
            got_ok = True
        except RuntimeError:
            got_ok = False
        if got_ok != want_ok:
            fails.append(
                "assert_backend(%r) %s -- expected it to %s: %s"
                % (requested, "accepted" if got_ok else "rejected",
                   "accept" if want_ok else "reject", why))
    # parse_perf on text that contains no sample must say so, not invent one.
    if parse_perf("nothing here") is not None:
        fails.append("parse_perf invented a sample from text with none")
    return fails


if __name__ == "__main__":
    import sys as _sys
    _f = self_test()
    for _m in _f:
        print("  " + _m)
    print("jce_determinism self-test: %s (%d case(s))"
          % ("FAILED" if _f else "OK", len(_BACKEND_CASES) + 1))
    _sys.exit(1 if _f else 0)
