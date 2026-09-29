#!/usr/bin/env python3
"""Does the editor notice a scene file somebody else wrote?  (REQ-SCN-04)

An agent working through the Automation API writes a scene into the project
inside a changeset and then asks a human to look at it before that changeset
is committed.  Until the scene-file watcher existed, the editor read the file
once -- at open -- and never looked again, so the human previewed whatever was
loaded earlier and approved it as though it were the new one.

TWO RUNS OF ONE BINARY AGAINST ONE SCENE.  The only difference is whether
somebody else writes the file while the editor is up.  THE SECOND RUN IS THE
ONE THAT MAKES THE FIRST MEAN ANYTHING: "the log says reloading" is equally
satisfied by a watcher that reloads on a timer, on startup, or unconditionally,
so the same run is made with the file left alone and the line must be absent.

    python tools/probe_scene_watch.py <editor.exe> <source.scene.json> [output_dir]

THREE WAYS THIS PROBE WAS WRONG BEFORE IT WAS RIGHT, each left in the code as
a guard rather than as a memory:

  1. It set JCE_STARTUP_SCENE.  The ENGINE reads that; the EDITOR reads
     JCE_SCENE and ignores the other entirely.  The run produced a complete,
     plausible log of the editor opening its own last scene, the watcher said
     nothing, and the watcher looked broken.  An environment variable nobody
     reads leaves no trace at all -- hence the opened-the-scene check, which
     REFUSES to score a run that was not about the subject.

  2. It read the child's stdout from a PIPE only after the child exited.  The
     editor writes more than a pipe holds, so it blocked on write while the
     parent blocked waiting for it to exit.  Measured: alive 4m20s having used
     1.7 SECONDS of CPU.  Wall clock cannot tell a slow run from a stuck one;
     CPU time can.  Stdout goes to a file here.

  3. A run killed between the edit and the restore left the scene file
     modified, so the NEXT run read the edited file as its baseline, made an
     edit that changed nothing, and reported the watcher silent.  The baseline
     is asserted pristine at startup, and the assertion names the command that
     repairs it.
"""
from __future__ import annotations

import io
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from jce_determinism import DETERMINISM                      # noqa: E402

MARKER = b'"Sun"'


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    exe = Path(argv[1])
    if not exe.is_file():
        print("no such executable: %s" % exe)
        return 2
    source_scene = Path(argv[2]).resolve()
    if not source_scene.is_file():
        print("no such source scene: %s" % source_scene)
        return 2
    work = Path(argv[3]) if len(argv) > 3 else (
        ROOT / "build" / "automation" / "scene-watch-probe")
    work.mkdir(parents=True, exist_ok=True)
    scene = work / "w.scene.json"
    scene.write_bytes(source_scene.read_bytes())
    original = scene.read_bytes()
    if MARKER not in original:
        print("the baseline scene is not pristine; re-copy it from %s"
              % source_scene.as_posix())
        return 2

    rows = []
    for tag, touch_at in (("touched", 10.0), ("untouched", None)):
        print("== run: %s ==" % tag)
        log = work / ("watch-%s.log" % tag)
        out = work / ("watch-%s.out" % tag)
        if log.exists():
            log.unlink()
        env = dict(os.environ)
        env.update(DETERMINISM)
        env.update({
            "JCE_MAX_FRAMES": "1500",
            "JCE_SCENE": str(scene),
            "JCE_LOG_FILE": str(log),
        })
        t0 = time.time()
        with io.open(str(out), "wb") as so:
            p = subprocess.Popen([str(exe)], cwd=str(exe.parent), env=env,
                                 stdout=so, stderr=subprocess.STDOUT)
            wrote = False
            while p.poll() is None:
                if touch_at is not None and not wrote and time.time() - t0 > touch_at:
                    body = original.replace(MARKER, b'"Sun_edited_by_someone_else"')
                    if body == original:
                        print("  the edit changed nothing; aborting")
                        p.terminate()
                        return 2
                    scene.write_bytes(body)
                    wrote = True
                    print("  rewrote the scene at t+%.1fs" % (time.time() - t0))
                time.sleep(0.2)
        scene.write_bytes(original)
        text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
        text += io.open(str(out), "rb").read().decode("utf-8", "replace")

        opened = scene.name in text
        print("  opened the scene under test : %s" % opened)
        if not opened:
            print("  REFUSED: the editor did not open this probe's scene, so "
                  "nothing measured here is about the watcher.")
            return 1
        row = {
            "run": tag,
            "rewritten_by_someone_else": wrote,
            "saw_changed_on_disk": "scene file changed on disk" in text,
            "saw_reload": "reloading:" in text,
            "saw_withheld": "reload withheld" in text,
            "exit_code": p.returncode,
        }
        for k in ("exit_code", "rewritten_by_someone_else",
                  "saw_changed_on_disk", "saw_reload", "saw_withheld"):
            print("  %-27s: %s" % (k, row[k]))
        rows.append(row)

    a, b = rows
    ok = (a["saw_changed_on_disk"] and a["saw_reload"]
          and not b["saw_changed_on_disk"] and not b["saw_reload"])
    print()
    print("VERDICT: %s" % ("the editor reloads a scene written by somebody "
                           "else, and does not otherwise"
                           if ok else
                           "NOT DEMONSTRATED -- see the two runs above"))
    # NOT MEASURED HERE, and saying so is part of the result: the branch that
    # WITHHOLDS a reload needs unsaved editor edits, which needs a person or a
    # UI driver.  Driving the editor through synthetic input is forbidden by
    # REQ-ARCH-02 and there is a gate for it, so this probe reports that half
    # as unmeasured rather than reaching for the thing the contract prohibits.
    print("NOT MEASURED: the withheld-on-unsaved-edits branch. It needs "
          "unsaved edits in a live editor; REQ-ARCH-02 forbids driving the "
          "editor with synthetic input, so this probe does not.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
