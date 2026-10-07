"""Distance-LOD route correctness; timings and cooked/streamed assets are separate."""
import argparse
import json
import math
from pathlib import Path
import re

from gbuffer_asset_suite import difference, image, sha256_file, write
from postfx_fixture import fixture
from tools.benchmark_suite import source_identity
from tools.perf_bench import run

MARKER = re.compile(r"benchmark-lod: frame=(\d+) enabled=(\d+) levels=(\d+) "
    r"picks=(\d+),(\d+),(\d+) culled=(\d+) pos=([-\d.]+),([-\d.]+),([-\d.]+)")
FRAMES = (20, 50, 80, 81, 100, 130, 160, 180)


def camera_z(frame):
    if 20 < frame <= 80:
        return 20 + (frame - 20) * 100 / 60
    if 80 < frame <= 100:
        return 120
    if 100 < frame <= 160:
        return 120 - (frame - 100) * 100 / 60
    return 20


def expected_route():
    """Independent distance/dead-zone oracle, using the public LOD contract."""
    previous = [-1] * 64
    result = []
    for frame in range(1, 201):
        counts = [0, 0, 0]
        for index in range(64):
            distance = math.sqrt(((index % 8 - 3.5) * 4) ** 2 + 20 ** 2
                + (camera_z(frame) - (index // 8 - 3.5) * 4) ** 2)
            level = previous[index]
            if level == -1:
                level = 0 if distance <= 45 else 1 if distance <= 75 else 2
            elif level < 2 and distance > (45, 75)[level] * 1.05:
                level += 1
            elif level > 0 and distance < (45, 75)[level - 1] * .95:
                level -= 1
            previous[index] = level
            counts[level] += 1
        result.append(counts)
    return result


def validate(log, enabled):
    rows = [[int(v) if i < 7 else float(v) for i, v in enumerate(row)]
            for row in MARKER.findall(log)]
    if [row[0] for row in rows] != list(range(1, 201)):
        raise ValueError("complete ordered LOD route frames required")
    expected = expected_route()
    for row in rows:
        frame, actual_enabled, levels, *tail = row
        if actual_enabled != enabled or levels != (3 if enabled else 0) \
                or tail[:3] != (expected[frame - 1] if enabled else [0, 0, 0]) \
                or tail[3] != 0 or abs(tail[4]) > .0001 or abs(tail[5] - 20) > .0001 \
                or abs(tail[6] - camera_z(frame)) > .0001:
            raise ValueError("LOD selection or camera differs at frame %u" % frame)
    return rows


def scene(empty=False):
    result = fixture(empty=True)
    result["rendering"]["postfx"].update(ssao=False, ssr=False)
    result["entities"][0]["components"][1].update(farClip=300, clearMode=2)
    result["entities"][1]["components"][1]["castsShadow"] = False
    if not empty:
        for index in range(64):
            result["entities"].append({"id": index + 3, "name": "Plane%u" % index,
                "components": [{"type": "Transform", "scaleX": 1, "scaleY": 1, "scaleZ": 1,
                    "posX": (index % 8 - 3.5) * 4, "posY": 0,
                    "posZ": (index // 8 - 3.5) * 4},
                    {"type": "MeshRenderer", "meshPath": "benchmark_lod_plane",
                     "visible": True, "baseColorR": .8, "baseColorG": .15,
                     "baseColorB": .1, "roughness": .7}]})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "vulkan", "gl"), required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    identity = sha256_file(args.exe)
    result = {"source": source_identity(), "executable": str(args.exe.resolve()),
              "executable_sha256": identity, "backend": args.backend, "cases": {},
              "scope": "global distance LOD correctness; no timing, cooked LOD or streaming claim"}
    for frame in FRAMES:
        captures = {}
        for mode in ("empty", "off", "on", "repeat"):
            directory = args.out / (mode + "-%u" % frame)
            directory.mkdir()
            write(directory / "scene.json", scene(mode == "empty"))
            process = run({"JCE_BACKEND": args.backend,
                "JCE_SCENE": str((directory / "scene.json").resolve()),
                "JCE_BENCH_LOD": "off" if mode in ("off", "empty") else "on",
                "JCE_BENCH_ASSET_STILL": "1", "JCE_GPU_TIER": "high",
                "JCE_BENCH_SHOT_FRAME": str(frame),
                "JCE_BENCH_PLAYER_SHOT": str((directory / "target.png").resolve())},
                directory / "engine.log", frames=200, exe=args.exe)
            log = (directory / "engine.log").read_text(encoding="utf-8")
            if "] ERROR -" in log:
                raise ValueError("LOD consumer logged an error")
            rows = validate(log, mode not in ("off", "empty"))
            captures[mode] = image(directory / "target.png")
            result["cases"][directory.name] = {"process": process, "markers": rows,
                "files": {name: {"path": str((directory / name).resolve()),
                                  "sha256": sha256_file(directory / name)}
                          for name in ("scene.json", "engine.log", "target.png")}}
            if sha256_file(args.exe) != identity:
                raise ValueError("consumer changed during the LOD route")
        coverage = difference(captures["off"], captures["empty"])
        if coverage <= .002:
            raise ValueError("plane geometry did not visibly render")
        parity = difference(captures["off"], captures["on"])
        repeat = difference(captures["on"], captures["repeat"])
        if parity > .001 or repeat > .0001:
            raise ValueError("equivalent plane LOD changed pixels or repeated output")
        result.setdefault("pixels", {})[str(frame)] = {"coverage": coverage,
            "parity": parity, "repeat": repeat}
        print(args.backend, frame, "LOD_ROUTE_PIXELS_PASS", coverage, parity, repeat, flush=True)
    result["status"] = "LOD_ROUTE_CONTROLS_PASS"
    write(args.out / "suite.json", result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
