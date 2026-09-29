#!/usr/bin/env python3
"""verify.py -- run the game and check the spec's twenty acceptance criteria.

Every check has THREE outcomes, never two:

    PASS   the game did the thing
    FAIL   the game did the wrong thing
    VOID   the check could not run, so it says NOTHING about the game

VOID exists because a harness with only PASS and FAIL reports a measurement
it never made as a pass.  That has happened on this project more than once:
a content guard that could not execute returned "no complaint", which read
as approval.  A VOID is not a soft failure -- it means the run is not
evidence, and the summary counts it separately so it cannot be mistaken for
one.

The evidence is the SNAKETRACE line the Lua module emits once per tick.  It
carries everything needed to judge that tick -- state, score, length, head
cell, food cell, verdicts, body cells -- so no verdict is ever assembled
from two lines that might not be from the same tick.
"""
import os
import pathlib
import re
import struct
import subprocess
import sys
import time

# Derived from this file's own location, not hard-coded: this script lives in
# the repository now, and an absolute path baked in here works on exactly one
# machine and fails on every clone with a "the game is not built" VOID that
# says nothing about the game.
#   <repo>/examples/snake_seven/verify.py  ->  <repo>/dist/games/...
_REPO = pathlib.Path(__file__).resolve().parents[2]
PKG = pathlib.Path(os.environ.get(
    "SNAKE_PKG", _REPO / "dist" / "games" / "SnakeSeven-release-x86_64"))
EXE = PKG / ("SnakeSeven.exe" if os.name == "nt" else "SnakeSeven")
BEST = PKG / "snake_best.txt"

COLS, ROWS = 30, 20
FOOD_SCORE = 10
INITIAL_LEN = 3            # body segments; snake length 4 including the head

# Keycodes, as the .jirc format records them.
K_UP, K_DOWN, K_LEFT, K_RIGHT = 82, 81, 80, 79
K_W, K_A, K_S, K_D = 26, 4, 22, 7
K_ENTER, K_SPACE, K_ESC, K_P, K_Q = 40, 44, 41, 19, 20

TRACE = re.compile(
    r"SNAKETRACE tick=(\d+) state=(\d+) score=(\d+) len=(\d+) "
    r"head=(-?\d+),(-?\d+) food=(-?\d+),(-?\d+) wall=(\d) self=(\d) "
    # bres= and cpp= sit between self= and body=.  They were added to the
    # trace and this pattern was not, so every line stopped matching and the
    # harness reported "the game never ticked" -- a statement about the game,
    # produced by a change to the instrument.  A trace format and its readers
    # have to move together; there is more than one reader.
    r"bres=(\d+) cpp=(-?\d+),(-?\d+),(-?\d+) "
    r"body=([0-9,\- ]*)")

# The tick clock only runs while PLAYING, so everything that happens AFTER a
# game ends -- which is most of what criteria 10 and 18 are about -- is only
# ever visible on an idle line.  A harness that read SNAKETRACE alone would
# see a game that simply stopped producing output and would have to guess
# why.
IDLE = re.compile(
    r"SNAKEIDLE state=(\d+) score=(\d+) intent=(\d+) pend=(-?\d+),(-?\d+) "
    r"dir=(-?\d+),(-?\d+) enter=(\w+) head=(-?\d+),(-?\d+) "
    r"wall=(\d) self=(\d)")


class Idle:
    __slots__ = ("state", "score", "intent", "head", "wall", "self_hit")

    def __init__(self, m):
        self.state = int(m.group(1))
        self.score = int(m.group(2))
        self.intent = int(m.group(3))
        self.head = (int(m.group(9)), int(m.group(10)))
        self.wall = m.group(11) == "1"
        self.self_hit = m.group(12) == "1"

FRAME_SIZE = 2336          # sizeof(JceInputFrame)
FRAME_VERSION = 2          # JCE_INPUT_FRAME_VERSION
KEY_COUNT = 512            # JCE_KEY_COUNT
KEYS_OFFSET = 8            # after `uint32_t version, key_count`


class Tick:
    __slots__ = ("tick", "state", "score", "length", "head", "food",
                 "wall", "self_hit", "body")

    def __init__(self, m):
        self.tick = int(m.group(1))
        self.state = int(m.group(2))
        self.score = int(m.group(3))
        self.length = int(m.group(4))
        self.head = (int(m.group(5)), int(m.group(6)))
        self.food = (int(m.group(7)), int(m.group(8)))
        self.wall = m.group(9) == "1"
        self.self_hit = m.group(10) == "1"
        self.body = [tuple(int(v) for v in c.split(","))
                     for c in m.group(15).split() if c]


def write_jirc(path, frames):
    """frames: list of iterables of keycodes held during that frame.

    Container (jce_input_record.c): "JIRC", version, frame_size, pad=0, then
    frame_size-byte frames.  frame_size is in the header precisely so a
    stride mismatch is caught at open rather than producing garbage that
    looks like input.

    Inside a JceInputFrame the key bits are NOT at offset 0: the struct
    opens with `uint32_t version, key_count`, so keys_bits[64] begins at
    byte 8.  Writing the bits at 0 sets those two fields instead, and the
    replay then reads a frame with no keys held -- every steering check
    would come back "the key did nothing", which is indistinguishable from
    a real input bug.
    """
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


def run(name, frames=None, max_frames=600, extra_env=None, keep_best=False):
    """Run the game once.  Returns (ticks, log, returncode)."""
    env = dict(os.environ)
    env["JCE_MAX_FRAMES"] = str(max_frames)
    env["JCE_WINDOW_HIDDEN"] = "1"
    env["JCE_MULTI_INSTANCE"] = "1"
    # A fixed timestep makes tick counts comparable between runs; without it
    # the same input produces a different number of ticks on a busy machine
    # and every count-based check becomes a coin flip.
    env["JCE_FRAME_DT_FIXED"] = "0.016666"
    if extra_env:
        env.update(extra_env)
    if not keep_best and BEST.is_file():
        BEST.unlink()
    if frames is not None:
        jirc = PKG / ("_verify_%s.jirc" % name)
        write_jirc(jirc, frames)
        env["JCE_INPUT_REPLAY"] = str(jirc)
    try:
        p = subprocess.run([str(EXE)], cwd=str(PKG), env=env,
                           capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=300)
    except subprocess.TimeoutExpired:
        return [], "TIMEOUT", -1
    log = (p.stdout or "") + (p.stderr or "")
    return [Tick(m) for m in TRACE.finditer(log)], log, p.returncode


def idles(log):
    return [Idle(m) for m in IDLE.finditer(log)]


class Report:
    def __init__(self):
        self.rows = []

    def add(self, num, title, verdict, detail):
        self.rows.append((num, title, verdict, detail))
        mark = {"PASS": "PASS", "FAIL": "FAIL", "VOID": "VOID"}[verdict]
        print("  %-4s %2d. %-38s %s" % (mark, num, title, detail))

    def summary(self):
        p = sum(1 for r in self.rows if r[2] == "PASS")
        f = sum(1 for r in self.rows if r[2] == "FAIL")
        v = sum(1 for r in self.rows if r[2] == "VOID")
        print()
        print("=" * 72)
        print("PASS %d   FAIL %d   VOID %d   of %d criteria"
              % (p, f, v, len(self.rows)))
        if v:
            print("VOID is not a pass: those criteria were NOT measured.")
        return 0 if (f == 0 and v == 0) else 1


DT = 0.016666             # JCE_FRAME_DT_FIXED used by run()
STEP_INIT = 0.18          # spec 14, seconds per cell at score 0
TICK_FRAMES = STEP_INIT / DT      # ~10.8 frames per logic tick


def calibrate():
    """Learn which tick a key pressed at a known frame lands on.

    Input replay is OPEN LOOP: nothing tells the harness what frame the game
    is on, and "press DOWN around tick 9" is not expressible directly.  But
    the timestep is fixed, so ticks land on a regular frame grid -- the only
    unknown is the offset, i.e. which tick the game was on at a given frame.

    So: press DOWN at a known frame, see which tick actually turned, and
    solve for the offset.  Measured rather than computed, because the offset
    depends on when ENTER was processed and on the first frame's dt.

    Returns (frame_of_tick, food_cell) or (None, None).
    """
    # The press has to land while the snake is still ALIVE.  Heading right
    # from the centre it reaches the wall at about tick 15, which with a
    # 0.18 s step at 1/60 is frame ~200 -- so the first version of this
    # calibrated at frame 200 and pressed a key into a corpse, returned
    # None, and every eating criterion went VOID with "no score increase"
    # as though the game could not feed itself.
    f0 = 80
    frames = (hold((), 30) + hold((K_ENTER,), 6) + hold((), f0 - 36)
              + hold((K_DOWN,), 300))
    t, log, _ = run("cal", frames, max_frames=420)
    if len(t) < 3:
        return None, None
    turn = None
    for i in range(1, len(t)):
        if t[i].head[1] != t[i - 1].head[1]:
            turn = t[i].tick
            break
    if turn is None:
        return None, None

    def frame_of_tick(k):
        return int(round(f0 + (k - turn) * TICK_FRAMES))

    return frame_of_tick, t[0].food


def drive_to_food(frame_of_tick, food, start=(15, 10)):
    """Frames that walk the head right to the food's column, then to it.

    Returns None when the simple two-leg path cannot reach the food: the
    snake starts heading RIGHT and an exact reversal is refused, so a food
    to the left of the start is not reachable without a longer route.  That
    is a VOID, not a failure -- the game is fine, the script is too simple.
    """
    fx, fz = food
    sx, sz = start
    if fx <= sx or fz == sz:
        return None
    turn_tick = (fx - sx) + 1          # the tick that should move in z
    press_at = frame_of_tick(turn_tick)
    if press_at < 40:
        return None
    key = K_DOWN if fz > sz else K_UP
    tail = frame_of_tick(turn_tick + abs(fz - sz) + 4)
    return (hold((), 30) + hold((K_ENTER,), 6)
            + hold((), press_at - 36)
            + hold((key,), max(tail - press_at, 40)))


def sample_working_set(name, frames, max_frames, interval=1.0):
    """Run the game and sample its working set while it lives.

    Returns a list of byte counts.  An empty list means the process could
    not be sampled at all, which the caller must report as VOID rather than
    as a clean result -- "I never looked" and "I looked and it was fine"
    have to stay distinguishable.
    """
    env = dict(os.environ)
    env["JCE_MAX_FRAMES"] = str(max_frames)
    env["JCE_WINDOW_HIDDEN"] = "1"
    env["JCE_MULTI_INSTANCE"] = "1"
    env["JCE_FRAME_DT_FIXED"] = "0.016666"
    jirc = PKG / ("_verify_%s.jirc" % name)
    write_jirc(jirc, frames)
    env["JCE_INPUT_REPLAY"] = str(jirc)

    out = []
    proc = subprocess.Popen([str(EXE)], cwd=str(PKG), env=env,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        while proc.poll() is None:
            time.sleep(interval)
            try:
                r = subprocess.run(
                    ["powershell", "-NoProfile", "-Command",
                     "(Get-Process -Id %d -ErrorAction Stop).WorkingSet64"
                     % proc.pid],
                    capture_output=True, text=True, timeout=20)
                v = r.stdout.strip()
                if v.isdigit():
                    out.append(int(v))
            except Exception:
                break
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait(timeout=30)
    return out


def schedule(frame_of_tick, turns, total_ticks):
    """Frames that press each key so it is ADOPTED on the given tick.

    turns: [(tick, keycode)].  A direction is consumed by the snake module at
    a tick boundary, so the key must be held across the frame that tick lands
    on -- pressing exactly ON it is a coin flip against rounding.  Each key is
    held for one tick's width, centred on its target.
    """
    end = frame_of_tick(total_ticks) + int(TICK_FRAMES)
    frames = [() for _ in range(max(end, 60))]
    for i in range(30, 36):
        frames[i] = (K_ENTER,)
    for tick, key in turns:
        # NARROW, and centred ON the tick's own frame rather than the tick
        # before it.  frame_of_tick(k) is calibrated as "press here and it is
        # adopted at tick k", so a window one full tick wide reaches back into
        # tick k-1 and the turn lands EARLY.  Measured: a route meant to turn
        # down at column 23 turned at 22 and missed the food by one cell,
        # which then read as "the snake cannot eat".
        centre = frame_of_tick(tick)
        for i in range(max(centre - 1, 40), min(centre + 2, len(frames))):
            frames[i] = (key,)
    return frames


def hold(keys, n):
    return [keys] * n


def main():
    if not EXE.is_file():
        print("VOID: %s is not built" % EXE)
        return 2

    rep = Report()
    print("Running the game.  Each criterion is judged from SNAKETRACE.")
    print()

    # ---- Run A: start, then let it run into the wall unattended ----------
    # 60 frames idle (menu), ENTER, then nothing.  Heading right from the
    # centre, it reaches the wall in ~15 cells.
    frames_a = hold((), 30) + hold((K_ENTER,), 6) + hold((), 520)
    a, log_a, rc_a = run("a", frames_a)

    if rc_a != 0:
        rep.add(1, "game starts", "FAIL", "exit code %d" % rc_a)
    elif "runtime assets mounted" not in log_a:
        rep.add(1, "game starts", "FAIL", "assets never mounted")
    else:
        rep.add(1, "game starts", "PASS", "exit 0, assets mounted")

    if not a:
        for n, t in ((2, "menu enters the game"), (3, "snake moves by itself"),
                     (10, "wall ends the game"), (18, "no movement after death"),
                     (19, "body stays connected"), (20, "food stays in bounds"),
                     (6, "food never inside the snake")):
            rep.add(n, t, "VOID", "no SNAKETRACE lines: the game never ticked")
        print()
        return rep.summary()

    rep.add(2, "menu enters the game", "PASS",
            "%d ticks after ENTER; first state=%d" % (len(a), a[0].state))

    moved = len({t.head for t in a}) > 1
    rep.add(3, "snake moves by itself", "PASS" if moved else "FAIL",
            "%d distinct head cells with no steering" % len({t.head for t in a}))

    # 10 / 18 are judged from the IDLE lines: the tick clock stops at the
    # game over, so the death itself and everything after it is only there.
    ia = idles(log_a)
    dead = [i for i in ia if i.state == 3]
    if dead:
        by_wall = [i for i in dead if i.wall]
        rep.add(10, "wall ends the game", "PASS" if by_wall else "FAIL",
                ("GAMEOVER with wall=1 at head %s" % (by_wall[0].head,))
                if by_wall else
                "GAMEOVER reached but no wall verdict (self=%s)"
                % dead[0].self_hit)
        heads = {i.head for i in dead}
        rep.add(18, "no movement after death",
                "PASS" if len(heads) == 1 else "FAIL",
                "%d idle samples after death, %d distinct head cells %s"
                % (len(dead), len(heads), sorted(heads)[:3]))
    else:
        states = sorted({i.state for i in ia})
        rep.add(10, "wall ends the game", "VOID",
                "never reached GAMEOVER (idle states seen: %s)" % states)
        rep.add(18, "no movement after death", "VOID", "never died")

    # 19. body continuity: consecutive segments are always adjacent
    bad = None
    checked = 0
    for t in a:
        chain = [t.head] + t.body
        for i in range(len(chain) - 1):
            (x0, z0), (x1, z1) = chain[i], chain[i + 1]
            d = abs(x0 - x1) + abs(z0 - z1)
            checked += 1
            if d != 1:
                bad = (t.tick, chain[i], chain[i + 1], d)
                break
        if bad:
            break
    if checked == 0:
        rep.add(19, "body stays connected", "VOID",
                "no body segments appeared in any trace line")
    elif bad:
        rep.add(19, "body stays connected", "FAIL",
                "tick %d: %s -> %s is %d cells apart" % bad)
    else:
        rep.add(19, "body stays connected", "PASS",
                "%d adjacent pairs across %d ticks, all distance 1"
                % (checked, len(a)))

    # 20. food inside the board
    oob = [t for t in a if not (0 <= t.food[0] < COLS and 0 <= t.food[1] < ROWS)]
    rep.add(20, "food stays in bounds", "FAIL" if oob else "PASS",
            ("tick %d food at %s" % (oob[0].tick, oob[0].food)) if oob
            else "%d ticks, food always within 0..%d x 0..%d"
                 % (len(a), COLS - 1, ROWS - 1))

    # 6. food never on a cell the snake occupies
    clash = [t for t in a if t.food in ([t.head] + t.body)]
    rep.add(6, "food never inside the snake", "FAIL" if clash else "PASS",
            ("tick %d: food %s is under the snake"
             % (clash[0].tick, clash[0].food)) if clash
            else "%d ticks, food never on head or a traced segment" % len(a))

    # ---- Run B: steering ------------------------------------------------
    # ENTER, run right briefly, then press UP and require z to DECREASE.
    # An ABSOLUTE expectation, deliberately: the inverted-axis bug that got
    # through was invisible to every symmetric check, because the editor and
    # the runtime invert together and two wrong pictures compare equal.
    frames_b = (hold((), 30) + hold((K_ENTER,), 6) + hold((), 40)
                + hold((K_UP,), 30) + hold((), 200))
    b, log_b, rc_b = run("b", frames_b)
    if not b:
        rep.add(4, "arrow keys steer correctly", "VOID", "no trace lines")
        rep.add(5, "cannot reverse into itself", "VOID", "no trace lines")
    else:
        zs = [t.head[1] for t in b]
        turned = [i for i in range(1, len(b)) if b[i].head[1] < b[i - 1].head[1]]
        if turned:
            rep.add(4, "arrow keys steer correctly", "PASS",
                    "UP decreased z at tick %d (%d -> %d)"
                    % (b[turned[0]].tick, b[turned[0] - 1].head[1],
                       b[turned[0]].head[1]))
        else:
            rep.add(4, "arrow keys steer correctly", "FAIL",
                    "UP never decreased z; z went %s" % (zs[:8],))

        # 5. no reversal: consecutive head cells never repeat the cell before
        rev = None
        for i in range(2, len(b)):
            if b[i].head == b[i - 2].head and b[i].head != b[i - 1].head:
                rev = b[i].tick
                break
        rep.add(5, "cannot reverse into itself", "FAIL" if rev else "PASS",
                ("head returned to its previous cell at tick %d" % rev) if rev
                else "%d ticks, the head never stepped back onto its own "
                     "previous cell" % len(b))

    # ---- Run C: eating ---------------------------------------------------
    # Open-loop replay cannot chase a moving target, but the food's first
    # cell is deterministic (seeded from the run counter), so the harness
    # reads it from a calibration run and then drives straight to it.
    frame_of_tick, food0 = calibrate()
    eat_frames = (drive_to_food(frame_of_tick, food0)
                  if frame_of_tick and food0 else None)
    c = []
    if eat_frames:
        c, log_c, _ = run("c", eat_frames, max_frames=len(eat_frames) + 40)

    eat = None
    for src in (c, a, b):
        for i in range(1, len(src)):
            if src[i].score > src[i - 1].score:
                eat = (src[i - 1], src[i])
                break
        if eat:
            break

    if not eat:
        why = ("the food at %s is not reachable by the two-leg route this "
               "harness can script" % (food0,)) if food0 and not eat_frames               else "no tick in any run showed a score increase"
        for n, t in ((7, "eating grows the snake"),
                     (8, "eating adds 10 points"),
                     (9, "food respawns after being eaten")):
            rep.add(n, t, "VOID", why)
    else:
        before, after = eat
        rep.add(8, "eating adds 10 points",
                "PASS" if after.score - before.score == FOOD_SCORE else "FAIL",
                "score %d -> %d at tick %d"
                % (before.score, after.score, after.tick))
        rep.add(7, "eating grows the snake",
                "PASS" if after.length > before.length else "FAIL",
                "len %d -> %d" % (before.length, after.length))
        rep.add(9, "food respawns after being eaten",
                "PASS" if after.food != before.food else "FAIL",
                "food %s -> %s" % (before.food, after.food))

        # Best score, read NOW: the eating run ended at the wall, so the
        # manager has written it, and every later run() clears the file.
        if BEST.is_file():
            try:
                val = int(BEST.read_text().strip())
                rep.add(15, "best score is recorded",
                        "PASS" if val >= FOOD_SCORE else "FAIL",
                        "%s holds %d after a run that scored %d"
                        % (BEST.name, val, after.score))
            except ValueError:
                rep.add(15, "best score is recorded", "FAIL",
                        "%s is not a number" % BEST.name)
        else:
            rep.add(15, "best score is recorded", "VOID",
                    "the run scored %d but ended without a game over, so the "
                    "manager never wrote %s" % (after.score, BEST.name))

    # ---- Run D: pause ------------------------------------------------
    # The first version of this check compared tick COUNTS against an
    # unpaused reference and failed a working pause: the snake dies at the
    # wall after exactly 15 ticks either way, so the count is bounded by the
    # wall and not by the clock.  The count was the wrong quantity.
    #
    # What "pause stops the logic" actually means is that the HEAD DOES NOT
    # MOVE while paused, and the idle line carries the head.
    frames_d = (hold((), 30) + hold((K_ENTER,), 6) + hold((), 40)
                + hold((K_P,), 6) + hold((), 240) + hold((K_P,), 6)
                + hold((), 200))
    d, log_d, rc_d = run("d", frames_d)
    idl = idles(log_d)
    paused = [i for i in idl if i.state == 2]
    if not paused:
        rep.add(12, "pause stops the logic", "VOID",
                "the game never reached PAUSED (idle states: %s)"
                % sorted({i.state for i in idl}))
        rep.add(13, "resume continues the game", "VOID", "never paused")
    else:
        heads = {i.head for i in paused}
        rep.add(12, "pause stops the logic",
                "PASS" if len(heads) == 1 else "FAIL",
                "%d idle samples at PAUSED, %d distinct head cells %s"
                % (len(paused), len(heads), sorted(heads)[:3]))
        # 13: ticks must exist AFTER the pause window ended.
        after = [t for t in d if t.head not in heads]
        resumed = len(d) > 0 and (d[-1].tick > 0)
        rep.add(13, "resume continues the game",
                "PASS" if resumed and after else "FAIL",
                "%d ticks total, %d of them past the paused cell"
                % (len(d), len(after)))

    # ---- Run E: restart resets everything -------------------------------
    frames_e = (hold((), 30) + hold((K_ENTER,), 6) + hold((), 520)
                + hold((K_ENTER,), 6) + hold((), 120))
    e, log_e, rc_e = run("e", frames_e, max_frames=800)
    if not e:
        rep.add(14, "restart resets the game", "VOID", "no trace lines")
    else:
        restarts = [i for i in range(1, len(e))
                    if e[i].score < e[i - 1].score or e[i].tick < e[i - 1].tick]
        if not restarts:
            rep.add(14, "restart resets the game", "VOID",
                    "the run never reached a second game")
        else:
            i = restarts[0]
            ok = e[i].score == 0 and e[i].length == INITIAL_LEN
            rep.add(14, "restart resets the game", "PASS" if ok else "FAIL",
                    "after restart: score=%d len=%d (want 0 / %d)"
                    % (e[i].score, e[i].length, INITIAL_LEN))

    # ---- 15 is checked right after run C, above, because run() clears the
    # best-score file before every launch.  Checking it here, after five more
    # runs, read the file as absent and reported VOID -- a harness ordering
    # artefact that looked exactly like "the game never records a best score".

    # ---- 16: speed is frame-rate independent ----------------------------
    # Same input, two timesteps, and a frame budget SHORT ENOUGH THAT
    # NEITHER RUN DIES.  The first version ran both to the wall, where the
    # snake stops after exactly 15 ticks regardless of dt -- so both runs
    # reported 15 and the ratio was 1.00, which read as a failure of a
    # property that actually holds.  A quantity clamped by something other
    # than the thing under test measures that other thing.
    #
    # 120 frames is 2.0 s at 1/60 (~11 ticks) and 1.0 s at 1/120 (~5), both
    # well short of the ~15 ticks it takes to reach the wall.
    warm = hold((), 30) + hold((K_ENTER,), 6) + hold((), 200)
    slow, _, _ = run("f60", warm, max_frames=120,
                     extra_env={"JCE_FRAME_DT_FIXED": "0.016666"})
    fast, _, _ = run("f120", warm, max_frames=120,
                     extra_env={"JCE_FRAME_DT_FIXED": "0.008333"})
    if not slow or not fast:
        rep.add(16, "speed does not follow frame rate", "VOID",
                "one of the two timestep runs produced no ticks (%d / %d)"
                % (len(slow), len(fast)))
    elif max(t.state for t in slow) == 3 or max(t.state for t in fast) == 3:
        rep.add(16, "speed does not follow frame rate", "VOID",
                "a run reached GAMEOVER inside the window, so the tick "
                "count is bounded by the wall and not by the clock")
    else:
        ratio = len(slow) / max(len(fast), 1)
        ok = 1.6 <= ratio <= 2.4
        rep.add(16, "speed does not follow frame rate",
                "PASS" if ok else "FAIL",
                "%d ticks at dt=1/60 vs %d at dt=1/120 over the same 120 "
                "frames (ratio %.2f, want ~2.0)"
                % (len(slow), len(fast), ratio))

    # ---- 11: self-collision ---------------------------------------------
    # A snake of length 4 (head + 3) CANNOT hit itself: the tightest loop
    # that returns to an occupied cell is four moves, and four moves back is
    # one cell further than the tail.  So this needs the snake to eat first,
    # which is why it runs after run C and reuses its route.
    #
    # After eating at (23,18) heading DOWN with length 4, the body occupies
    # the four cells above the head.  Turning left, up, then right walks the
    # head back onto the third of them.
    if eat_frames and frame_of_tick:
        eat_tick = None
        for i in range(1, len(c)):
            if c[i].score > c[i - 1].score:
                eat_tick = c[i].tick
                break
        if eat_tick is None:
            rep.add(11, "self-collision ends the game", "VOID",
                    "the route did not reach the food, so the snake never "
                    "grew past length 3 and cannot reach itself")
        else:
            fx, fz = food0
            turns = [(t, K_DOWN) for t in range((fx - 15) + 1, eat_tick + 1)]
            turns += [(eat_tick + 1, K_LEFT),
                      (eat_tick + 2, K_UP),
                      (eat_tick + 3, K_RIGHT)]
            sc_frames = schedule(frame_of_tick, turns, eat_tick + 6)
            sc, log_s, _ = run("s", sc_frames, max_frames=len(sc_frames) + 40)
            hit = [t for t in sc if t.self_hit]
            isc = [i for i in idles(log_s) if i.self_hit]
            if hit or isc:
                where = hit[0].head if hit else isc[0].head
                rep.add(11, "self-collision ends the game", "PASS",
                        "self verdict raised with the head at %s" % (where,))
            else:
                reached = max((t.length for t in sc), default=0)
                rep.add(11, "self-collision ends the game", "VOID",
                        "the scripted loop did not close on the body in %d "
                        "ticks (length reached %d)" % (len(sc), reached))
    else:
        rep.add(11, "self-collision ends the game", "VOID",
                "no calibrated route, so the snake could not be fed first")

    # ---- 17: no unbounded growth over a long run ------------------------
    # Actually sampled, not asserted.  The previous version of this check
    # printed "this harness does not sample RSS" and returned VOID, which
    # was at least honest; a version that returned PASS without sampling
    # would have been a criterion reported as met on no evidence at all.
    #
    # The shape being looked for is a TREND, not a peak: a game that loads
    # its assets and settles is normal, and one whose working set climbs
    # steadily for the whole run is not.  So the first samples are discarded
    # (startup) and the remainder is compared head to tail.
    samples = sample_working_set("mem", frames_a, max_frames=2400)
    if len(samples) < 6:
        rep.add(17, "no runaway memory over a long run", "VOID",
                "only %d usable samples; the process exited too quickly to "
                "see a trend" % len(samples))
    else:
        settled = samples[len(samples) // 3:]
        first = sum(settled[:2]) / 2.0
        last = sum(settled[-2:]) / 2.0
        growth = (last - first) / max(first, 1.0)
        rep.add(17, "no runaway memory over a long run",
                "PASS" if growth < 0.10 else "FAIL",
                "%d samples over the run, settled RSS %.1f MB -> %.1f MB "
                "(%+.1f%%)" % (len(samples), first / 1048576.0,
                               last / 1048576.0, growth * 100.0))

    print()
    return rep.summary()


if __name__ == "__main__":
    sys.exit(main())
