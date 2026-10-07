#!/usr/bin/env python3
"""Directional-shadow pixel liveness controls; not complete R10 acceptance.

The movement pair excludes pixels changed by unshadowed geometry. No independent
light-space silhouette oracle, per-cascade draws or shadow GPU timing is claimed.
"""
import argparse
import math
import re
from pathlib import Path

import numpy as np

from gbuffer_asset_suite import difference, image, sha256_file, write
from postfx_fixture import fixture
from tools.benchmark_suite import source_identity
from tools.jce_determinism import assert_backend
from tools.perf_bench import run

WIDTH, HEIGHT = 1280, 720
CASES = ("empty-off", "empty-on", "off", "on", "repeat", "moved-off", "moved-on")


def scene(label, *, camera_mode="orthographic", distinct_casters=False,
          sun_direction=(-.6, -1, -.2), receiver_tilt_deg=0):
    if label not in CASES:
        raise ValueError("unknown shadow case")
    if len(sun_direction) != 3 or not all(math.isfinite(x) for x in sun_direction) \
            or sum(x*x for x in sun_direction) <= 0:
        raise ValueError("sun direction must be finite and nonzero")
    if not math.isfinite(receiver_tilt_deg) or abs(receiver_tilt_deg) > 30:
        raise ValueError("receiver tilt must be finite and within 30 degrees")
    result = fixture(empty=True)
    camera, sun = result["entities"]
    camera["components"][0].update(posX=0, posY=30, posZ=0,
        rotX=-90, rotY=0, rotZ=0)
    camera["components"][1].update(orthographic=True, orthoSize=28,
        nearClip=.1, farClip=60, clearMode=2)
    if camera_mode == "perspective":
        camera["components"][0].update(posX=0, posY=25, posZ=25,
            rotX=-45, rotY=0, rotZ=0)
        camera["components"][1].update(orthographic=False, fov=55,
            nearClip=.1, farClip=90)
    elif camera_mode != "orthographic":
        raise ValueError("unknown camera mode")
    sun["components"][1].update(dirX=sun_direction[0], dirY=sun_direction[1],
        dirZ=sun_direction[2],
        intensity=2, castsShadow=label in ("empty-on", "on", "repeat", "moved-on"))
    result["rendering"]["shadows"] = {
        "distance": 60, "cascades": 2, "resolution": 1024, "soft": 0}
    result["rendering"]["ambient"] = {"color": [.1, .1, .1], "intensity": .2}
    result["rendering"]["postfx"].update(ssao=False, ssr=False, bloom=False,
        autoExposure=False, fxaa=False)

    def box(name, position, scale):
        result["entities"].append({"id": len(result["entities"]) + 1, "name": name,
            "components": [{"type": "Transform",
                **dict(zip(("posX", "posY", "posZ"), position)),
                **dict(zip(("scaleX", "scaleY", "scaleZ"), scale))},
                {"type": "MeshRenderer", "meshShape": 0, "visible": True,
                 "baseColorR": .6, "baseColorG": .6, "baseColorB": .6,
                 "roughness": 1, "metallic": 0}]})

    box("Receiver", (0, -.2, 0), (40, .4, 28))
    if not label.startswith("empty"):
        for index in range(64):
            x, z = (index % 8 - 3.5) * 3, (index // 8 - 3.5) * 3
            if label.startswith("moved") and index == 27:
                x += 1.2
            box("Caster%u" % index, (x, 1, z), (.8, 2, .8))
            # Contrast is required for the independent geometry-only movement
            # control: an exact overhead camera sees only the cube top, which
            # otherwise matches the receiver material and disappears in pixels.
            result["entities"][-1]["components"][1].update(
                baseColorR=.25, baseColorG=.35, baseColorB=.55)
            if distinct_casters:
                result["entities"][-1]["components"][1].update(
                    baseColorR=.2, baseColorG=.4, baseColorB=.6)
    if receiver_tilt_deg:
        # Rigidly tilt receiver AND casters around world Z, preserving contacts
        # and the same tangential movement. Tilting only the floor intersects
        # cubes and would confound the independent geometry-movement control.
        angle = math.radians(receiver_tilt_deg)
        c, sine = math.cos(angle), math.sin(angle)
        for entity in result["entities"][2:]:
            transform = entity["components"][0]
            x, y = transform["posX"], transform["posY"]
            transform.update(posX=x*c-y*sine, posY=x*sine+y*c,
                rotZ=receiver_tilt_deg)
    return result


def mask(first, second):
    return np.max(np.abs(first[:, :, :3].astype(np.int16)
                        - second[:, :, :3].astype(np.int16)), axis=2) > 2


def validate(images):
    if set(images) != set(CASES):
        raise ValueError("incomplete shadow controls")
    if any(p.shape != (HEIGHT, WIDTH, 4) for p in images.values()):
        raise ValueError("unexpected capture dimensions")
    # A floor alone must not acquire a large self-shadow when the sun is enabled.
    empty = difference(images["empty-off"], images["empty-on"])
    repeated = difference(images["on"], images["repeat"])
    geometry = difference(images["empty-off"], images["off"])
    changed = mask(images["on"], images["off"])
    darker = np.mean(images["off"][:, :, :3].astype(np.float64)
                     - images["on"][:, :, :3], axis=2) > 3
    moved_geometry = mask(images["off"], images["moved-off"])
    # Reject geometry-only movement: the shadow-on pair must change outside
    # pixels changed by its independently rendered shadow-off movement pair.
    movement = mask(images["on"], images["moved-on"]) & ~moved_geometry
    metrics = {"empty_delta": empty, "repeat_delta": repeated,
        "caster_geometry_delta": geometry, "shadow_delta": float(changed.mean()),
        "darkened_fraction": float((changed & darker).mean()),
        "geometry_movement": float(moved_geometry.mean()),
        "shadow_movement_outside_geometry": float(movement.mean())}
    if empty > .0001 or repeated > .0001 or geometry < .001 \
            or metrics["darkened_fraction"] < .001 \
            or metrics["geometry_movement"] < .00001 \
            or metrics["shadow_movement_outside_geometry"] < .00001:
        raise ValueError("shadow empty/repeat/darkening/movement control failed")
    return metrics


def primitive_environment(selection):
    if selection is None:
        return {}
    if selection not in ("on", "off"):
        raise ValueError("unknown primitive-instancing selection")
    return {"JCE_PRIM_INSTANCE": "1" if selection == "on" else "0"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "vulkan", "gl"), required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--primitive-instancing", choices=("on", "off"),
        help="Explicit path diagnostic; omission preserves the renderer default")
    parser.add_argument("--camera", choices=("orthographic", "perspective"),
        default="orthographic")
    parser.add_argument("--diagnostics", action="store_true",
        help="CSM logs/profiler and shadow-cache bypass; not formal performance")
    parser.add_argument("--distinct-casters", action="store_true",
        help="Diagnostic caster material contrast; default fixture unchanged")
    parser.add_argument("--sun-direction", type=float, nargs=3,
        default=(-.6, -1, -.2), metavar=("X", "Y", "Z"))
    parser.add_argument("--receiver-tilt-deg", type=float, default=0,
        help="Rigid Z-axis tilt of receiver and casters, bounded to +/-30 degrees")
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    binary_hash = sha256_file(args.exe)
    result = {"status": "INCOMPLETE", "backend": args.backend,
        "executable": str(args.exe.resolve()), "executable_sha256": binary_hash,
        "source": source_identity(), "cases": {},
        "controls": {"primitive_instancing": args.primitive_instancing,
            "camera": args.camera, "diagnostics": args.diagnostics,
            "distinct_casters": args.distinct_casters,
            "sun_direction": args.sun_direction, "receiver_tilt_deg": args.receiver_tilt_deg},
        "inputs": {"casters": 64, "directional_lights": 1,
                   "authored_cascades": 2, "authored_map_resolution": 1024},
        "scope": "Sampled shadow pixel liveness only; effective cascade/map state, "
                 "independent shadow projection, draws/GPU/memory and R10 timing remain unverified"}
    write(args.out / "suite.json", result)
    try:
        images = {}
        for label in CASES:
            directory = args.out / label
            directory.mkdir()
            value = scene(label, camera_mode=args.camera,
                distinct_casters=args.distinct_casters, sun_direction=args.sun_direction,
                receiver_tilt_deg=args.receiver_tilt_deg)
            write(directory / "scene.json", value)
            env = {"JCE_BACKEND": args.backend, "JCE_GPU_TIER": "high",
                "JCE_SCENE": str(directory / "scene.json"),
                "JCE_BENCH_ASSET_STILL": "1", "JCE_BENCH_SHOT_FRAME": "50",
                "JCE_BENCH_PLAYER_SHOT": str(directory / "target.png")}
            env.update(primitive_environment(args.primitive_instancing))
            if args.diagnostics:
                env.update(JCE_DBG_CSM_STATS="1",
                    JCE_DBG_CSM_LOG=str(directory / "csm_uniforms.log"),
                    JCE_DBG_SHPROF="1", JCE_DISABLE_SHADOWCACHE="1",
                    JCE_BENCH_SHADOW_TARGETS="1")
            process = run(env, directory / "engine.log", frames=200, exe=args.exe.resolve())
            log = (directory / "engine.log").read_text(encoding="utf-8")
            assert_backend(log, args.backend)
            if "] ERROR -" in log:
                raise ValueError("consumer error or actual backend mismatch")
            targets = []
            if args.diagnostics:
                count = re.search(r"benchmark-shadow-targets: frame=50 count=(\d+)", log)
                targets = re.findall(r"benchmark-shadow-target: frame=50 index=(\d+) name=(\S+) width=(\d+) height=(\d+) texture=(\d+) kind=(\d+)", log)
                if not count or len(targets) != int(count.group(1)) \
                        or [int(row[0]) for row in targets] != list(range(len(targets))):
                    raise ValueError("missing or incomplete public shadow target inventory")
            images[label] = image(directory / "target.png")
            if sha256_file(args.exe) != binary_hash:
                raise ValueError("executable changed during captures")
            retained = ["scene.json", "engine.log", "target.png"]
            if (directory / "csm_uniforms.log").is_file():
                retained.append("csm_uniforms.log")
            result["cases"][label] = {"process": process, "environment": env,
                "camera": value["entities"][0]["components"],
                "targets": [{"index": int(i), "name": name, "width": int(w),
                    "height": int(h), "texture": int(texture), "kind": int(kind)}
                    for i, name, w, h, texture, kind in targets],
                "files": {name: {"path": str(directory / name),
                    "sha256": sha256_file(directory / name)}
                    for name in retained}}
            write(args.out / "suite.json", result)
        result["metrics"] = validate(images)
        result["status"] = "SHADOW_PIXEL_LIVENESS_PASS"
    except Exception as error:
        result.update(status="FAIL", error=str(error))
        raise
    finally:
        write(args.out / "suite.json", result)


if __name__ == "__main__":
    main()
