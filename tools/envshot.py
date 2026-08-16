#!/usr/bin/env python3
"""envshot.py -- deterministic environment capture and comparison.

WHY THIS EXISTS.  Every environment change -- shadows, fog, clouds, wind,
vegetation, terrain material -- alters how existing scenes look, and the only
way to review such a change is to put two pictures side by side under
conditions that are identical in every respect except the one under test.

Doing that by hand does not work.  A single investigation of shadow flicker
produced five conclusions that were all wrong, and each for a different
mechanical reason:

  * the running build had a stale shader PAK, so the measurement described
    code that was no longer in the tree;
  * the camera was pointed at a hillside while the question was about the
    forest floor;
  * one capture came from the ImGui-FBO path and the other from the backbuffer
    screenshot path -- two different images by construction;
  * a spin capture fired on step 39 while the static reference was posed at
    step 40, so a 1.5 degree camera mismatch was read as flicker;
  * a debug probe that recoloured only TERRAIN fragments was summarised over a
    region that also contained sky, water and meshes, so most of the histogram
    described pixels the probe never touched.

None of those is a lapse of care; they are the failure modes of ad-hoc
measurement.  Each is eliminated here by construction rather than by
remembering.

WHAT IT GUARANTEES.
  * the shader PAK is rebuilt before capture, so the picture matches the tree;
  * one capture path for every shot;
  * the camera is stated as data and recorded in the manifest beside the image;
  * the compared REGION is named and stored, so two runs cannot summarise
    different pixels;
  * a run refuses to compare images whose manifests disagree on anything but
    the variable under test.

Usage:
    python tools/envshot.py capture  --name base
    python tools/envshot.py capture  --name fix --set ambient.intensity=0.2
    python tools/envshot.py compare  base fix
    python tools/envshot.py list
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("JCE_ENVSHOT_BUILD",
                            ROOT / "build/desktop/windows-x64"))
EXE = BUILD / "release/jce_editor.exe"

# JCE_ENVSHOT_BUILD points the harness at a DIFFERENT build of the editor while
# still reading scenes and assets from this tree.
#
# Needed because a concurrent session editing unrelated engine source can leave
# the shared tree in a state that does not compile -- it did, for half an hour,
# in the middle of a measurement, over an input-binding API restructure with
# nothing to do with rendering. Building the same commit in a clean `git
# worktree` sidesteps that without touching their work, and JCE_SCENE is an
# absolute path, so the other build renders THIS tree's scene and assets.
OUT = ROOT / "build/envshots"

# ── The fixed conditions ─────────────────────────────────────────────
#
# Stated as data, written into every manifest, and compared before any two
# images are allowed to be diffed.  A camera that lives in a shell history is
# a camera nobody can reproduce.

VIEWS = {
    # Looking down at the forest floor: where a cast shadow is unmistakable.
    "forest": dict(tx=-60, ty=30, tz=-60, dist=55, pitch=42, yaw=140),
    # Across the bay toward the far ridge: aerial perspective and water.
    "bay":    dict(tx=0,   ty=16, tz=0,   dist=70, pitch=4,  yaw=25),
    # Up at the sky: clouds and atmosphere.
    "sky":    dict(tx=0,   ty=40, tz=0,   dist=45, pitch=-22, yaw=25),
    # Same target/distance as "forest" but near-horizontal, for isolating
    # whether a behaviour depends on pitch.
    # Dense canopy from above -- the view the user's recording flickered in,
    # and the only one that actually exercises foliage. "forest" is mostly
    # shoreline and water, which is why a foliage change measured as nothing there.
    "canopy": dict(tx=-70, ty=42, tz=-40, dist=48, pitch=38, yaw=205),
    # Across the bay to the far ridge, near-horizontal: maximum distant ground
    # in frame, which is where cascades 2 and 3 are.
    "vista":    dict(tx=0, ty=22, tz=-20, dist=85, pitch=6, yaw=170),
    # The user's screenshot framing: a wooded slope seen from above at a
    # shallow angle, where a distance-banded material change is obvious.
    "beauty":   dict(tx=-65.7, ty=37.9, tz=45.6, dist=60, pitch=11.5, yaw=149.0),
    "slope":    dict(tx=-40, ty=55, tz=40, dist=75, pitch=32, yaw=200),
    "forestflat": dict(tx=-60, ty=30, tz=-60, dist=55, pitch=8, yaw=140),
    # Elemental Serenity's own BeautyCam, converted to the orbit rig: the
    # scene is a small diorama at the origin, and every hidden_cove view sits
    # tens of metres outside it looking at empty grid -- which reads exactly
    # like "the models are gone".
    "es":         dict(tx=0, ty=0, tz=0, dist=34.4, pitch=18.1, yaw=33.7),
}

# Named crop regions, as fractions of the frame.  The scene viewport only --
# never the panels, and never the profiler graphs, which change every frame by
# design and would drown any real signal.
REGIONS = {
    "viewport": (0.20, 0.06, 0.70, 0.62),
    "ground":   (0.22, 0.30, 0.68, 0.58),
    "skyband":  (0.22, 0.08, 0.68, 0.30),
    # The DISTANT ground: the band just under the horizon, where cascades 2-3
    # land. Flicker reported "at a distance" cannot be measured in a close-up
    # crop, and the near ground drowns it in a whole-viewport one.
    "far":      (0.22, 0.26, 0.68, 0.40),
}

# The tier's HIGH preset, written in full.
#
# The .rp.json parser starts from a zeroed desc, so a file naming ONE key turns
# everything else OFF -- an override of enable_taa alone was observed to produce
# csm=0 shadow=512 cascades=1 post=low. Any pipeline override must therefore
# restate the whole preset, which is why this lives here rather than in a shell
# line somebody writes from memory.
RP_HIGH = {
    "enable_csm": True, "csm_cascade_count": 4, "shadow_resolution": 2048,
    "shadow_filter_quality": 2, "enable_ssao": True, "enable_ssr": True,
    "enable_bloom": True, "enable_volumetric_fog": True,
    "enable_gpu_particles": True, "enable_cloth": True,
    "enable_stylized_sky": False, "enable_motion_blur": False,
    "hdr_color": True, "depth_prepass": True, "msaa_samples": 2,
    "render_scale": 1.0, "enable_taa": True,
}

DEFAULT_SCENE = "caged_kingdom/resources/assets/scenes/hidden_cove.scene.json"
CAPTURE_FRAME = 300


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=isinstance(cmd, str), cwd=str(ROOT),
                          capture_output=True, text=True, **kw)


def build_shaders():
    """Rebuild before every capture.

    Not an optimisation to skip: a stale shader PAK is invisible -- the editor
    starts, the scene renders, the numbers look plausible -- and it silently
    describes code that is no longer in the tree.
    """
    # Build the EDITOR target, not everything.
    #
    # `cmake --build BUILD` with no target also builds the 300-odd test
    # executables, so any unrelated broken test blocks every capture -- which
    # it did twice in one session, once from a stale ninja rule and once from
    # another session's half-landed header.  Neither had anything to do with
    # what was being measured, and neither should be able to stop a
    # measurement.  jce_editor.exe still pulls the engine and the shader PAK,
    # so the guarantee this function exists for is unchanged.
    # JCE_ENVSHOT_NO_BUILD=1 skips the rebuild, and every capture taken that
    # way says so in its manifest.
    #
    # This exists only for the case where the shared tree does not compile for
    # reasons unrelated to the measurement -- a concurrent session mid-way
    # through an API change in another subsystem. Skipping silently would give
    # back exactly the failure this function was written to prevent, so the
    # escape is recorded rather than merely permitted: `no_build` in the
    # manifest marks the picture as one whose provenance was not checked.
    if os.environ.get("JCE_ENVSHOT_NO_BUILD", "") not in ("", "0"):
        print("envshot: SKIPPING the rebuild (JCE_ENVSHOT_NO_BUILD) -- "
              "captures will be marked no_build in their manifest")
        return
    r = sh(["cmake", "--build", str(BUILD), "--target", "jce_editor.exe"])
    if r.returncode != 0:
        tail = "\n".join((r.stdout + r.stderr).splitlines()[-15:])
        sys.exit(f"build failed:\n{tail}")


def apply_overrides(scene_path, overrides, tod_run=False):
    """Apply dotted-path overrides to a copy of the scene; return the copy."""
    d = json.loads(Path(scene_path).read_text(encoding="utf-8"))
    for kv in overrides:
        key, _, val = kv.partition("=")
        try:
            v = json.loads(val)
        except json.JSONDecodeError:
            v = val

        # "Name.field=V" targets a COMPONENT FIELD on the entity called Name;
        # anything else targets scene.rendering. Component fields are where
        # terrain tiling, water and light parameters actually live, and a tool
        # that could only reach rendering settings could not ablate any of them.
        ent_name, _, field = key.partition(".")
        hit = False
        if field:
            for e in d["scene"]["entities"]:
                if e.get("name") == ent_name:
                    for c in e.get("components", []):
                        if field in c:
                            c[field] = v
                            hit = True
            if hit:
                continue
        if "." in key and not hit and ent_name not in d["scene"]["rendering"]:
            # Named an entity that does not exist, or a field it does not have.
            # Silently writing it into rendering instead would be a no-op that
            # looks like a measurement.
            names = sorted({e.get("name", "") for e in d["scene"]["entities"]})
            if ent_name in names:
                sys.exit(f"entity {ent_name!r} has no field {field!r}")

        node = d["scene"]["rendering"]
        parts = key.split(".")
        # Refuse to CREATE a rendering key that does not already exist.
        #
        # The scene writer emits every rendering field, so a key that is
        # missing is a key nothing reads -- and writing it produces a capture
        # that differs from the baseline in nothing at all. That is the same
        # silent no-op as an override that never reaches the viewport, and it
        # has now happened twice here: `--rp enable_csm=false` moved the editor
        # UI by 5.55 grey levels and the viewport by 0.0007, and
        # `--set postfx.tonemapOp=N` created rendering.postfx.tonemapOp while
        # the field is read from rendering.look. Both times the tell was three
        # different overrides producing byte-identical numbers.
        for pp in parts[:-1]:
            if pp not in node or not isinstance(node[pp], dict):
                sys.exit(f"no rendering group {pp!r} in {'.'.join(parts)} "
                         f"-- have: {sorted(k for k, vv in node.items() if isinstance(vv, dict))}")
            node = node[pp]
        if parts[-1] not in node:
            sys.exit(f"rendering key {key!r} does not exist -- writing it would "
                     f"change nothing and read as a measurement. "
                     f"{'.'.join(parts[:-1]) or 'rendering'} has: "
                     f"{sorted(node)}")
        node[parts[-1]] = v

    # PIN THE DAY CLOCK.
    #
    # tod_speed is documented as "hours advanced per real second", and the
    # renderer advances an internal clock by it every frame.  A capture is
    # taken at a FRAME NUMBER, so with the clock running, the hour that frame
    # lands on is a function of how long startup took -- not of the authored
    # hour, and not the same twice.
    #
    # At the default speed of 1.0 a shot on frame 200 is already three or more
    # hours past what the scene says, which is enough to cross dusk.  Two
    # captures authored at 17:30 and 17:54 came out byte-identical because the
    # clock had carried both into night, and the conclusion drawn from them --
    # "the sky never goes warm at a low sun" -- was entirely an artefact of
    # that.  Pinning the clock and re-shooting 17:36 produced blue-minus-red
    # +16.2 at the horizon: a sunset, from the same build.
    #
    # This is the same rule as the spin-pair refusal below: a measurement whose
    # independent variable is set by startup timing is not a measurement.
    env_node = d["scene"]["rendering"].get("environment")
    tod = env_node.get("timeOfDay") if isinstance(env_node, dict) else None
    if isinstance(tod, dict) and tod.get("enabled") and not tod_run:
        spd = float(tod.get("speed", 0.0) or 0.0)
        if spd > 0.0:
            print("envshot: pinning timeOfDay.speed to 0 (was %.3f). The clock "
                  "advances %.3f hours per REAL second, so the hour a fixed "
                  "frame lands on depends on startup timing. --tod-run keeps "
                  "it running." % (spd, spd))
            tod["speed"] = 0.0

    # Beside the ORIGINAL scene, never in the output directory.
    #
    # A scene names its terrain, textures and models with paths relative to
    # itself. Writing the override copy somewhere else silently breaks every
    # one of them, and the result is not an error -- it is a scene that loads,
    # renders, and looks plausibly different. That cost this harness its first
    # two comparisons: overriding shadows.distance and overriding an unrelated
    # key produced the SAME bright, flat image, because neither override was
    # what changed the picture. The give-away was two unrelated settings
    # agreeing to within half a grey level.
    src = Path(scene_path)
    tmp = src.with_name(f"_envshot_override{src.suffix}")
    # bytes, not write_text: write_text would use the platform newline.
    tmp.write_bytes(json.dumps(d, ensure_ascii=False, indent=1).encode("utf-8"))
    return tmp


def capture(args):
    OUT.mkdir(parents=True, exist_ok=True)
    if not EXE.exists():
        sys.exit(f"editor not built: {EXE}")

    build_shaders()

    scene = ROOT / args.scene
    # apply_overrides also pins a running day/night clock, so it has to run
    # whenever the scene HAS one -- not only when --set was passed.
    _sd = json.loads(Path(scene).read_text(encoding="utf-8"))
    _env = _sd.get("scene", {}).get("rendering", {}).get("environment") or {}
    _tod = _env.get("timeOfDay") or {}
    _needs_pin = bool(_tod.get("enabled")) and float(_tod.get("speed", 0) or 0) > 0
    if args.set or (_needs_pin and not args.tod_run):
        scene = apply_overrides(scene, args.set or [], tod_run=args.tod_run)

    # BOOT-INERT RENDER-PIPELINE KEYS.
    #
    # Four RP fields are read by a CHANGE-DRIVEN block in the scene renderer
    # (jce_scene_renderer.c, "Settings S1"): it only acts when the value
    # DIFFERS from the previous frame's, and it is gated on rp_seen so the
    # first frame never fires. Its comment says so on purpose -- "Boot values
    # never trigger anything => untouched defaults stay byte-identical."
    #
    # Every value envshot writes is a boot value. So `--rp enable_taa=false`
    # is a silent no-op, and it produced a full table of residuals that read
    # `notaa -0.0%` -- a number indistinguishable from "TAA does not matter
    # here", which is the opposite of the truth (forcing the cvar off raised
    # the frame-to-frame residual by 19%).
    #
    # Refuse rather than warn: a warning scrolls past and the numbers do not.
    _BOOT_INERT = {
        "enable_taa": "--cvar r.taa=0|1",
        "shadow_resolution": "no boot override exists; the panel latches it "
                             "only on a runtime change",
        "csm_cascade_count": "no boot override exists; same latch",
        "msaa_samples": "no boot override exists; same latch -- and even if "
                        "there were, the editor viewport and Game View render "
                        "into an offscreen target that carries no "
                        "BGFX_TEXTURE_RT_MSAA_Xn at all "
                        "(jce_offscreen_target.c says so): the MSAA setting "
                        "configures the BACKBUFFER, which nothing you capture "
                        "here draws into",
        # Measured, not assumed: render_scale 1.0 vs 2.0 at a fixed pose gave
        # leaf 8.999 vs 8.997, trunk 14.017 vs 14.016, ground 3.805 vs 3.805 --
        # byte-identical. Same reason as msaa_samples: it does not reach the
        # offscreen target these captures come from.
        "render_scale": "inert for the offscreen target these captures render "
                        "into (measured byte-identical at 1.0 vs 2.0)",
    }
    for kv in (args.rp or []):
        k = kv.partition("=")[0]
        if k in _BOOT_INERT:
            sys.exit("envshot: --rp %s is BOOT-INERT -- the renderer only "
                     "reacts to a RUNTIME change of it, so this override "
                     "would silently do nothing and still produce numbers. "
                     "Use: %s" % (k, _BOOT_INERT[k]))

    rp_path = EXE.parent / "Settings" / "RenderPipeline.rp.json"
    rp_force = ""
    if args.rp:
        desc = dict(RP_HIGH)
        for kv in args.rp:
            k, _, v = kv.partition("=")
            try:
                desc[k] = json.loads(v)
            except json.JSONDecodeError:
                desc[k] = v
        rp_path.parent.mkdir(parents=True, exist_ok=True)
        rp_path.write_bytes(json.dumps(desc, indent=2).encode("utf-8"))

        # AND force the same flags through JCE_RP_FORCE, because the preset
        # ALONE does not reach the editor's viewport pipeline.
        #
        # This is not belt-and-braces. Measured on this scene, `--rp
        # enable_csm=false` moved 144 pixels out of 1.07 million with a peak of
        # 3 grey levels: shadows stayed on and the capture said so only if you
        # looked. Every ablation taken with --rp before this line existed was
        # measuring the preset file, not the renderer -- including a "positive
        # control" in this session that reported a real change and was reading
        # nothing but noise. An ablation switch that silently does not ablate is
        # worse than none, because it converts "the feature does nothing" into a
        # result instead of a question.
        #
        # The preset is still written: it is what the UI reads, and leaving the
        # two disagreeing would be its own trap. JCE_RP_FORCE announces itself in
        # the log and outranks every other source, which is what makes the pair
        # honest rather than merely redundant.
        pairs = []
        for kv in args.rp:
            k, _, v = kv.partition("=")
            # The engine's forcer takes 0/1; the preset takes JSON booleans.
            on = 0 if str(v).strip().lower() in ("0", "false", "off", "no") else 1
            # enable_csm -> csm: the preset key and the feature name differ.
            pairs.append("%s=%d" % (k[len("enable_"):] if k.startswith("enable_") else k, on))
        rp_force = ",".join(pairs)

    # A pure TRANSLATION of the orbit rig: target and camera move together, so
    # the view direction is unchanged and only position varies. Rotation and
    # translation stress a cascade fit differently -- rotation moves the slice
    # centroid around the camera, translation drags the whole slice through the
    # world -- and only the first had ever been tested here.
    view = dict(VIEWS[args.view])
    view["tx"] += args.dx
    view["tz"] += args.dz
    # Orbit distance override. Added because proving a defect that lives on a
    # 1.8 m object needs the object to be more than twenty pixels tall: at the
    # bay view's 70 m a character's shadow is smaller than the capture-to-
    # capture noise floor, so "no measurable change" says nothing about the
    # shadow and everything about the framing.
    if args.dist is not None:
        view["dist"] = args.dist
    # Full orbit target, so a camera BOOKMARK can be reproduced exactly.
    # --dx/--dz can only shift tx/tz; a bookmark carries ty as well, and an
    # eye height that is off by tens of metres is a different camera.
    if args.target:
        tx, ty, tz = (float(v) for v in args.target.split(","))
        view["tx"], view["ty"], view["tz"] = tx, ty, tz
    # Absolute angle overrides, on top of the named view's target and distance.
    #
    # A named view is a whole framing chosen for one question, which is the
    # right unit when the question is "does this shader change these pixels".
    # It is the wrong unit for "does anything break AT SOME ANGLE", because
    # that needs a SWEEP -- and adding eight more named views to sweep with
    # would leave eight more framings for the next reader to interpret.
    # Overriding the two angles keeps the sweep anchored to a known target and
    # distance, so every frame in it differs from every other in exactly one
    # or two numbers.
    if args.pitch is not None: view["pitch"] = args.pitch
    if args.yaw   is not None: view["yaw"]   = args.yaw
    png = OUT / f"{args.name}.png"
    if png.exists():
        png.unlink()

    env = dict(os.environ)
    env.update({
        "JCE_BACKEND": args.backend,
        "JCE_SCENE": str(scene),
        "JCE_DBG_VISTA": "1",
        "JCE_DBG_VISTA_TX": str(view["tx"]), "JCE_DBG_VISTA_TY": str(view["ty"]),
        "JCE_DBG_VISTA_TZ": str(view["tz"]), "JCE_DBG_VISTA_DIST": str(view["dist"]),
        "JCE_DBG_VISTA_PITCH": str(view["pitch"]), "JCE_DBG_VISTA_YAW": str(view["yaw"]),
        # ONE capture path, always.  JCE_SHOT_* is the backbuffer screenshot --
        # what F12 produces.  JCE_WINCAP_* is the ImGui-FBO path and yields a
        # different image; mixing the two makes any diff meaningless.
        "JCE_SHOT_FRAME": (f"{args.frame},{args.stride}"
                           if args.stride else str(args.frame)),
        "JCE_SHOT_PATH": str(png),
        # WHICH PANEL gets captured must not be inherited state.
        #
        # The editor restores the selected dock tab from imgui.ini, so without
        # this the subject of the capture is whatever tab the LAST run left
        # selected -- and one --play run leaves the Game View selected for
        # every plain run after it. That happened: three backend captures taken
        # after a --play session all photographed the Game View, and the
        # numbers (Vulkan and GL agreeing with each other to 0.36 grey levels
        # while both differed from D3D11 by 56) read exactly like a backend
        # defect. They were a tab.
        #
        # Same rule as the day-clock pin above: a measurement whose subject is
        # decided by leftover state is not a measurement.
        # ...but a caller who names the panel explicitly via --env still wins:
        # setting both leaves FOCUS_SCENE in charge (the layout checks it
        # first), which silently ignored --env JCE_DBG_FOCUS_GAME=1 and cost a
        # run before it was noticed.
        **({"JCE_DBG_AUTOPLAY": "60", "JCE_DBG_FOCUS_GAME": "1",
            "JCE_DBG_AUTOWALK": args.play}
           if args.play is not None
           else ({} if any(e.startswith("JCE_DBG_FOCUS_")
                           for e in (args.env or []))
                 else {"JCE_DBG_FOCUS_SCENE": "1"})),
    })
    if rp_force:
        env["JCE_RP_FORCE"] = rp_force
    if args.spin:
        env["JCE_DBG_VISTA_SPIN"] = str(args.spin)
    for kv in (args.env or []):
        k, _, v = kv.partition("=")
        env[k] = v
    if args.cvar:
        # Pinned at registration and refused to every later write -- unlike the
        # settings layers, which each re-apply their own value and silently
        # undo an override.
        env["JCE_CVAR"] = ";".join(args.cvar)

    # --log: keep the editor's own stdout.
    #
    # Discarding it was fine until a measurement needed something the engine
    # already reports and no capture can show -- JCE_PERF_LOG's per-view GPU
    # breakdown is the case that forced this: the cost of a shader change is a
    # GPU cost, wall-clock around a whole run cannot resolve it (5 runs spread
    # 18.2-21.7 s on an 18 s job), and the number was being printed all along
    # into a pipe pointed at nothing.
    log_f = open(args.log, "wb") if args.log else subprocess.DEVNULL
    p = subprocess.Popen([str(EXE)], env=env, cwd=str(EXE.parent),
                         stdout=log_f, stderr=subprocess.STDOUT
                                       if args.log else subprocess.DEVNULL)
    # With --stride the editor writes name.png, name_1.png, name_2.png ...
    # from ONE run, which is the only way two shots are guaranteed to be
    # consecutive: the shot frame does NOT map to the same spin step across
    # separate process launches (startup timing shifts it), so a "pair"
    # captured as two runs can silently be the same pose -- or two poses many
    # steps apart.
    # The editor names a strided sequence name_0.png, name_1.png, ... so the
    # last one to wait for is name_{pairs-1}.png.
    want = png if not args.stride else OUT / f"{args.name}_{args.pairs-1}.png"
    for k in range(0, args.pairs + 60):
        try:
            (OUT / f"{args.name}_{k}.png").unlink(missing_ok=True)
        except OSError:
            pass
    deadline = time.time() + args.timeout
    while time.time() < deadline and not want.exists():
        time.sleep(0.5)
    time.sleep(1.5)          # let the PNG writer thread finish
    p.kill()

    if scene != ROOT / args.scene:
        scene.unlink(missing_ok=True)   # the override copy is not an asset
    if args.rp:
        # Never leave it behind: it would silently re-tier every later run of
        # the editor, including the user's own.
        rp_path.unlink(missing_ok=True)
        try:
            rp_path.parent.rmdir()
        except OSError:
            pass

    if not want.exists():
        sys.exit("capture produced no image -- the shot frame was never reached")
    if args.stride:
        # A stride keeps firing until the process dies; drop the overshoot so
        # only the requested pair remains and nobody diffs an unintended frame.
        time.sleep(1.0)          # let the PNG writer thread release its files
        for k in range(args.pairs, args.pairs + 60):
            try:
                (OUT / f"{args.name}_{k}.png").unlink(missing_ok=True)
            except OSError:
                pass             # still held; harmless, it is not in the pair

    manifest = dict(name=args.name, scene=args.scene, view=args.view,
                    view_params=view, backend=args.backend,
                    pan=[args.dx, args.dz],
                    shot_frame=args.frame, spin=args.spin,
                    overrides=sorted(args.set or []),
                    rp=sorted(args.rp or []), rp_force=rp_force,
                    cvar=sorted(args.cvar or []),
                    env=sorted(args.env or []),
                    no_build=os.environ.get("JCE_ENVSHOT_NO_BUILD", "") not in ("", "0"),
                    git=sh(["git", "rev-parse", "--short", "HEAD"]).stdout.strip())
    (OUT / f"{args.name}.json").write_bytes(
        json.dumps(manifest, indent=2, ensure_ascii=False).encode("utf-8"))
    print(f"captured {png}  (view={args.view} git={manifest['git']})")


def _load(name):
    png = OUT / f"{name}.png"
    base = name.rsplit("_", 1)[0] if (
        "_" in name and name.rsplit("_", 1)[1].isdigit()
        and not (OUT / f"{name}.json").exists()) else name
    man = OUT / f"{base}.json"
    if not png.exists():
        sys.exit(f"no capture named {name!r} -- run: envshot.py capture --name {name}")
    return png, json.loads(man.read_text(encoding="utf-8"))


def compare(args):
    try:
        from PIL import Image
        import numpy as np
    except ImportError:
        sys.exit("compare needs Pillow and numpy")

    pa, ma = _load(args.a)
    pb, mb = _load(args.b)

    # Refuse to compare captures that differ in more than the variable under
    # test.  Two pictures taken from different cameras will always differ, and
    # that difference reads exactly like the effect being looked for.
    fixed = ("scene", "view", "view_params", "backend", "shot_frame", "spin")
    mismatch = [k for k in fixed if ma.get(k) != mb.get(k)]
    if mismatch and not args.force:
        print("REFUSING to compare -- these captures differ in:", ", ".join(mismatch))
        for k in mismatch:
            print(f"   {k}: {ma.get(k)!r}  vs  {mb.get(k)!r}")
        print("A camera or scene mismatch reads exactly like the effect you are")
        print("looking for. Re-capture, or pass --force if you meant this.")
        sys.exit(2)

    A = np.asarray(Image.open(pa).convert("RGB")).astype(np.float32)
    B = np.asarray(Image.open(pb).convert("RGB")).astype(np.float32)
    if A.shape != B.shape:
        sys.exit(f"size mismatch {A.shape} vs {B.shape}")

    h, w, _ = A.shape
    x0, y0, x1, y1 = REGIONS[args.region]
    sl = (slice(int(h * y0), int(h * y1)), slice(int(w * x0), int(w * x1)))
    ca, cb = A[sl], B[sl]
    d = np.abs(ca - cb)
    dm = d.max(axis=2)

    print(f"region={args.region}  {ca.shape[1]}x{ca.shape[0]} px")
    print(f"  {args.a}: mean RGB {ca.reshape(-1,3).mean(0).round(1)}  std {ca.std():.2f}")
    print(f"  {args.b}: mean RGB {cb.reshape(-1,3).mean(0).round(1)}  std {cb.std():.2f}")
    stats = _diff_stats(ca, cb)
    print(f"  diff: mean {stats['mean']:.3f}  max {stats['max']:.0f}  "
          f"changed {stats['n']} px ({stats['pct']:.3f}%)  "
          f">4: {stats['p4']:.2f}%   >16: {stats['p16']:.2f}%")

    # THE VERDICT, and what this instrument is allowed to conclude on its own.
    #
    # It used to print "BIT-IDENTICAL: the change had no effect here" whenever
    # the MEAN difference was under 0.01 -- a claim about the average, printed
    # as a claim about every pixel. A change that moved 4468 pixels by up to 13
    # grey levels in a 1.07-megapixel frame has a mean of 0.002 and was reported
    # as having had no effect at all. That is the instrument telling you your fix
    # did nothing while the picture disagrees, and it is worse than no verdict
    # because it ends the investigation.
    #
    # The obvious repair -- print "CHANGED" whenever any pixel differs -- is the
    # SAME mistake mirrored, and it was written here first before being measured.
    # Capturing the identical build twice at the identical frame on this machine
    # moves 10492 px with a peak of 14: MORE than the real change above. Both
    # verdicts were confident and neither was entitled to be.
    #
    # So the rule is: this command may state exactly one conclusion from one
    # comparison -- that nothing differs. Everything else needs a noise floor,
    # and a noise floor is a second measurement, not a constant somebody
    # remembers. Pass --noise A,B (two captures of the SAME build) and it will
    # judge against it; without one it reports numbers and says what is missing.
    if stats['n'] == 0:
        print("  => IDENTICAL in this region: not one pixel differs.")
    elif args.noise:
        na, nb = [s.strip() for s in args.noise.split(",", 1)]
        NA = np.asarray(Image.open(OUT / f"{na}.png").convert("RGB")).astype(np.float32)[sl]
        NB = np.asarray(Image.open(OUT / f"{nb}.png").convert("RGB")).astype(np.float32)[sl]
        nf = _diff_stats(NA, NB)
        print(f"  noise floor ({na} vs {nb}): mean {nf['mean']:.3f}  "
              f"max {nf['max']:.0f}  changed {nf['n']} px ({nf['pct']:.3f}%)")
        # Above the floor on EITHER axis counts. A change can be broad and
        # shallow (fog, exposure) or narrow and deep (a blade of grass, a shadow
        # edge); requiring both would miss one of the two kinds entirely.
        broader = stats['n']   > nf['n']   * 1.5
        deeper  = stats['max'] > nf['max'] * 1.5
        if broader or deeper:
            which = " and ".join(
                [x for x in ("broader" if broader else "", "deeper" if deeper else "") if x])
            print(f"  => ABOVE THE NOISE FLOOR ({which}): this is a real change.")
        else:
            print("  => AT OR BELOW THE NOISE FLOOR: this comparison cannot tell "
                  "the change from run-to-run variation. NOT proof of no effect -- "
                  "find a camera or setting where the effect is larger, or make "
                  "the ablation bigger.")
    else:
        print(f"  => {stats['n']} px differ (peak {stats['max']:.0f}/255). No verdict: "
              f"pass --noise A,B (two captures of the same build) to say whether "
              f"this is above run-to-run variation. The mean being small is NOT "
              f"evidence of no effect -- most of the region is simply unaffected.")

    if args.write_diff:
        out = OUT / f"diff_{args.a}_{args.b}.png"
        Image.fromarray(np.clip(dm * 4, 0, 255).astype(np.uint8)).save(out)
        print(f"  wrote {out}")


def _diff_stats(ca, cb):
    """Everything the verdict needs, computed once so the comparison and its
    noise floor are summarised by identical arithmetic. When they were computed
    by two different expressions the floor was not comparable to the thing it
    was supposed to bound.

    numpy is imported here rather than at module scope for the same reason the
    callers do it: `list` and `capture` must run on a machine with no numpy."""
    import numpy as np
    d  = np.abs(ca - cb)
    dm = d.max(axis=2)
    n  = int((dm > 0).sum())
    return {"mean": float(d.mean()), "max": float(d.max()), "n": n,
            "pct": 100.0 * n / dm.size,
            "p4": float(100 * (dm > 4).mean()), "p16": float(100 * (dm > 16).mean())}


def flicker(args):
    """Residual between two adjacent SPIN frames after motion compensation.

    Camera motion changes every pixel, so a raw frame difference says nothing
    about flicker. Compensating the best rigid shift first removes the part the
    motion explains; what survives is temporal instability -- alpha-test
    coverage flipping, texture aliasing, LOD popping.
    """
    try:
        from PIL import Image
        import numpy as np
    except ImportError:
        sys.exit("flicker needs Pillow and numpy")

    pa, ma = _load(args.a)
    pb, mb = _load(args.b)
    for k in ("scene", "view", "backend", "spin"):
        if ma.get(k) != mb.get(k):
            sys.exit(f"the pair differs in {k}: {ma.get(k)!r} vs {mb.get(k)!r}")
    # A strided sequence shares one manifest, so adjacency is carried by the
    # _N suffix rather than by shot_frame. Two shots from SEPARATE runs are
    # never accepted: the shot frame does not map to the same spin step across
    # process launches, so such a "pair" can silently be the same pose.
    ia = args.a.rsplit("_", 1)
    ib = args.b.rsplit("_", 1)
    same_seq = (len(ia) == 2 and len(ib) == 2 and ia[0] == ib[0]
                and ia[1].isdigit() and ib[1].isdigit()
                and abs(int(ia[1]) - int(ib[1])) == 1)
    if not same_seq:
        sys.exit(
            "flicker needs an ADJACENT pair from ONE run:"
            "  envshot.py capture --name S --spin 1.5 --stride 1 --pairs 2"
            "  envshot.py flicker S_0 S_1"
            "Two separate captures are not a pair: the shot frame does not"
            "land on the same spin step across process launches.")

    def vp(p):
        a = np.asarray(Image.open(p).convert("L")).astype(np.float32)
        h, w = a.shape
        x0, y0, x1, y1 = REGIONS[args.region]
        return a[int(h*y0):int(h*y1), int(w*x0):int(w*x1)]

    A, B = vp(pa), vp(pb)
    raw = float(np.abs(A - B).mean())
    best, shift = 1e18, (0, 0)
    for dy in (-3, -2, -1, 0, 1, 2, 3):
        for dx in range(-40, 41):
            Ac = A[8+dy:A.shape[0]-8+dy, 44+dx:A.shape[1]-44+dx]
            Bc = B[8:B.shape[0]-8, 44:B.shape[1]-44]
            hh = min(Ac.shape[0], Bc.shape[0]); ww = min(Ac.shape[1], Bc.shape[1])
            e = float(np.abs(Ac[:hh,:ww] - Bc[:hh,:ww]).mean())
            if e < best:
                best, shift = e, (dx, dy)
    # A pair the camera never moved between cannot measure flicker, and would
    # report a reassuring near-zero residual. That is the same shape of failure
    # this whole tool exists to prevent, so it is refused rather than reported.
    #
    # It is a REAL case: JCE_DBG_VISTA_SPIN was observed to do nothing at some
    # camera setups (draw count stayed pinned at its static value), so --spin
    # cannot be assumed to have taken effect just because it was passed.
    if shift == (0, 0) and raw < 0.5:
        print(f"REFUSING: these two frames are the same pose "
              f"(rigid shift {shift}, raw diff {raw:.3f}).")
        print("The camera did not move between them, so there is no flicker to")
        print("measure. --spin did not take effect; verify it by checking that")
        print("the draw count rises (static ~195 vs rotating ~1400) before")
        print("trusting any residual from this pair.")
        sys.exit(2)

    # NORMALISE BY CAMERA SPEED, and say so loudly.
    #
    # The residual grows with how far the camera moved between the two frames,
    # so two runs are only comparable if their per-frame motion matches. That
    # is not automatic: --spin advances the yaw once per camera update, the
    # shot fires once per frame, and the two do not have to keep step across
    # separate process launches. Measured on one A/B here: 11.40 px per frame
    # in one run and 14.40 in the other from the SAME --spin value -- a 26%
    # difference in camera speed, which produced a 22.6% difference in raw
    # residual that read exactly like a rendering effect. Normalised, the same
    # A/B was 3.7%, i.e. inside this harness's own run-to-run variance.
    #
    # So the per-pixel-of-motion figure is the one to compare, and even it is
    # only good to about 20% between runs.
    px = max((shift[0] ** 2 + shift[1] ** 2) ** 0.5, 1e-6)
    print(f"region={args.region}  best rigid shift {shift}  (|shift| {px:.2f} px)")
    print(f"  raw adjacent-frame diff      {raw:7.3f}")
    print(f"  residual PER PIXEL OF MOTION {best / px:7.4f}   "
          f"<- compare THIS across runs, never the raw residual")
    print(f"  motion-compensated residual  {best:7.3f}   "
          f"({100*best/max(raw,1e-6):.0f}% unexplained by motion)")
    print("  lower residual = less flicker")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("capture")
    c.add_argument("--name", required=True)
    c.add_argument("--view", default="forest", choices=sorted(VIEWS))
    c.add_argument("--scene", default=DEFAULT_SCENE)
    c.add_argument("--backend", default="d3d11")
    c.add_argument("--spin", type=float, default=0.0,
                   help="degrees per frame; 0 = static")
    c.add_argument("--set", action="append", metavar="rendering.a.b=VALUE",
                   help="override a scene rendering setting for this capture")
    c.add_argument("--rp", action="append", metavar="enable_csm=false",
                   help="render-pipeline override; the full HIGH preset is "
                        "written with these applied on top")
    c.add_argument("--frame", type=int, default=CAPTURE_FRAME,
                   help="frame index to capture; two consecutive values "
                        "with --spin give an adjacent-frame pair")
    c.add_argument("--pitch", type=float, default=None,
                   help="override the view's pitch, in degrees (for angle sweeps)")
    c.add_argument("--yaw", type=float, default=None,
                   help="override the view's yaw, in degrees (for angle sweeps)")
    c.add_argument("--dx", type=float, default=0.0,
                   help="pan the whole orbit rig along world X, in metres")
    c.add_argument("--dz", type=float, default=0.0,
                   help="pan the whole orbit rig along world Z, in metres")
    c.add_argument("--dist", type=float, default=None,
                   help="override the view's orbit distance, in metres")
    c.add_argument("--target", metavar="X,Y,Z", default=None,
                   help="override the orbit target, in world metres; with "
                        "--dist/--pitch/--yaw this reproduces a camera "
                        "bookmark exactly (.jce/editor-state.json stores "
                        "x,y,z,yaw,pitch,dist with the ANGLES IN RADIANS)")
    c.add_argument("--play", metavar="WALK_X,WALK_Z", nargs="?", const="0,0",
                   help="capture the GAME view in Play mode instead of the "
                        "scene view; the optional argument feeds a constant "
                        "walk vector so the game camera is in motion (a defect "
                        "reported as 'when the view moves' cannot be measured "
                        "from a still frame)")
    c.add_argument("--tod-run", action="store_true",
                   help="let the day/night clock keep running; the captured "
                        "hour then depends on startup timing and the shot is "
                        "NOT reproducible")
    c.add_argument("--env", action="append", metavar="JCE_X=1",
                   help="extra environment variable for this capture")
    c.add_argument("--cvar", action="append", metavar="r.taa=0",
                   help="pin a cvar for this capture (JCE_CVAR)")
    c.add_argument("--stride", type=int, default=0,
                   help="repeat the shot every N frames, from ONE run; "
                        "the only way to get a genuine consecutive pair")
    c.add_argument("--pairs", type=int, default=1,
                   help="how many shots to wait for when using --stride")
    c.add_argument("--timeout", type=float, default=90.0)
    c.add_argument("--log", metavar="PATH",
                   help="keep the editor's stdout+stderr here "
                        "(JCE_PERF_LOG output lands in it)")
    c.set_defaults(func=capture)

    m = sub.add_parser("compare")
    m.add_argument("a"); m.add_argument("b")
    m.add_argument("--region", default="viewport", choices=sorted(REGIONS))
    m.add_argument("--write-diff", action="store_true")
    m.add_argument("--noise", metavar="A,B",
                   help="two captures of the SAME build; judges the comparison "
                        "against their difference instead of guessing")
    m.add_argument("--force", action="store_true")
    m.set_defaults(func=compare)

    f = sub.add_parser("flicker")
    f.add_argument("a"); f.add_argument("b")
    f.add_argument("--region", default="viewport", choices=sorted(REGIONS))
    f.set_defaults(func=flicker)

    l = sub.add_parser("list")
    l.set_defaults(func=lambda a: [
        print(f"{p.stem:20s} {json.loads(p.read_text(encoding='utf-8')).get('view'):8s} "
              f"{json.loads(p.read_text(encoding='utf-8')).get('git')}")
        for p in sorted(OUT.glob("*.json")) if p.stem != "_scene_override"])

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
