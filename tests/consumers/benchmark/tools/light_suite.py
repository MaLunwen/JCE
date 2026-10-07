#!/usr/bin/env python3
"""Independent light-patch correctness, not lighting performance certification.

Every case retains 100 authored light components; zero/ten/hundred describe
positive-intensity participation, not component-count throughput. Reordered
sentinels probe authored-order loss, not an asserted internal shader-slot index.
The projected top-left ROIs must survive actual geometry and sentinel captures;
synthetic unit tests do not establish renderer projection or row-origin parity.
"""
import argparse
import math
from pathlib import Path

import numpy as np

from gbuffer_asset_suite import difference, image, sha256_file, write
from postfx_fixture import fixture
from tools.benchmark_suite import source_identity
from tools.perf_bench import run

WIDTH, HEIGHT, ORTHO_HEIGHT = 1280, 720, 30.0
COUNT, STEP = 100, 2.4
SENTINELS = (16, 64, 99)


def position(index):
    if not 0 <= index < COUNT:
        raise ValueError("patch index outside fixture")
    return ((index % 10 - 4.5) * STEP, (index // 10 - 4.5) * STEP)


def roi(index):
    """Project interior of a front-facing patch, with top-left image origin."""
    x, y = position(index)
    scale = HEIGHT / ORTHO_HEIGHT
    cx, cy = WIDTH / 2 + x * scale, HEIGHT / 2 - y * scale
    radius = .3 * scale
    return (math.ceil(cx - radius), math.ceil(cy - radius),
            math.floor(cx + radius), math.floor(cy + radius))


def scene(kind, enabled=(), *, first=None, geometry=False):
    if kind not in ("point", "spot"):
        raise ValueError("unknown light kind")
    enabled = set(enabled)
    if not enabled.issubset(range(COUNT)) or first not in (None, *range(COUNT)):
        raise ValueError("invalid light selection")
    result = fixture(empty=True)
    camera, sun = result["entities"]
    camera["components"][0].update(posX=0, posY=0, posZ=30, rotX=0, rotY=0, rotZ=0)
    camera["components"][1].update(orthographic=True, orthoSize=ORTHO_HEIGHT,
        nearClip=.1, farClip=60, clearMode=2)
    sun["components"][1].update(intensity=0, castsShadow=False)
    result["rendering"]["ambient"] = {"color": [0, 0, 0], "intensity": 0}
    result["rendering"]["postfx"].update(ssao=False, ssr=False, bloom=False,
        autoExposure=False, fxaa=False)
    for index in range(COUNT):
        x, y = position(index)
        result["entities"].append({"id": index + 3, "name": "Patch%u" % index,
            "components": [{"type": "Transform", "posX": x, "posY": y, "posZ": 0,
                "scaleX": 1, "scaleY": 1, "scaleZ": .1},
                {"type": "MeshRenderer", "meshShape": 0, "visible": True,
                 "baseColorR": .6, "baseColorG": .6, "baseColorB": .6,
                 "roughness": 1, "metallic": 0, "emissiveR": int(geometry),
                 "emissiveG": int(geometry), "emissiveB": int(geometry)}]})
    order = list(range(COUNT))
    if first is not None:
        order.remove(first)
        order.insert(0, first)
    for index in order:
        x, y = position(index)
        light = {"type": "PointLight" if kind == "point" else "SpotLight",
            "posX": x, "posY": y, "posZ": 1.2, "colorR": 1, "colorG": 1,
            "colorB": 1, "intensity": 3 if index in enabled else 0,
            "radius": 1.8, "castsShadow": False}
        if kind == "spot":
            light.update(dirX=0, dirY=0, dirZ=-1, innerConeDeg=25, outerConeDeg=35)
        result["entities"].append({"id": index + 103, "name": "Light%u" % index,
            "components": [{"type": "Transform", "posX": x, "posY": y,
                "posZ": 1.2, "scaleX": 1, "scaleY": 1, "scaleZ": 1}, light]})
    return result


def responses(pixels, baseline):
    if pixels.shape != (HEIGHT, WIDTH, 4) or baseline.shape != pixels.shape:
        raise ValueError("capture dimensions differ from projection")
    delta = pixels[:, :, :3].astype(np.float64) - baseline[:, :, :3]
    values = []
    for index in range(COUNT):
        x0, y0, x1, y1 = roi(index)
        values.append(float(delta[y0:y1, x0:x1].mean()))
    if not np.isfinite(values).all():
        raise ValueError("nonfinite patch response")
    return values


def validate(images):
    baseline = images["zero"]
    geometry = responses(images["geometry"], baseline)
    if min(geometry) < 20:
        raise ValueError("each projected patch must visibly exist")
    metrics = {"geometry": geometry}
    for label, active in (("ten", set(range(10))), ("hundred", set(range(COUNT))),
            ("omit-last", set(range(COUNT - 1)))):
        values = responses(images[label], baseline)
        if any(value < 8 if index in active else abs(value) > 2
               for index, value in enumerate(values)):
            raise ValueError("missing or unexpected light participation: " + label)
        metrics[label] = values
    if difference(images["hundred"], images["repeat"]) > .0001:
        raise ValueError("repeated light capture differs")
    for sentinel in SENTINELS:
        late = images["late-%u" % sentinel]
        early = images["first-%u" % sentinel]
        values = responses(late, baseline)
        if values[sentinel] < 8 or any(abs(v) > 2 for i, v in enumerate(values)
                if i != sentinel) or difference(late, early) > .0001:
            raise ValueError("late-index light or reordered control failed")
        metrics["late-%u" % sentinel] = values
    return metrics


def case_specs():
    result = [("geometry", (), None, True), ("zero", (), None, False),
        ("ten", range(10), None, False), ("hundred", range(COUNT), None, False),
        ("repeat", range(COUNT), None, False),
        ("omit-last", range(COUNT - 1), None, False)]
    for index in SENTINELS:
        result.extend([("late-%u" % index, (index,), None, False),
                       ("first-%u" % index, (index,), index, False)])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "vulkan", "gl"), required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    binary_hash = sha256_file(args.exe)
    result = {"status": "INCOMPLETE", "backend": args.backend,
        "executable": str(args.exe.resolve()), "executable_sha256": binary_hash,
        "source": source_identity(), "cases": {}, "scope":
        "100 independent patch light participation; no timing, shadows or clustered occupancy claim",
        "projection": {"width": WIDTH, "height": HEIGHT, "ortho_height": ORTHO_HEIGHT,
                       "rois": [roi(i) for i in range(COUNT)]}}
    write(args.out / "suite.json", result)
    try:
        for kind in ("point", "spot"):
            images = {}
            for label, active, first, geometry in case_specs():
                directory = args.out / kind / label
                directory.mkdir(parents=True)
                write(directory / "scene.json", scene(kind, active, first=first, geometry=geometry))
                env = {"JCE_BACKEND": args.backend, "JCE_FORWARDPLUS": "1",
                    "JCE_SCENE": str(directory / "scene.json"), "JCE_GPU_TIER": "high",
                    "JCE_BENCH_ASSET_STILL": "1", "JCE_BENCH_SHOT_FRAME": "50",
                    "JCE_BENCH_PLAYER_SHOT": str(directory / "target.png")}
                process = run(env, directory / "engine.log", frames=200, exe=args.exe)
                log = (directory / "engine.log").read_text(encoding="utf-8")
                if "] ERROR -" in log or "JCE_FORWARDPLUS set -> r.forwardplus ON" not in log:
                    raise ValueError("consumer error or missing explicit Forward+ authority")
                images[label] = image(directory / "target.png")
                if sha256_file(args.exe) != binary_hash:
                    raise ValueError("consumer executable changed")
                result["cases"][kind + "/" + label] = {"process": process, "environment": env,
                    "files": {name: {"path": str(directory / name),
                        "sha256": sha256_file(directory / name)}
                        for name in ("scene.json", "engine.log", "target.png")}}
                write(args.out / "suite.json", result)
            result[kind] = validate(images)
        result["status"] = "LIGHT_PATCH_CONTROLS_PASS"
    except Exception as error:
        result.update(status="FAIL", error=str(error))
        raise
    finally:
        write(args.out / "suite.json", result)


if __name__ == "__main__":
    main()
