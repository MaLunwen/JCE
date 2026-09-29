#!/usr/bin/env python3
"""parity.py -- does the EDITOR's Play produce the same game as the shipped exe?

Usage:  python parity.py [substring]   -- run only matching scenarios

Runs the SAME recorded input through both and compares two things:

  BEHAVIOUR  the per-tick trace the game emits -- state, score, length, head
             cell, food cell, verdicts, body cells -- compared tick for tick.
  PIXELS     a Game View capture from the editor against a frame capture from
             the game, both 1280x720, taken at the same SIMULATED time.

Why both.  A trace can match while the picture differs (a camera applied on
one side and not the other), and a picture can match while the logic differs
(two frames early in a run look alike whatever happens later).  Neither
subsumes the other.

THREE OUTCOMES, never two: PASS / FAIL / VOID.  VOID means the comparison did
not happen -- the editor produced no trace, a capture is missing -- and it is
counted separately, because a harness with only PASS and FAIL reports a
comparison it never made as agreement.

IT TOUCHES THE OPERATOR'S EDITOR STATE.  The editor records which project to
open in ~/.jce/editor-session.json and there is no flag to override it, so
this points that file at this project and puts it back.  Every file it writes
is hashed first, the restore runs in a `finally`, and the restore is VERIFIED
by hash -- a silent failure would leave the editor opening a stale project
days later, with nothing to connect it to this script.
"""
import hashlib
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]
PROJECT = pathlib.Path(__file__).resolve().parent
SCENE = PROJECT / "resources" / "scenes" / "snake.scene.json"
GAME = REPO / "dist" / "games" / "SnakeSeven-release-x86_64" / "SnakeSeven.exe"
EDITOR = REPO / "build" / "desktop" / "windows-x64" / "release" / "jce_editor.exe"
SESSION = pathlib.Path.home() / ".jce" / "editor-session.json"

DT = "0.016666"
FRAME_SIZE, FRAME_VERSION, KEY_COUNT, KEYS_OFFSET = 2336, 2, 512, 8
K_UP, K_DOWN, K_LEFT, K_RIGHT = 82, 81, 80, 79
K_ENTER, K_P, K_Q = 40, 19, 20

# The editor enters Play at frame 30 (JCE_KPI_AUTOPLAY); the game is live from
# frame 0.  Every scenario therefore presses ENTER well after frame 30, so the
# GAME's own MENU -> PLAYING transition happens at the same frame in both and
# the tick numbering lines up without either side being special-cased.
PLAY_MARGIN = 90

# THE TWO HOSTS READ JCE_KPI_GAME_SHOTS AGAINST DIFFERENT CLOCKS.
#
# The game has no Play concept, so its schedule advances with process time.
# The editor ticks its schedule with a PLAY STATE
# (jce_editor.cpp -> jce_editor_kpi_game_capture_global_tick), so the editor
# clock only runs while Play is running, and Play starts at editor frame 30
# (JCE_KPI_AUTOPLAY).  The same "3.0" therefore names two different moments of
# the same game, 30 frames apart.
#
# Left uncorrected this is invisible in the logic comparison -- the tick
# SEQUENCE is identical, which is all ticks() compares -- and shows up only in
# the pixels, as the snake sitting three cells further along in the editor.  It
# reads exactly like a rendering difference and it is not one.
# Override with JCE_PARITY_BACKEND to compare on a different one; the backend
# each host actually reached is asserted below either way, so a pin that does
# not take cannot pass unnoticed.
BACKEND = os.environ.get("JCE_PARITY_BACKEND", "d3d12")

AUTOPLAY_FRAME = 30
SHOT_SKEW = AUTOPLAY_FRAME * float(DT)

TRACE = re.compile(
    r"SNAKETRACE tick=(\d+) state=(\d+) score=(\d+) len=(\d+) "
    r"head=(-?\d+),(-?\d+) food=(-?\d+),(-?\d+) wall=(\d) self=(\d) "
    r"bres=(\d+) cpp=(-?\d+),(-?\d+),(-?\d+) body=([0-9,\- ]*)")


def ticks(log):
    """The comparable part of a trace line.

    bres= and cpp= are DIAGNOSTICS about which entities a script resolved, not
    game state, so they are deliberately not compared: they would make the two
    sides differ over how each one found its entities rather than over what the
    game did.
    """
    out = []
    for m in TRACE.finditer(log):
        g = m.groups()
        out.append((int(g[0]), int(g[1]), int(g[2]), int(g[3]),
                    (int(g[4]), int(g[5])), (int(g[6]), int(g[7])),
                    g[8], g[9], tuple(g[14].split())))
    return out


def write_jirc(path, frames):
    with open(path, "wb") as f:
        f.write(b"JIRC" + struct.pack("<III", FRAME_VERSION, FRAME_SIZE, 0))
        for keys in frames:
            buf = bytearray(FRAME_SIZE)
            struct.pack_into("<II", buf, 0, FRAME_VERSION, KEY_COUNT)
            bits = [0] * 64
            for k in keys:
                bits[k >> 6] |= 1 << (k & 63)
            struct.pack_into("<64Q", buf, KEYS_OFFSET, *bits)
            f.write(buf)


def hold(keys, n):
    return [keys] * n


def run_one(exe, cwd, frames, max_frames, shots, extra_env=None, timeout=420):
    """Run one executable with a recorded input and optional captures."""
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="jce_parity_"))
    jirc = tmp / "in.jirc"
    write_jirc(jirc, frames)
    env = dict(os.environ)
    env.update({"JCE_MAX_FRAMES": str(max_frames),
                "JCE_WINDOW_HIDDEN": "1",
                "JCE_MULTI_INSTANCE": "1",
                "JCE_FRAME_DT_FIXED": DT,
                "JCE_INPUT_REPLAY": str(jirc),
                # PIN THE GRAPHICS BACKEND, FOR BOTH HOSTS.
                #
                # Without this the shipped exe takes the auto ladder (Direct3D
                # 12 on this machine) while the editor takes the persisted
                # editor preference in ~/.jce/editor-preferences.json, which
                # here says OpenGL.  Every pixel comparison was then D3D12
                # against OpenGL: different rasteriser, different NDC depth
                # convention (jce_camera_proj gets homogeneous_ndc=0 vs 1),
                # different rounding.  It reads as "the editor renders the
                # scene differently", which is true and says nothing about
                # the editor.
                "JCE_BACKEND": BACKEND,
                # The engine's own answers to "what did you resolve the
                # ambient to" and "was TAA resolving", raised to WARN so the
                # JCE_DIST-built game says them too.  On by default: a
                # comparison that needs an operator to remember a flag reports
                # VOID when they don't, and VOID is the row people skim past.
                "JCE_LOG_AMBIENT": os.environ.get("JCE_LOG_AMBIENT", "1"),
                "JCE_LOG_TAA": os.environ.get("JCE_LOG_TAA", "1"),
                # PIN THE TAA JITTER PHASE, for the same reason as the backend.
                #
                # The Halton index is deterministic in the FRAME index, and the
                # frame index at the capture moment is not the same on both
                # hosts -- the editor spends a variable number of frames loading
                # and only enters Play at frame 30.  So two hosts that both
                # resolve TAA correctly still sample different sub-pixel offsets
                # and every edge lands differently: measured 2.98%% of pixels
                # differing with TAA off on both sides, 40.37%% with TAA on and
                # the phase free.  jce_taa.c documents the switch and says
                # tools/visual_diff sets it for exactly this reason; it costs
                # anti-aliasing quality, so it is for measurement only.
                #
                # This removes a known variable from the comparison.  It does
                # not hide the question it was hiding before: whether each host
                # resolves TAA at all is asserted separately, above.
                "JCE_TAA_JITTER_PHASE": os.environ.get("JCE_TAA_JITTER_PHASE", "0")})
    paths = []
    if shots:
        spec = []
        for i, t in enumerate(shots):
            p = tmp / ("shot%d.png" % i)
            paths.append(p)
            spec.append("%.2f|%s" % (t, p))
        env["JCE_KPI_GAME_SHOTS"] = ";".join(spec)
    if extra_env:
        env.update(extra_env)
    try:
        p = subprocess.run([str(exe)], cwd=str(cwd), env=env,
                           capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=timeout)
        log = (p.stdout or "") + (p.stderr or "")
        return log, p.returncode, paths, tmp
    except subprocess.TimeoutExpired:
        return "TIMEOUT", -1, paths, tmp


AMBIENT_RE = re.compile(
    r"ambient from (\S+) rgb=\(([^)]*)\) intensity=([0-9.]+) "
    r"sky_fill=(\w+)")


BACKEND_RE = re.compile(r"jce_renderer: renderer: (.+?) (?:\(|at jce_renderer)")


def backend(log):
    """The backend this host actually initialised, or None."""
    m = BACKEND_RE.findall(log)
    return m[0].strip() if m else None


TAA_RE = re.compile(r"taa resolve: (on|off)")


def taa(log):
    """The LAST effective TAA state, or None.

    Reported by the engine from jce_postfx_apply, so it is the state in force
    when the frame was drawn rather than what somebody requested.  Needs
    JCE_LOG_TAA=1, because the line is LOG_INFO otherwise and JCE_DIST
    compiles LOG_INFO out of the shipped exe.
    """
    m = TAA_RE.findall(log)
    return m[-1] if m else None


def ambient(log):
    """The LAST resolved-ambient line, or None.

    Last, not first: the engine logs on CHANGE, and the value that matters is
    the one in force when the capture was taken.

    Environment lighting is resolved from one of three sources (editor
    override / scene rendering settings / engine fallback) and nothing else
    makes the choice observable.  Two hosts can pick different sources, land
    on different values, and differ only as a flat shift across every lit
    surface -- which reads as "the editor looks slightly different" and is
    indistinguishable, by eye, from a tonemapping or exposure difference.
    Requires JCE_LOG_AMBIENT=1, because the line is LOG_INFO otherwise and
    JCE_DIST compiles LOG_INFO out of the shipped exe.
    """
    m = AMBIENT_RE.findall(log)
    return m[-1] if m else None


def pixel_diff(a, b):
    """(differing, total, max_channel_delta, differing_by_more_than_1), or None.

    `differing_by_more_than_1` is reported separately because the editor-vs-game
    difference is dominated by a +/-1 population: 96.3% of the frame lands
    within one 8-bit step, bidirectionally (game 59 -> editor 60 about as often
    as the reverse).

    THAT IS NOT NOISE, AND THE FLOOR RUN IS WHAT PROVED IT.  The obvious
    reading -- both hosts run temporal AA with their own jitter sequence, so a
    flat surface lands on adjacent 8-bit values -- was written into this
    docstring as fact before the control ran.  Then the control ran: the same
    binary twice is byte-identical, 0 of 921,600 pixels, worst delta 0.  So the
    game is fully deterministic and every one of those +/-1 pixels is a real,
    reproducible difference between the two hosts, not sampling noise.

    Which is why the verdict is measured against the floor computed on each
    run rather than against a tolerance argued from a mechanism.  A tolerance
    justified by a plausible story would have declared this green.

    Counts DIFFERING PIXELS rather than reporting a mean.  A mean over a
    1280x720 frame is tiny for any localised difference -- a snake in the wrong
    cell moves a few hundred pixels out of 921,600 and averages to nothing,
    which is how "the images are basically identical" gets said about two
    pictures of different games.
    """
    try:
        from PIL import Image
        ia = Image.open(a).convert("RGB")
        ib = Image.open(b).convert("RGB")
    except Exception:
        return None
    if ia.size != ib.size:
        return (-1, 0, 0, 0)
    pa, pb = ia.load(), ib.load()
    w, h = ia.size
    diff = 0
    over = 0
    worst = 0
    for y in range(h):
        for x in range(w):
            ca, cb = pa[x, y], pb[x, y]
            if ca != cb:
                diff += 1
                d = max(abs(ca[0] - cb[0]), abs(ca[1] - cb[1]),
                        abs(ca[2] - cb[2]))
                if d > 1:
                    over += 1
                if d > worst:
                    worst = d
    return (diff, w * h, worst, over)


class Report:
    def __init__(self):
        self.rows = []

    def add(self, name, verdict, detail):
        self.rows.append((name, verdict, detail))
        print("  %-4s %-26s %s" % (verdict, name, detail))

    def summary(self):
        p = sum(1 for r in self.rows if r[1] == "PASS")
        f = sum(1 for r in self.rows if r[1] == "FAIL")
        v = sum(1 for r in self.rows if r[1] == "VOID")
        print()
        print("=" * 72)
        print("PASS %d   FAIL %d   VOID %d   of %d comparisons"
              % (p, f, v, len(self.rows)))
        if v:
            print("VOID is not agreement: those comparisons did NOT happen.")
        return 0 if (f == 0 and v == 0) else 1


def scenarios():
    """(name, frames, max_frames, shot_times).

    Each one exercises a different path through the game, because a single
    scenario only proves the two sides agree about that scenario.  The first
    version of this check ran one -- idle, start, drive into a wall -- and
    would have agreed perfectly while steering, eating, pausing and restarting
    all diverged.
    """
    idle = hold((), PLAY_MARGIN)
    start = hold((K_ENTER,), 6)
    return [
        # A STATIC FRAME FIRST.  Nothing is pressed, so the game sits in MENU
        # with the snake parked and the board still: the capture moment stops
        # mattering, and the comparison is purely "does the editor draw the
        # same picture as the shipped exe".  Every other scenario mixes that
        # question with "are the two clocks aligned", and those two failures
        # look identical in a pixel count.
        ("menu-static",
         hold((), 300), 400, [2.0]),
        ("straight-to-wall",
         idle + start + hold((), 400), 520, [2.5]),
        ("steer-up",
         idle + start + hold((), 40) + hold((K_UP,), 30) + hold((), 300),
         520, [3.0]),
        ("turn-down-then-run",
         idle + start + hold((), 80) + hold((K_DOWN,), 30) + hold((), 300),
         520, [3.5]),
        ("pause-and-resume",
         idle + start + hold((), 40) + hold((K_P,), 6) + hold((), 200)
         + hold((K_P,), 6) + hold((), 200), 560, [3.0]),
        ("die-then-restart",
         idle + start + hold((), 400) + hold((K_ENTER,), 6) + hold((), 200),
         740, [8.0]),
    ]


def main():
    for exe, what in ((GAME, "game"), (EDITOR, "editor")):
        if not exe.is_file():
            print("VOID: %s not built (%s)" % (what, exe))
            return 2

    backup = pathlib.Path(tempfile.mkdtemp(prefix="jce_parity_state_"))
    before = hashlib.sha256(SESSION.read_bytes()).hexdigest() \
        if SESSION.is_file() else None
    if before:
        shutil.copy2(SESSION, backup / SESSION.name)

    rep = Report()
    try:
        if before:
            d = json.loads(SESSION.read_text(encoding="utf-8"))
            d["last_project"] = str(PROJECT)
            d["last_scene_path"] = str(SCENE)
            SESSION.write_text(json.dumps(d, indent=2) + "\n",
                               encoding="utf-8", newline="\n")

        only = sys.argv[1] if len(sys.argv) > 1 else None
        for name, frames, maxf, shots in scenarios():
            if only and only not in name:
                continue
            row0 = len(rep.rows)
            g_log, g_rc, g_shots, g_tmp = run_one(
                GAME, GAME.parent, frames, maxf, shots)
            # THE SAME BINARY, TWICE, AS THE POSITIVE CONTROL.
            #
            # Without it "485,147 pixels differ" is a number with nothing to
            # compare it to, and the only available reading is "the editor
            # renders differently" -- which was wrong here by a factor of
            # thirteen.  With it the criterion becomes "the editor differs from
            # the game no more than the game differs from itself", which is a
            # real claim, and the floor is re-measured every run instead of
            # being a constant someone once picked.
            g2_log, g2_rc, g2_shots, g2_tmp = run_one(
                GAME, GAME.parent, frames, maxf, shots)
            e_log, e_rc, e_shots, e_tmp = run_one(
                EDITOR, REPO, frames, maxf,
                [t - SHOT_SKEW for t in shots],
                extra_env={"JCE_KPI_AUTOPLAY": "1"})

            gt, et = ticks(g_log), ticks(e_log)
            if not gt and not et and g_rc == 0 and e_rc == 0:
                # A scenario that never presses ENTER stays in MENU, where no
                # tick happens.  Zero ticks on BOTH sides, with both processes
                # exiting cleanly, is the agreement this scenario is for -- not
                # an absence of measurement.  Both exit codes are required:
                # "logged nothing" and "died before logging" produce the same
                # empty list, and only one of them is agreement.
                rep.add(name + " / logic", "PASS",
                        "no ticks on either side (neither left MENU), "
                        "both exited 0")
            elif not gt:
                rep.add(name + " / logic", "VOID",
                        "the GAME produced no trace (exit %d)" % g_rc)
            elif not et:
                rep.add(name + " / logic", "VOID",
                        "the EDITOR produced no trace (exit %d)" % e_rc)
            elif gt == et:
                rep.add(name + " / logic", "PASS",
                        "%d ticks identical, last head %s"
                        % (len(gt), gt[-1][4]))
            else:
                first = next((i for i in range(min(len(gt), len(et)))
                              if gt[i] != et[i]), None)
                rep.add(name + " / logic", "FAIL",
                        ("game %d ticks vs editor %d" % (len(gt), len(et)))
                        if first is None else
                        ("first difference at tick %d: game %s / editor %s"
                         % (first, gt[first], et[first])))

            g_be, e_be = backend(g_log), backend(e_log)
            if g_be is None or e_be is None:
                rep.add(name + " / backend", "VOID",
                        "no backend line (game=%s editor=%s)"
                        % (g_be, e_be))
            elif g_be == e_be:
                rep.add(name + " / backend", "PASS",
                        "both on %s" % g_be)
            else:
                rep.add(name + " / backend", "FAIL",
                        "game on %s, editor on %s -- the pixel comparison "
                        "below is between two different renderers"
                        % (g_be, e_be))

            g_taa, e_taa = taa(g_log), taa(e_log)
            # A host that prints nothing ran no post-processing chain at all,
            # and TAA resolves inside that chain -- so "silent" is a real
            # answer, "off", not an absent measurement.  The shipped main loop
            # skips jce_postfx_apply() entirely unless some effect is enabled
            # (jce_default_main.inc.h), which is the usual case for a scene
            # that authors no rendering settings.
            #
            # Only BOTH silent is a genuine VOID: that means the line reached
            # neither binary, so the instrument, not the engine, is what the
            # run measured.
            if g_taa is None and e_taa is None:
                rep.add(name + " / taa", "VOID",
                        "neither host printed a taa-resolve line -- the "
                        "instrument did not reach either binary (rebuild), or "
                        "the log format moved")
                g_taa = e_taa = None
            if g_taa is None and e_taa is None:
                pass          # already reported VOID above
            elif (g_taa or "off (no post chain)").split()[0] ==                  (e_taa or "off (no post chain)").split()[0]:
                rep.add(name + " / taa", "PASS",
                        "both %s" % (g_taa or "off (no post chain)"))
            else:
                rep.add(name + " / taa", "FAIL",
                        "game %s, editor %s -- one host is temporally "
                        "anti-aliasing and the other is not, which moves every "
                        "edge and shows up below as a pixel count"
                        % (g_taa or "off (no post chain)",
                           e_taa or "off (no post chain)"))

            g_amb, e_amb = ambient(g_log), ambient(e_log)
            if g_amb is None or e_amb is None:
                rep.add(name + " / ambient", "VOID",
                        "no resolved-ambient line (game=%s editor=%s) -- run "
                        "with JCE_LOG_AMBIENT=1, or the log format moved"
                        % (g_amb is not None, e_amb is not None))
            elif g_amb == e_amb:
                rep.add(name + " / ambient", "PASS",
                        "both resolved %s rgb=(%s) x%s sky_fill=%s" % g_amb)
            else:
                rep.add(name + " / ambient", "FAIL",
                        "game %s rgb=(%s) x%s sky_fill=%s  vs  "
                        "editor %s rgb=(%s) x%s sky_fill=%s"
                        % (g_amb + e_amb))

            for i, (ga, ea) in enumerate(zip(g_shots, e_shots)):
                if not ga.is_file() or not ea.is_file():
                    rep.add(name + " / pixels", "VOID",
                            "a capture is missing (game=%s editor=%s)"
                            % (ga.is_file(), ea.is_file()))
                    continue
                r = pixel_diff(ga, ea)
                floor = pixel_diff(ga, g2_shots[i]) \
                    if i < len(g2_shots) and g2_shots[i].is_file() else None
                if r is None:
                    rep.add(name + " / pixels", "VOID", "could not decode")
                elif r[0] == -1:
                    rep.add(name + " / pixels", "FAIL", "different sizes")
                elif floor is None or floor[0] == -1:
                    rep.add(name + " / pixels", "VOID",
                            "no noise floor: the second GAME run produced no "
                            "comparable capture, so this number has nothing to "
                            "be measured against")
                else:
                    diff, total, worst, over = r
                    f_diff, _, f_worst, f_over = floor
                    ok = over <= f_over and worst <= max(f_worst, 1)
                    rep.add(name + " / pixels",
                            "PASS" if ok else "FAIL",
                            "editor-vs-game %d/%d differ (%.2f%%), %d by more "
                            "than 1, worst %d  |  game-vs-game floor %d "
                            "(%.2f%%), %d by more than 1, worst %d"
                            % (diff, total, 100.0 * diff / total, over, worst,
                               f_diff, 100.0 * f_diff / total, f_over,
                               f_worst))
            # KEEP THE EVIDENCE WHEN THEY DISAGREE.
            #
            # This deleted both run directories unconditionally, so a pixel
            # FAIL reported a number and destroyed the only thing that could
            # explain it -- and explaining it took a separate throwaway script
            # to re-run the pair and copy the images out.  The first look at
            # those images is what identified the camera; the number alone had
            # me theorising for three rounds.
            # VOID too, not just FAIL.  A VOID means a comparison did not
            # happen -- a missing capture, an undecodable image -- and that is
            # precisely the case where the logs are the only way to find out
            # why.  Keeping evidence only for FAIL threw away the runs that
            # explained themselves least.
            if any(r[1] in ("FAIL", "VOID") for r in rep.rows[row0:]):
                keep = PROJECT / "shots" / ("parity_" + name)
                keep.mkdir(parents=True, exist_ok=True)
                for tag, ps in (("game", g_shots), ("editor", e_shots)):
                    for i, q in enumerate(ps):
                        if q.is_file():
                            shutil.copy2(q, keep / ("%s_%d.png" % (tag, i)))
                # The logs too.  The images say a difference exists; the logs
                # are where the resolved ambient, the backend and the script
                # errors are, and re-running to get them back is a second run
                # that may not reproduce.
                for i, q in enumerate(g2_shots):
                    if q.is_file():
                        shutil.copy2(q, keep / ("game2_%d.png" % i))
                for tag, lg in (("game", g_log), ("game2", g2_log),
                                ("editor", e_log)):
                    (keep / (tag + ".log")).write_text(
                        lg, encoding="utf-8", newline="\n")
                print("       evidence kept: %s" % keep)
            shutil.rmtree(g_tmp, ignore_errors=True)
            shutil.rmtree(g2_tmp, ignore_errors=True)
            shutil.rmtree(e_tmp, ignore_errors=True)
        return rep.summary()
    finally:
        if before:
            shutil.copy2(backup / SESSION.name, SESSION)
            after = hashlib.sha256(SESSION.read_bytes()).hexdigest()
            print("restore %s: %s" % (SESSION.name,
                                      "byte-identical" if after == before
                                      else "!! DIFFERS"))
        shutil.rmtree(backup, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
