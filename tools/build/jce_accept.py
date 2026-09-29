#!/usr/bin/env python3
"""Project acceptance protocol: does the SHIPPED artifact actually work?

`jce.py package game` ends when a directory exists.  That is not acceptance --
it proves files were copied, not that the thing runs.  Every failure mode this
repo has recorded in the shipping path survives a green package:

  - the exe boots only where the repo sits above it (loose assets, a
    CWD-relative jce_project.json, a dev shader dir)
  - the exe boots into the DEFAULT empty scene because the manifest's
    startup_scene never reached the runtime
  - the exe renders a blank frame, which compares perfectly equal to any
    other blank frame
  - the exe prints a perf line and then dies, and the number is kept
  - bgfx falls back to another backend and nobody checks which one rendered

So this file runs the artifact, from a directory with no repository above it,
and looks at what came out.

The protocol is GENERAL.  The project is a parameter (`--project`), read from
its own `jce_project.json`; nothing here knows the name of any user project.
That is the standing rule: the general engine and editor come first, user
projects are dogfooding, and a general tool must never learn one of their names.

Stages, in order, each PASS / FAIL / SKIP with a reason:

    package      cook + build + stage        (delegated to jce.py)
    relocation   copy the bundle out of the repo and run from there
    boot         bounded run; the exit code is part of the evidence
    scene        the log names the project's startup_scene, not the default
    backend      the engine's own "renderer:" line, not the JCE_BACKEND request
    screenshot   a captured frame that is not blank
    perf         a steady-state frame time from a run that finished

Usage:

    python scripts/jce_accept.py --project examples/caged_kingdom
    python scripts/jce_accept.py --project space --bundle dist/games/... --skip-package
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))

# The determinism recipe, the backend assertion and the blank-frame guard are
# single-sourced.  A local copy here would be the fourth copy of a recipe this
# repo has already had to unify once.
from jce_determinism import (DETERMINISM, assert_backend,   # noqa: E402
                             assert_has_content, read_png, parse_perf)

PASS, FAIL, SKIP = "PASS", "FAIL", "SKIP"


class Stage:
    def __init__(self, name):
        self.name = name
        self.state = SKIP
        self.detail = "not run"
        self.seconds = 0.0

    def as_dict(self):
        return {"stage": self.name, "state": self.state,
                "detail": self.detail, "seconds": round(self.seconds, 2)}


class Report:
    def __init__(self):
        self.stages = []

    def stage(self, name):
        s = Stage(name)
        self.stages.append(s)
        return s

    @property
    def failed(self):
        return [s for s in self.stages if s.state == FAIL]

    def render(self):
        width = max(len(s.name) for s in self.stages)
        lines = []
        for s in self.stages:
            lines.append("  %-*s  %-4s  %6.1fs  %s"
                         % (width, s.name, s.state, s.seconds, s.detail))
        return "\n".join(lines)


def run(cmd, cwd=None, env=None, timeout=1800):
    return subprocess.run([str(c) for c in cmd], cwd=str(cwd) if cwd else None,
                          env=env, capture_output=True, text=True,
                          timeout=timeout, errors="replace")


def read_manifest(project: Path) -> dict:
    p = project / "jce_project.json"
    if not p.is_file():
        return {}
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return {}


def newest_bundle(project: Path):
    games = project / "dist" / "games"
    if not games.is_dir():
        return None
    dirs = [d for d in games.iterdir() if d.is_dir()]
    if not dirs:
        return None
    return max(dirs, key=lambda d: d.stat().st_mtime)


def find_exe(bundle: Path):
    exes = sorted(bundle.glob("*.exe")) if os.name == "nt" else \
        [p for p in sorted(bundle.iterdir())
         if p.is_file() and os.access(p, os.X_OK) and p.suffix == ""]
    return exes[0] if exes else None


# ── stages ───────────────────────────────────────────────────────────────

def stage_package(rep, project, arch, variant, skip):
    st = rep.stage("package")
    if skip:
        st.detail = "--skip-package: using an existing bundle"
        return None
    t0 = time.time()
    # `project` is POSITIONAL on `jce.py package game`; passing it as
    # --project made argparse reject the whole invocation.
    cmd = [sys.executable, str(REPO_ROOT / "scripts" / "jce.py"), "package",
           "game", str(project), "--variant", variant]
    if arch:
        cmd += ["--arch", arch]
    r = run(cmd, cwd=REPO_ROOT)
    st.seconds = time.time() - t0
    if r.returncode != 0:
        st.state = FAIL
        st.detail = ("jce.py package game exited %d: %s"
                     % (r.returncode,
                        (r.stderr or r.stdout or "").strip()[-300:]))
        return None
    st.state = PASS
    st.detail = "cook + build + stage"
    return None


def stage_relocation(rep, bundle, keep):
    """Copy the bundle somewhere with no repository above it, and run there.

    This is the stage that catches a bundle which only works because the repo
    happens to sit above it -- loose assets, a CWD-relative manifest, a dev
    shader directory.  Under the repo those all resolve; one directory move
    away they do not.
    """
    st = rep.stage("relocation")
    t0 = time.time()
    if bundle is None or not bundle.is_dir():
        st.state = FAIL
        st.detail = "no bundle to relocate"
        return None
    root = Path(tempfile.mkdtemp(prefix="jce_accept_"))
    dest = root / bundle.name
    try:
        shutil.copytree(bundle, dest)
    except Exception as exc:
        st.state = FAIL
        st.detail = "copy failed: %s" % exc
        return None
    st.seconds = time.time() - t0
    # Prove the claim rather than assert it: walk up and confirm no .git.
    above = [p for p in [dest] + list(dest.parents) if (p / ".git").exists()]
    if above:
        st.state = FAIL
        st.detail = "relocated into a tree that still has a repo above it: %s" % above[0]
        return dest
    st.state = PASS
    st.detail = "%s (no .git in any parent%s)" % (
        dest, "; kept" if keep else "; removed at exit")
    return dest


def _launch(exe, workdir, frames, backend, shot, log_path, extra=None):
    env = dict(os.environ)
    env.update(DETERMINISM)
    env.update({
        "JCE_MAX_FRAMES": str(frames),
        "JCE_PERF_LOG": "1",
        "JCE_LOG_FILE": str(log_path),
        "JCE_WINDOW_HIDDEN": "1",
    })
    if backend:
        env["JCE_BACKEND"] = backend
    if shot:
        # Capture late enough that streaming and TAA have settled; a frame-0
        # capture is a photograph of the loading screen.
        env["JCE_CAPTURE_FRAME"] = str(max(1, frames - 2))
        env["JCE_CAPTURE_PATH"] = str(shot)
    if extra:
        env.update(extra)
    # The repo must not be reachable through the environment either, or the
    # relocation stage proves nothing.
    for leak in ("JCE_SDK_DIR", "JCE_SHADER_DEV_DIR", "JCE_DEV_ASSETS",
                 "JCE_INCLUDE_DIR", "JCE_TEMPLATES_DIR"):
        env.pop(leak, None)
    return run([exe], cwd=workdir, env=env, timeout=600)


def stage_boot(rep, dest, frames, backend):
    st = rep.stage("boot")
    if dest is None:
        st.state = FAIL
        st.detail = "no relocated bundle"
        return None, None, None
    exe = find_exe(dest)
    if exe is None:
        st.state = FAIL
        st.detail = "no executable in %s" % dest
        return None, None, None
    log_path = dest / "accept.log"
    shot = dest / "accept.png"
    t0 = time.time()
    try:
        proc = _launch(exe, dest, frames, backend, shot, log_path)
    except subprocess.TimeoutExpired:
        st.seconds = time.time() - t0
        st.state = FAIL
        st.detail = ("the run did not finish within its timeout; a hang is not "
                     "a pass, and JCE_MAX_FRAMES=%d should have ended it" % frames)
        return exe, None, shot
    st.seconds = time.time() - t0
    text = ""
    if log_path.is_file():
        text = log_path.read_text(encoding="utf-8", errors="replace")
    combined = (proc.stdout or "") + "\n" + text
    if proc.returncode != 0:
        st.state = FAIL
        st.detail = ("exit %d -- output from a process that did not finish is "
                     "not evidence.  tail: %s"
                     % (proc.returncode,
                        (proc.stderr or proc.stdout or "").strip()[-260:]))
        return exe, combined, shot
    if "single-instance lock already held" in combined:
        st.state = FAIL
        st.detail = ("hit the single-instance lock and exited without "
                     "rendering; this run is NOT a measurement")
        return exe, combined, shot
    st.state = PASS
    st.detail = "%d frames, exit 0, ran from %s" % (frames, dest.name)
    return exe, combined, shot


def stage_scene(rep, combined, manifest):
    """The manifest's startup_scene must appear in the log.

    A runtime that cannot find jce_project.json boots into the default empty
    scene and reports nothing wrong.  The bundle looks fine and is empty.
    """
    st = rep.stage("scene")
    want = (manifest.get("startup_scene") or "").strip()
    if not want:
        st.detail = "the project declares no startup_scene"
        return
    if combined is None:
        st.state = FAIL
        st.detail = "no log to read"
        return
    stem = Path(want).stem
    if stem and stem.lower() in combined.lower():
        st.state = PASS
        st.detail = "log names %s" % want
    else:
        st.state = FAIL
        st.detail = ("the log never names the declared startup_scene %r, so "
                     "this bundle probably booted the default empty scene "
                     "and would still have passed packaging" % want)


def stage_backend(rep, combined, backend):
    st = rep.stage("backend")
    if combined is None:
        st.state = FAIL
        st.detail = "no output to read"
        return
    if not backend:
        st.detail = "no --backend requested, so there is nothing to assert"
        return
    try:
        got = assert_backend(combined, backend)
    except Exception as exc:
        st.state = FAIL
        st.detail = str(exc)
        return
    st.state = PASS
    st.detail = "engine reported %s" % got


def stage_screenshot(rep, shot):
    st = rep.stage("screenshot")
    if shot is None:
        st.state = FAIL
        st.detail = "no capture requested"
        return
    if not shot.is_file():
        st.state = FAIL
        st.detail = ("JCE_CAPTURE_PATH produced no file at %s; a capture that "
                     "did not happen is not a blank frame, it is no frame" % shot)
        return
    try:
        # assert_has_content takes the DECODED image, not a path.  Passing the
        # path raised "cannot unpack non-iterable WindowsPath" -- a guard that
        # throws on its own argument reports FAIL for every input, which looks
        # exactly like a real defect and is not one.
        assert_has_content(read_png(shot), shot)
    except Exception as exc:
        st.state = FAIL
        st.detail = str(exc)
        return
    st.state = PASS
    st.detail = "%s, %d bytes, not blank" % (shot.name, shot.stat().st_size)


def stage_perf(rep, combined, budget):
    st = rep.stage("perf")
    if combined is None:
        st.state = FAIL
        st.detail = "no log to read"
        return
    # One shared pattern.  The local one this used to carry matched nothing the
    # engine actually prints, so this stage failed every run that HAD a sample.
    s = parse_perf(combined)
    if s is None:
        st.state = FAIL
        st.detail = ("no steady-state perf line -- the run either never got "
                     "there or JCE_PERF_LOG was not set")
        return
    summary = ("%.2f ms / %d fps, cpu %.1f / gpu %.1f, %d draws, rss %d MB "
               "(last of %d samples)"
               % (s["frame_ms"], s["fps"], s["cpu_ms"], s["gpu_ms"],
                  s["draws"], s["rss_mb"], s["samples"]))

    # BUDGETS.  Until 2026-09-01 this stage ended at PASS unconditionally: it
    # parsed six numbers and printed them as prose, so ANY number passed and the
    # only way it could fail was the line being missing.  That is a report
    # wearing a gate's clothes -- the pattern this audit found nine times
    # elsewhere, here in the acceptance protocol itself.
    #
    # The thresholds are CEILINGS on the shipped artifact, not targets: they are
    # set far enough above what the artifact costs today that only a real
    # regression reaches them, and they are stated so that crossing one is a
    # decision somebody makes in a commit message rather than a number that
    # drifts.  rss is the 512 MB charter budget; the frame ceiling is the 30 fps
    # floor the low-end profile promises, doubled for the measuring machine.
    over = []
    if s["frame_ms"] > budget["frame_ms"]:
        over.append("frame %.2f ms > the %.2f ms ceiling (%d fps)"
                    % (s["frame_ms"], budget["frame_ms"],
                       int(1000.0 / budget["frame_ms"])))
    if s["draws"] > budget["draws"]:
        over.append("%d draws > %d ceiling -- for a fixed scene this is a "
                    "property of the renderer, not the machine"
                    % (s["draws"], budget["draws"]))
    if s["rss_mb"] > budget["rss_mb"]:
        over.append("rss %d MB > %d MB ceiling (512 MB charter)"
                    % (s["rss_mb"], budget["rss_mb"]))

    if over:
        st.state = FAIL
        st.detail = summary + " -- OVER: " + "; ".join(over)
        return
    st.state = PASS
    st.detail = summary


def stage_listener(rep, exe, dest, frames):
    """Does the shipped artifact open a listening socket nobody asked for?

    Tracy starts its server during static initialisation, so the socket exists
    before main() and for the whole run whether or not anyone profiles.  Bound
    to 0.0.0.0 that is the Windows Firewall dialog players report; bound to
    127.0.0.1 it is merely an unused listener in a shipped game.  Either way it
    is invisible to every other stage here -- a bundle with a public listener
    boots, renders, and measures exactly like one without.

    Windows only: this reads the OS socket table, so there is nothing to fall
    back on elsewhere.  Report SKIP rather than inventing a pass.
    """
    st = rep.stage("listener")
    if os.name != "nt":
        st.detail = "socket table probe is Windows-only on this host"
        return
    if exe is None or dest is None:
        st.state = FAIL
        st.detail = "no executable to observe"
        return
    ps = (
        "$p = Start-Process -FilePath '%s' -WorkingDirectory '%s' -PassThru;"
        "$seen=@{};"
        "foreach($i in 1..14){Start-Sleep -Milliseconds 600; if($p.HasExited){break};"
        "try{Get-NetTCPConnection -State Listen -ErrorAction Stop|"
        "Where-Object{$_.OwningProcess -eq $p.Id}|"
        "ForEach-Object{$seen[\"TCP $($_.LocalAddress):$($_.LocalPort)\"]=$true}}catch{};"
        "try{Get-NetUDPEndpoint -ErrorAction Stop|"
        "Where-Object{$_.OwningProcess -eq $p.Id}|"
        "ForEach-Object{$seen[\"UDP $($_.LocalAddress):$($_.LocalPort)\"]=$true}}catch{}};"
        "$seen.Keys|Sort-Object|ForEach-Object{Write-Output $_};"
        "if(-not $p.HasExited){$p.WaitForExit(120000)|Out-Null}"
    ) % (str(exe).replace("'", "''"), str(dest).replace("'", "''"))
    env = dict(os.environ)
    env.update(DETERMINISM)
    env["JCE_MAX_FRAMES"] = str(frames)
    env["JCE_WINDOW_HIDDEN"] = "1"
    t0 = time.time()
    try:
        r = run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps],
                cwd=dest, env=env, timeout=300)
    except subprocess.TimeoutExpired:
        st.state = FAIL
        st.detail = "the socket probe did not finish"
        return
    st.seconds = time.time() - t0
    socks = [ln.strip() for ln in (r.stdout or "").splitlines() if ln.strip()]
    if not socks:
        st.state = PASS
        st.detail = "no listening socket for the whole run"
        return
    public = [s for s in socks if "127.0.0.1" not in s and "::1" not in s]
    if public:
        st.state = FAIL
        st.detail = ("opens a NON-loopback listener: %s -- this is the Windows "
                     "Firewall prompt players see on a game they never asked to "
                     "profile" % ", ".join(public))
        return
    st.state = FAIL
    st.detail = ("opens an unused loopback listener: %s.  It does not prompt "
                 "the firewall, but a shipped game should not hold a socket "
                 "nobody connects to -- build the SDK with "
                 "`jce.py sdk --profiling off`" % ", ".join(socks))


# ── driver ───────────────────────────────────────────────────────────────

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--project", required=True,
                    help="project directory (relative to the repo root or "
                         "absolute).  Required: this tool knows no project by "
                         "name.")
    ap.add_argument("--bundle", default=None,
                    help="an already-staged bundle; default is the newest "
                         "under <project>/dist/games")
    ap.add_argument("--skip-package", action="store_true",
                    help="do not cook/build/stage; accept an existing bundle")
    ap.add_argument("--arch", default=None)
    ap.add_argument("--variant", default="release")
    ap.add_argument("--backend", default=None,
                    help="backend to REQUEST and then assert the engine "
                         "actually used (bgfx falls back silently)")
    ap.add_argument("--frames", type=int, default=90)
    ap.add_argument("--keep", action="store_true",
                    help="keep the relocated copy for inspection")
    ap.add_argument("--json", default=None, help="write the report as JSON")
    # Ceilings, not targets.  Overridable because a project may legitimately
    # ship a heavier scene, but NOT removable: there is no --no-budget, because
    # the whole defect being fixed here was a stage that could not fail.
    ap.add_argument("--budget-frame-ms", type=float, default=66.0,
                    help="frame-time ceiling in ms (default 66 = the 30 fps "
                         "low-end floor, doubled for the measuring machine)")
    ap.add_argument("--budget-draws", type=int, default=4000,
                    help="draw-call ceiling for the shipped scene")
    ap.add_argument("--budget-rss-mb", type=int, default=1024,
                    help="resident-set ceiling in MB (512 MB charter, doubled "
                         "for a windowed desktop run)")
    args = ap.parse_args(argv)

    project = Path(args.project)
    if not project.is_absolute():
        project = (REPO_ROOT / project).resolve()
    if not project.is_dir():
        print("jce_accept: project dir not found: %s" % project)
        return 2
    manifest = read_manifest(project)

    budget = {"frame_ms": args.budget_frame_ms,
              "draws": args.budget_draws,
              "rss_mb": args.budget_rss_mb}

    rep = Report()
    stage_package(rep, project, args.arch, args.variant, args.skip_package)

    bundle = Path(args.bundle).resolve() if args.bundle else newest_bundle(project)
    dest = stage_relocation(rep, bundle, args.keep)
    exe, combined, shot = stage_boot(rep, dest, args.frames, args.backend)
    stage_scene(rep, combined, manifest)
    stage_backend(rep, combined, args.backend)
    stage_screenshot(rep, shot)
    stage_perf(rep, combined, budget)
    stage_listener(rep, exe, dest, min(args.frames, 120))

    print("jce_accept: %s" % project.name)
    print("  bundle: %s" % (bundle if bundle else "(none)"))
    print(rep.render())
    n_pass = sum(1 for s in rep.stages if s.state == PASS)
    n_fail = len(rep.failed)
    n_skip = sum(1 for s in rep.stages if s.state == SKIP)
    # Report the three states separately.  A SKIP is not a PASS; collapsing
    # them is how a protocol reports success for work it never did.
    print("  %d PASS, %d FAIL, %d SKIP of %d stage(s)"
          % (n_pass, n_fail, n_skip, len(rep.stages)))

    if args.json:
        Path(args.json).write_text(
            json.dumps({"project": project.name,
                        "bundle": str(bundle) if bundle else None,
                        "stages": [s.as_dict() for s in rep.stages]},
                       ensure_ascii=False, indent=2),
            encoding="utf-8", newline="\n")

    if dest is not None and not args.keep:
        shutil.rmtree(dest.parent, ignore_errors=True)
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
