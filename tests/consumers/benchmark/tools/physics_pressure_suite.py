#!/usr/bin/env python3
"""Actual runtime 2000-body controls; stationary velocity is NOT sleeping state."""

import argparse
import json
import math
from pathlib import Path
import re
from check_asset_pixels import transform, read_png, full_pixel_difference
from geometry_asset_suite import write, raw_hashes, validate_exit, WARMUP, MEASURED
from tools.benchmark_suite import (
    host_render_environment,
    player_counters,
    read_frames,
    frame_summary,
    sha256_file,
    source_identity,
)
from tools.perf_bench import run
from tools.jce_determinism import assert_backend

KINDS = ("boxes", "spheres", "mixed", "falling")
COUNT = 2000
LAST_OBSERVATION_FRAME = 900
CAPTURE_TAIL_FRAMES = 32


def correctness_frames(shot):
    if type(shot) is not int or shot not in (120, LAST_OBSERVATION_FRAME):
        raise ValueError("supported actual capture frame required")
    # Keep polling the existing asynchronous GPU readback after the final shot.
    # These diagnostic tail frames are outside the frame900 pose oracle.
    return max(LAST_OBSERVATION_FRAME, shot) + CAPTURE_TAIL_FRAMES


def initial_position(kind, index):
    if kind == "boxes":
        return [
            (index % 20 - 9.5) * 1.6,
            0.55 + (index // 400) * 1.0,
            ((index // 20) % 20 - 9.5) * 1.6,
        ]
    side = math.ceil(math.sqrt(COUNT))
    return [
        (index % side - (side - 1) / 2) * 1.6,
        6.0 if kind == "falling" else 2.0 + (index % 7) * 0.2,
        (index // side - (side - 1) / 2) * 1.6,
    ]


def fixture(kind):
    if kind not in KINDS:
        raise ValueError("unsupported physical shape control")
    pose = transform(0, 90, 90)
    pose["rotX"] = -45
    rows = [
        dict(
            id=1,
            name="Camera",
            components=[
                pose,
                dict(
                    type="Camera",
                    primary=True,
                    orthographic=True,
                    orthoSize=110,
                    nearClip=0.1,
                    farClip=6000,
                    clearMode=1,
                ),
            ],
        ),
        dict(
            id=2,
            name="Sun",
            components=[
                transform(),
                dict(
                    type="DirectionalLight", dirX=-0.5, dirY=-1, dirZ=-0.5, intensity=2
                ),
            ],
        ),
    ]
    if kind != "falling":
        ground = transform(0, -0.25, 0)
        ground.update(scaleX=100, scaleY=0.5, scaleZ=100)
        rows.append(
            dict(
                id=3,
                name="Floor",
                components=[
                    ground,
                    dict(
                        type="MeshRenderer",
                        meshShape=0,
                        visible=True,
                        baseColorR=0.35,
                        baseColorG=0.35,
                        baseColorB=0.35,
                    ),
                    dict(
                        type="Rigidbody",
                        bodyType=3,
                        mass=0,
                        useGravity=False,
                        friction=0.7,
                        ccdMode=0,
                    ),
                    dict(type="BoxCollider", sizeX=1, sizeY=1, sizeZ=1),
                ],
            )
        )
    for index in range(COUNT):
        sphere = kind == "spheres" or kind == "mixed" and index % 2 == 1
        shape = (
            dict(type="SphereCollider", radius=0.5)
            if sphere
            else dict(type="BoxCollider", sizeX=1, sizeY=1, sizeZ=1)
        )
        rows.append(
            dict(
                id=index + 10,
                name="PBody%u" % index,
                components=[
                    transform(*initial_position(kind, index), scale=0.9),
                    dict(
                        type="MeshRenderer",
                        meshShape=1 if sphere else 0,
                        visible=True,
                        baseColorR=0.7,
                        baseColorG=0.2,
                        baseColorB=0.12,
                    ),
                    dict(
                        type="Rigidbody",
                        bodyType=1,
                        mass=1,
                        friction=0.7,
                        restitution=0,
                        useGravity=True,
                        gravityScale=1,
                        freezeRotation=True,
                        ccdMode=0,
                    ),
                    shape,
                ],
            )
        )
    return dict(entities=rows)


def validate_inventory(text, kind, enabled, observer):
    expected_static = 0 if kind == "falling" else 1
    pattern = r"physics-pressure-inventory: frame=1 kind=(\w+) dynamic=(\d+) rigidbodies=(\d+) static=(\d+) actual_bodies=(\d+) enabled=(\d+) observer=(\d+) fixed_hz=60 gravity=-9.81 solver_iterations=10 ccd=discrete sleep=unavailable"
    expected = (
        kind,
        str(COUNT),
        str(COUNT + expected_static),
        str(expected_static),
        str(COUNT + expected_static if enabled else 0),
        str(int(enabled)),
        str(int(observer)),
    )
    if (
        re.findall(pattern, text) != [expected]
        or "] ERROR -" in text
        or "benchmark-player: entered Play" not in text
    ):
        raise ValueError("actual runtime population/configuration missing or wrong")
    return dict(
        dynamic=COUNT,
        static=expected_static,
        actual_bodies=COUNT + expected_static if enabled else 0,
        enabled=enabled,
        observer=observer,
        solver_iterations=10,
        ccd="discrete",
        sleep_transition="UNAVAILABLE_PUBLIC_API",
    )


def validate_records(records, kind, enabled):
    if [row.get("frame") for row in records] != [1, 120, 600, 900]:
        raise ValueError("actual body observation sequence incomplete")
    expected_bodies = COUNT + (kind != "falling") if enabled else 0
    for row in records:
        if (
            row.get("kind") != kind
            or row.get("enabled") != enabled
            or row.get("actual_bodies") != expected_bodies
            or row.get("finite") is not True
        ):
            raise ValueError("actual physics world differs or invalid pose")
        if row.get("sleep_transition") != "UNAVAILABLE_PUBLIC_API":
            raise ValueError("sleep state is not public-observable")
        bodies = row.get("bodies", [])
        if (
            len(bodies) != COUNT
            or [body.get("index") for body in bodies] != list(range(COUNT))
            or len({body["entity"] for body in bodies}) != COUNT
        ):
            raise ValueError("actual body population/identity incomplete")
        for body in bodies:
            for field, size in (
                ("position", 3),
                ("velocity", 3),
                ("angular_velocity", 3),
                ("rotation", 4),
            ):
                values = body.get(field, [])
                if len(values) != size or not all(
                    isinstance(v, (int, float)) and math.isfinite(v) for v in values
                ):
                    raise ValueError("nonfinite body state")
            if enabled and body["handle"] == 4294967295:
                raise ValueError("runtime body absent")
        if enabled and len({body["handle"] for body in bodies}) != COUNT:
            raise ValueError("duplicate runtime body handles")
        quiet = sum(
            math.sqrt(sum(v * v for v in b["velocity"])) < 0.05
            and math.sqrt(sum(v * v for v in b["angular_velocity"])) < 0.05
            for b in bodies
        )
        if row.get("stationary_velocity_proxy") != quiet:
            raise ValueError("stationary proxy does not match actual velocities")
        if any(type(b.get("contact_this_step")) is not bool for b in bodies) or row.get(
            "contacted_this_step"
        ) != sum(b["contact_this_step"] for b in bodies):
            raise ValueError("current contact observations inconsistent")
        if any(
            type(row.get(key)) is not int or row[key] < 0
            for key in ("begin_events", "persist_events", "end_events")
        ):
            raise ValueError("invalid actual contact event counts")
    for a, b in zip(records, records[1:]):
        if any(
            b[key] < a[key] for key in ("begin_events", "persist_events", "end_events")
        ):
            raise ValueError("contact events reversed")
    baseline = [body["entity"] for body in records[0]["bodies"]]
    if any([body["entity"] for body in row["bodies"]] != baseline for row in records):
        raise ValueError("runtime body recreated during control")
    if enabled:
        baseline_handles = [body["handle"] for body in records[0]["bodies"]]
        if any(
            [body["handle"] for body in row["bodies"]] != baseline_handles
            for row in records
        ):
            raise ValueError(
                "runtime full body handle/generation changed during control"
            )
    if not enabled:
        for row in records:
            if row["begin_events"] or row["persist_events"] or row["end_events"]:
                raise ValueError("disabled world generated contacts")
            for body in row["bodies"]:
                if (
                    max(
                        abs(a - b)
                        for a, b in zip(
                            body["position"], initial_position(kind, body["index"])
                        )
                    )
                    > 1e-5
                ):
                    raise ValueError("disabled physics changed authored pose")
        return dict(
            status="DISABLED_INVARIANCE_OBSERVED",
            sleep_transition="UNAVAILABLE_PUBLIC_API",
        )
    if kind == "falling":
        for row in records:
            if row["begin_events"] or row["persist_events"]:
                raise ValueError("unbound grid unexpectedly collided")
            elapsed = row["frame"] / 60
            for body in row["bodies"]:
                start = initial_position(kind, body["index"])
                if abs(
                    body["position"][1] - (start[1] - 0.5 * 9.81 * elapsed * elapsed)
                ) > max(0.5, 0.02 * elapsed * elapsed):
                    raise ValueError("unbound gravity trajectory differs")
                if row["frame"] >= 120 and body["velocity"][1] > -15:
                    raise ValueError("unbound body not falling")
        return dict(
            status="UNBOUND_GRAVITY_OBSERVED", sleep_transition="UNAVAILABLE_PUBLIC_API"
        )
    for row in records[2:]:
        if row["begin_events"] <= 0 or row["persist_events"] <= 0:
            raise ValueError("sustained contact evidence absent")
        for body in row["bodies"]:
            target = 0.45 + 0.9 * (body["index"] // 400) if kind == "boxes" else 0.45
            if abs(body["position"][1] - target) > 0.15:
                raise ValueError("independent floor/stack support height differs")
            if (
                math.sqrt(sum(v * v for v in body["velocity"])) >= 0.05
                or math.sqrt(sum(v * v for v in body["angular_velocity"])) >= 0.05
            ):
                raise ValueError("settled body velocity not stable")
    for a, b in zip(records[2]["bodies"], records[3]["bodies"]):
        if max(abs(x - y) for x, y in zip(a["position"], b["position"])) > 0.03:
            raise ValueError("support pose drifts between10and15simulation seconds")
    return dict(
        status="SUPPORTED_STATIONARY_POSES_OBSERVED",
        stationary_velocity_proxy=[row["stationary_velocity_proxy"] for row in records],
        sleep_transition="UNAVAILABLE_PUBLIC_API",
        scope="velocity/pose/contact observation; not Bullet sleeping-state transition",
    )


def environment(kind, enabled, observe, scene, target, backend):
    return dict(
        JCE_SCENE=str(scene),
        JCE_BACKEND=backend,
        JCE_WINDOW_HIDDEN="1",
        JCE_MULTI_INSTANCE="1",
        JCE_GPU_TIER="high",
        JCE_BENCH_PHYSICS_PRESSURE=kind,
        JCE_BENCH_PHYSICS_DISABLED=str(int(not enabled)),
        JCE_BENCH_PHYSICS_OBSERVE=str(int(observe)),
        JCE_BENCH_PHYSICS_RECORDS=str(target / "physics.jsonl"),
        JCE_PHYSICS_MAX_BODIES="4096",
        JCE_BENCH_PLAY_FRAME="1",
        JCE_BENCH_ASSET_STILL="1",
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path)
    parser.add_argument(
        "--backend", choices=("d3d11", "d3d12", "vulkan", "gl"), default="d3d11"
    )
    parser.add_argument("--kind", choices=(*KINDS, "all"), default="all")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--timing", action="store_true")
    parser.add_argument("--repeats", type=int, choices=(3, 7), default=7)
    args = parser.parse_args()
    if not args.dry_run and (not args.exe or not args.exe.is_file()):
        parser.error("actual runs require installed-SDK consumer --exe")
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    result = dict(
        status="INCOMPLETE",
        source=source_identity(),
        backend=args.backend,
        cases={},
        scope=__doc__,
        generator_sha256=sha256_file(Path(__file__)),
        last_numeric_observation_frame=LAST_OBSERVATION_FRAME,
        capture_tail_frames=CAPTURE_TAIL_FRAMES,
        sleep_transition="UNAVAILABLE_PUBLIC_API",
        ccd_counter=None,
        solver_counter=None,
        limitations=[
            "Configured CCD discrete/solver10 are inputs; internal broadphase/narrowphase/solver counters unavailable.",
            "Correctness observer listener/fullbody traversal excluded from formal timing; still extra cost in correctness frames.",
            "No physical input-to-display or matched industry ratio, no physical512MiB device certification.",
        ],
    )
    if args.exe:
        result["executable_sha256"] = sha256_file(args.exe)
    try:
        for kind in KINDS if args.kind == "all" else (args.kind,):
            directory = args.out / kind
            directory.mkdir()
            scene = directory / "scene.json"
            write(scene, fixture(kind))
            scene_hash = sha256_file(scene)
            case = dict(scene_sha256=scene_hash, controls={}, timings=[])
            result["cases"][kind] = case
            if args.dry_run:
                continue
            images = {}
            late = 120 if kind == "falling" else 900
            for label, enabled, shot in (
                ("early", True, 120),
                ("settled", True, late),
                ("repeat", True, late),
                ("disabled", False, late),
            ):
                target = directory / label
                target.mkdir()
                env = environment(kind, enabled, True, scene, target, args.backend)
                env.update(host_render_environment("player", target, witness=True))
                env["JCE_BENCH_SHOT_FRAME"] = str(shot)
                process = run(
                    env,
                    target / "engine.log",
                    frames=correctness_frames(shot),
                    timeout=900,
                    exe=args.exe.resolve(),
                    samples_path=target / "frames.csv",
                )
                validate_exit(process)
                text = (target / "engine.log").read_text()
                assert_backend(text, args.backend)
                inventory = validate_inventory(text, kind, enabled, True)
                records = [
                    json.loads(line)
                    for line in (target / "physics.jsonl").read_text().splitlines()
                    if line
                ]
                oracle = validate_records(records, kind, enabled)
                images[label] = read_png(target / "frame.png")
                if images[label][:2] != (1280, 720):
                    raise ValueError("actual preview dimensions differ")
                resources = player_counters(
                    (target / "engine_counters.jsonl").read_text()
                )
                if any(
                    r["engine"]["entity_count"] != COUNT + 2 + (kind != "falling")
                    for r in resources
                ):
                    raise ValueError("actual rendered entity population differs")
                if sha256_file(scene) != scene_hash:
                    raise ValueError("runtime modified input scene")
                case["controls"][label] = dict(
                    environment=env,
                    process=process,
                    inventory=inventory,
                    oracle=oracle,
                    resources=resources,
                    raw_sha256=raw_hashes(target),
                )
                print(
                    json.dumps(dict(kind=kind, control=label, status=oracle["status"])),
                    flush=True,
                )
                write(args.out / "suite.json", result)
            case["pixel_delta"] = dict(
                physics_disabled=full_pixel_difference(
                    images["disabled"], images["settled"], 5
                ),
                repeat=full_pixel_difference(images["settled"], images["repeat"], 5),
            )
            if (
                case["pixel_delta"]["physics_disabled"] <= 0.001
                or case["pixel_delta"]["repeat"] > 0.0001
            ):
                raise ValueError("physical movement/repeat image controls failed")
            if args.timing:
                for repeat in range(args.repeats):
                    target = directory / ("timing-%u" % repeat)
                    target.mkdir()
                    env = environment(kind, True, False, scene, target, args.backend)
                    env.update(host_render_environment("player", target))
                    process = run(
                        env,
                        target / "engine.log",
                        frames=WARMUP + MEASURED + 1,
                        timeout=900,
                        exe=args.exe.resolve(),
                        samples_path=target / "frames.csv",
                    )
                    validate_exit(process)
                    text = (target / "engine.log").read_text()
                    assert_backend(text, args.backend)
                    inventory = validate_inventory(text, kind, True, False)
                    if (target / "physics.jsonl").exists():
                        raise ValueError("fullbody diagnostic leaked into timing")
                    samples = read_frames(
                        target / "frames.csv", warmup=WARMUP, sample_count=MEASURED
                    )
                    case["timings"].append(
                        dict(
                            environment=env,
                            process=process,
                            inventory=inventory,
                            frame=frame_summary(samples),
                            physics_step_scope="Existing runtime physics phase if present; frame accumulated physics CPU, not isolated solver counters",
                            raw_sha256=raw_hashes(target),
                        )
                    )
                    if sha256_file(scene) != scene_hash:
                        raise ValueError("timing scene changed")
            write(args.out / "suite.json", result)
        if args.exe and sha256_file(args.exe) != result["executable_sha256"]:
            raise ValueError("consumer changed")
        result["status"] = (
            "DRY_RUN_VALIDATED" if args.dry_run else "P00_FUNCTIONAL_OBSERVATIONS"
        )
        result["performance_status"] = (
            "WHOLE_FRAME_OBSERVED" if args.timing and not args.dry_run else "NOT_RUN"
        )
        result["p00_complete"] = False
    except Exception as error:
        result.update(status="FAILED", error=str(error))
        raise
    finally:
        result["raw_sha256"] = {
            str(p.relative_to(args.out)): sha256_file(p)
            for p in args.out.rglob("*")
            if p.is_file() and p != args.out / "suite.json"
        }
        write(args.out / "suite.json", result)
    print(
        json.dumps(dict(suite=str(args.out / "suite.json"), status=result["status"])),
        flush=True,
    )


if __name__ == "__main__":
    main()
