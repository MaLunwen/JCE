#!/usr/bin/env python3
"""
check_determinism_contract.py - the capture-determinism recipe and the
backend assertion must stay true, single-sourced, and in step with the engine.

WHY THIS GATE EXISTS
--------------------
tools/jce_determinism.py was added after two independent audits found that
render_parity.py -- the sanctioned cross-backend instrument -- ran with no
determinism pins and never checked which backend bgfx actually selected.  The
fix was verified once, by hand.  A one-shot verification is not a gate: the
third audit's first finding was precisely that nothing would catch it rotting.

WHAT IT CHECKS
--------------
1. The recipe is SINGLE-SOURCED: no tool re-declares DETERMINISM locally, and
   every tool that captures imports it.
2. BGFX_NAME covers EXACTLY the backend set the ENGINE accepts, parsed out of
   engine/src/renderer/jce_renderer.c.  A table that drifts from the engine
   cannot detect a fallback -- and an unlisted backend used to be waved
   through.
3. actual_backend() anchors on the module tag.  The engine's LOG_TAG is
   "jce_renderer", so a bare "renderer: " anchor matches
   "imgui_renderer: initialized (view 250)" first and parses "initialized" as
   the backend name.  Measured 2026-08-27.
4. assert_backend() accepts a real match, and REFUSES: a fallback, a missing
   line, and an unknown request.
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS = REPO_ROOT / "tools"
sys.path.insert(0, str(TOOLS))

RENDERER_C = REPO_ROOT / "engine" / "src" / "renderer" / "jce_renderer.c"
# Every tool that LAUNCHES the engine to produce a number or an image.  All of
# them must pin the same environment, or two "identical" runs are not identical.
MEASURING_TOOLS = ("visual_diff.py", "render_parity.py", "perf_bench.py",
                   "envshot.py", "frame_budget.py", "probe_scene_watch.py",
                   "media_preview_probe.py", "build/jce_accept.py")
# The subset that PHOTOGRAPHS the engine.  Only these need the blank-frame
# guard: a perf number cannot be blank, a screenshot can.
CAPTURING_TOOLS = ("visual_diff.py", "render_parity.py", "envshot.py",
                   "media_preview_probe.py")

# A hardcoded list drifts, and a drifting list is a gate that stops covering
# what it names.  envshot.py launched the engine and set JCE_BACKEND for months
# while sitting outside MEASURING_TOOLS, so it kept its own unpinned
# environment and never checked which backend actually rendered -- invisible,
# because the gate only ever looked at the three names written above.
#
# So DERIVE the population instead of trusting the constant: anything under
# tools/ that starts a process AND speaks to the engine through its environment
# is a measuring tool, and must be listed.
LAUNCH_MARKERS = ("subprocess.run", "subprocess.Popen")
ENGINE_MARKERS = ("JCE_BACKEND", "JCE_MAX_FRAMES", "JCE_SHOT_PATH",
                  "JCE_CAPTURE_PATH", "JCE_PERF_LOG")

# rglob, not glob.  The derivation above was written to stop the list drifting,
# and then the SCAN could not reach half of what it was derived from: glob("*.py")
# sees only the top level, so every subdirectory of tools/ -- private/tools/ai/,
# tools/scriptgen/, private/tools/automation/ -- was outside it.  That is this gate's own
# documented failure wearing its other face: not "a name missing from the list"
# but "a place the scan cannot see", and a tool that launches the engine from a
# subdirectory would have been as invisible as envshot.py was.
#
# Names are kept as paths relative to tools/ so that two files with the same
# basename in different directories cannot be mistaken for each other.
SCAN_SKIP_DIRS = ("__pycache__", "tests", "lint", "audit")


def _strip_comments_and_strings(src):
    """Marker text inside a comment or a docstring is not a caller.

    Measured: private/tools/ai/jce_design.py mentions JCE_BACKEND in a comment
    explaining why it delegates every capture to envshot.py.  Matched raw, it
    reads as a tool that drives the engine itself, and the gate would demand it
    be listed as a measuring tool it is not.  This repository has already
    shipped a gate that counted a comment as a call site and produced 6 where
    the answer was 5.
    """
    out = re.sub(r'"""(?:.|\n)*?"""', "", src)
    out = re.sub(r"'''(?:.|\n)*?'''", "", out)
    out = re.sub(r"#[^\n]*", "", out)
    return out


def _unit_of(rel):
    """Which tool a file belongs to.

    A single script is its own unit; a file inside a Python package belongs to
    the package.  Grouping matters because the two markers can sit in DIFFERENT
    FILES of one package and the behaviour still spans them: in
    private/tools/automation/jce_automation, observe.py sets JCE_MAX_FRAMES and
    JCE_CAPTURE_PATH while the subprocess.run that consumes them lives in
    build.py.  Per-file, neither half trips the scan, and the package would
    launch the engine entirely outside this gate -- by nothing more deliberate
    than having been split into modules.
    """
    parts = rel.parts
    for i in range(len(parts) - 1, 0, -1):
        d = TOOLS.joinpath(*parts[:i])
        if (d / "__init__.py").is_file():
            return Path(*parts[:i]).as_posix()
    return rel.as_posix()


def source_of(name):
    """Every line of a unit, whether it is one file or a package."""
    p = TOOLS / name
    if p.is_dir():
        return "\n".join(f.read_text(encoding="utf-8", errors="replace")
                         for f in sorted(p.rglob("*.py")))
    if p.is_file():
        return p.read_text(encoding="utf-8", errors="replace")
    return None


def discovered_measuring_tools():
    """Tools that launch a process and drive the engine through JCE_* env."""
    units = {}
    for p in sorted(TOOLS.rglob("*.py")):
        if p.name == "jce_determinism.py" or p == TOOLS / "build/jce.py":      # the recipe itself, not a tool
            continue
        rel = p.relative_to(TOOLS)
        if any(part in SCAN_SKIP_DIRS for part in rel.parts[:-1]):
            continue
        src = _strip_comments_and_strings(
            p.read_text(encoding="utf-8", errors="replace"))
        u = units.setdefault(_unit_of(rel), {"launch": False, "engine": False})
        u["launch"] |= any(m in src for m in LAUNCH_MARKERS)
        u["engine"] |= any(m in src for m in ENGINE_MARKERS)
    return sorted(n for n, u in units.items() if u["launch"] and u["engine"])

failures = []


def fail(msg):
    failures.append(msg)


def engine_backend_names():
    """The JCE_BACKEND strings the engine compares against, from its source."""
    if not RENDERER_C.is_file():
        return None
    text = RENDERER_C.read_text(encoding="utf-8", errors="replace")
    names = set()
    for m in re.finditer(r'strcmp\(\s*low\s*,\s*"([a-z0-9]+)"\s*\)\s*==\s*0', text):
        names.add(m.group(1))
    return names or None


def main():
    try:
        import jce_determinism as jd
    except Exception as exc:                      # noqa: BLE001
        print("check_determinism_contract: FAILED")
        print("    tools/jce_determinism.py does not import: %s" % exc)
        return 1

    # RUN ITS SELF-TEST.  The recipe module carries the backend assertion that
    # every capture's honesty rests on, and that assertion was UNPASSABLE for
    # months: the engine's log line grew a "(graphics tier ...)" annotation and
    # the exact-match comparison could no longer succeed for any backend, so
    # every capture printed "backend NOT verified" and nobody noticed.  A table
    # of cases lives beside it now; this is what runs it.
    try:
        det_fails = jd.self_test()
    except AttributeError:
        print("check_determinism_contract: FAILED")
        print("    tools/jce_determinism.py has no self_test() -- the backend "
              "assertion is the one guard against a silent bgfx fallback and "
              "nothing would exercise it")
        return 1
    if det_fails:
        print("check_determinism_contract: FAILED")
        print("    tools/jce_determinism.py self-test:")
        for m in det_fails:
            print("      " + m)
        return 1

    # 1. recipe present and single-sourced
    for key in ("JCE_FRAME_DT_FIXED", "JCE_STREAM_SYNC", "JCE_TAA_JITTER_PHASE"):
        if key not in jd.DETERMINISM:
            fail("DETERMINISM is missing %s -- a capture without it does not repeat"
                 % key)
    # 1b. the list must cover everything that actually launches the engine
    for name in discovered_measuring_tools():
        if name not in MEASURING_TOOLS:
            fail("tools/%s starts a process and drives the engine through JCE_* "
                 "environment variables, but it is not in MEASURING_TOOLS -- so "
                 "nothing checks that it uses the shared determinism recipe. "
                 "Add it to MEASURING_TOOLS (and to CAPTURING_TOOLS if it "
                 "photographs the engine)." % name)

    for name in MEASURING_TOOLS:
        src = source_of(name)
        if src is None:
            fail("%s not found; this gate's tool list has drifted" % name)
            continue
        if name in CAPTURING_TOOLS and "def assert_has_content" in src:
            fail("%s declares its own assert_has_content -- the blank-frame "
                 "guard must be imported from jce_determinism.  A blank capture "
                 "compares perfectly equal to another blank capture, so a tool "
                 "without this guard passes vacuously (visual_diff.py did, three "
                 "times)" % name)
        # Count CALL SITES, not the import.  The first version of this check
        # tested `"assert_has_content" in src`, which the import line alone
        # satisfies -- deleting every call still passed.
        call_sites = sum(1 for ln in src.splitlines()
                         if "assert_has_content(" in ln
                         and not ln.lstrip().startswith(("from ", "import "))
                         and "noqa" not in ln)
        if name in CAPTURING_TOOLS and call_sites == 0:
            fail("%s imports assert_has_content but never CALLS it, so it can "
                 "compare two blank frames and report perfect agreement" % name)
        if "DETERMINISM = {" in src:
            fail("%s declares its own DETERMINISM -- the recipe must be imported "
                 "from jce_determinism, not copied (three copies of a wrong "
                 "shaderc probe is how this repo learned that)" % name)
        if "from jce_determinism import" not in src:
            fail("%s does not import jce_determinism, so it captures with no "
                 "determinism pins" % name)

    # 2. the table matches the engine
    engine = engine_backend_names()
    if engine is None:
        fail("could not read the engine's accepted backend names from %s"
             % RENDERER_C.relative_to(REPO_ROOT).as_posix())
    else:
        known = set(jd.BGFX_NAME) | set(n for n in jd.NO_ASSERTION if n)
        missing = engine - known
        extra = known - engine
        if missing:
            fail("the engine accepts %s but jce_determinism does not know them -- "
                 "an unknown request cannot detect a bgfx fallback"
                 % ", ".join(sorted(missing)))
        if extra:
            fail("jce_determinism lists %s which the engine does not accept"
                 % ", ".join(sorted(extra)))

    # 3. the parser anchors on the module tag
    decoy = ("2026-08-27 [MAIN] SUCCESS - imgui_renderer: initialized (view 250) "
             "at jce_imgui_renderer.cpp:216\n"
             "2026-08-27 [MAIN] INFO - jce_renderer: renderer: Direct3D 11 "
             "at jce_renderer.c:1771\n")
    got = jd.actual_backend(decoy)
    if got != "Direct3D 11":
        fail("actual_backend() returned %r on a log whose FIRST 'renderer: ' is "
             "imgui_renderer's -- it must anchor on the jce_renderer tag" % got)

    # 4. the assertion accepts truth and refuses the three lies
    def refuses(text, backend, why):
        try:
            jd.assert_backend(text, backend)
        except RuntimeError:
            return
        fail("assert_backend accepted %s" % why)

    try:
        if jd.assert_backend(decoy, "d3d11") != "Direct3D 11":
            fail("assert_backend did not return the reported name on a match")
    except RuntimeError as exc:
        fail("assert_backend rejected a correct run: %s" % exc)

    refuses(decoy, "opengl", "a bgfx FALLBACK (asked opengl, got Direct3D 11)")
    refuses("nothing here at all\n", "d3d11", "a run with no renderer line")
    refuses(decoy, "no_such_backend", "an UNKNOWN backend name")

    # "OpenGL" must not satisfy "opengles" and vice versa -- a substring test
    # would accept both.
    gles = decoy.replace("Direct3D 11", "OpenGLES")
    refuses(gles, "opengl", "OpenGLES as a match for opengl")
    gl = decoy.replace("Direct3D 11", "OpenGL")
    refuses(gl, "opengles", "OpenGL as a match for opengles")

    # 5. the blank-frame guard actually rejects a uniform frame
    W = H = 1600
    blank = (W, H, 3, bytearray([30]) * (W * H * 3))
    try:
        jd.assert_has_content(blank, "<synthetic blank>")
        fail("assert_has_content accepted a uniform frame -- two of those "
             "compare perfectly equal")
    except RuntimeError:
        pass
    noisy = bytearray(blank[3])
    for i in range(0, len(noisy), 997):
        noisy[i] = 200
    try:
        jd.assert_has_content((W, H, 3, noisy), "<synthetic noisy>")
    except RuntimeError as exc:
        fail("assert_has_content rejected a frame that HAS content: %s" % exc)


    # 4. every env var these tools set must have a reader in the engine
    #
    # JCE_LOG_FILE was set by perf_bench.py for months and read only by the
    # editor, so a tool pointed at a shipped game produced no log at all and
    # reported "the run produced nothing" -- a wiring hole that looks exactly
    # like a broken build.  A variable nobody reads is not a setting.
    engine_src = REPO_ROOT / "engine" / "src"
    editor_src = REPO_ROOT / "editor" / "src"
    readers = ""
    for root in (engine_src, editor_src):
        for f in root.rglob("*.c"):
            readers += f.read_text(encoding="utf-8", errors="replace")
        for f in root.rglob("*.cpp"):
            readers += f.read_text(encoding="utf-8", errors="replace")
    wanted = set(jd.DETERMINISM) | {"JCE_LOG_FILE", "JCE_PERF_LOG",
                                    "JCE_MAX_FRAMES", "JCE_BACKEND"}
    for var in sorted(wanted):
        if '"%s"' % var not in readers:
            fail("%s is set by the capture tools but nothing in engine/src or "
                 "editor/src reads it -- setting a variable no binary consumes "
                 "produces a run with none of the pins it claims" % var)

    # The failure gate must be the LAST thing before the summary.  It used to
    # sit in the middle: sections 4 and 5 ran after it, so every fail() they
    # recorded was collected into a list nobody read.  The blank-frame guard's
    # own synthetic test -- the check that proves the guard can still say no --
    # was one of them, and the negative control for the env-var check returned
    # exit 0 twice before this was spotted.
    if failures:
        print("check_determinism_contract: FAILED")
        for f in failures:
            print("    %s" % f)
        return 1

    # Report the two counts SEPARATELY.  An earlier version printed the
    # capturing count next to the words "recipe single-sourced", so adding a
    # third measuring tool would have left the summary quietly understating
    # its own coverage.
    print("check_determinism_contract: OK - recipe single-sourced across %d "
          "measuring tool(s) (%s), blank-frame guard enforced on %d capturing "
          "tool(s) (%s), backend table matches the engine's %d accepted names, "
          "parser anchors on the module tag, assertion refuses fallback / "
          "missing / unknown"
          % (len(MEASURING_TOOLS), ", ".join(MEASURING_TOOLS),
             len(CAPTURING_TOOLS), ", ".join(CAPTURING_TOOLS),
             len(engine or ())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
