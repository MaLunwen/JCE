"""Stock SDK host effect liveness, with repeated and empty picture controls."""
import argparse
import json
from pathlib import Path
import re
import sys
import uuid

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
from check_asset_pixels import full_pixel_difference
from culling_suite import read_png, sha256_file, source_identity
from postfx_fixture import EFFECTS
from tools.perf_bench import run

FLAGS = re.compile(r"stock-host: effect=(\w+) taa=(\d) ssao=(\d) ssr=(\d) bloom=(\d) fog=(\d)")
VIEWS = re.compile(r"stock-view: frame=(\d+) id=(\d+) name=([^\r\n]*?) at [^\r\n]+:\d+")


def validate_log(log, effect, *, before=None):
    rows = FLAGS.findall(log)
    expected = (effect, *[str(int(effect == name)) for name in EFFECTS[1:]])
    expected_rows = [expected]
    if before is not None:
        expected_rows.insert(0, (before, *[str(int(before == name)) for name in EFFECTS[1:]]))
    if rows != expected_rows or "] ERROR -" in log:
        raise ValueError("stock host did not apply the exact requested feature gates")
    views = {int(vid): name for frame, vid, name in VIEWS.findall(log) if frame == "120"}
    if not views or "Scene/Color" not in views.values():
        raise ValueError("stock host did not report live scene submissions")
    required = {"base": (), "taa": ("PostFX/TAA_Resolve",),
                "bloom": ("PostFX/BloomExtract",)}.get(effect, ())
    if any(name not in views.values() for name in required):
        raise ValueError("requested named effect pass was not submitted")
    expected_ids = {"ssao": (32, 33), "ssr": (48, 49), "fog": (45, 46)}.get(effect, ())
    if any(vid not in views for vid in expected_ids):
        raise ValueError("requested SDK scene effect view was not submitted")
    return views


def image_metrics(images):
    if any(image[:2] != (1280, 720) for image in images.values()):
        raise ValueError("incorrect stock host viewport size")
    repeat = full_pixel_difference(images["base"], images["repeat"], 2)
    coverage = full_pixel_difference(images["empty"], images["base"], 2)
    effects = {effect: full_pixel_difference(images["base"], images[effect], 2)
               for effect in EFFECTS[1:]}
    return {"coverage_fraction": coverage, "repeat_fraction": repeat,
            "effect_changed_fractions": effects, "rgb_threshold": 2}


def validate_metrics(metrics):
    if metrics["repeat_fraction"] > 0.0001 or metrics["coverage_fraction"] < 0.005:
        raise ValueError("empty or repeated controls failed")
    if any(value <= metrics["repeat_fraction"] + 0.00001
           for value in metrics["effect_changed_fractions"].values()):
        raise ValueError("an effect did not exceed the control noise floor")


def validate_images(images):
    metrics = image_metrics(images)
    validate_metrics(metrics)
    return metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "gl", "vulkan"), required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if not args.exe.is_file():
        parser.error("existing installed-SDK consumer required")
    root = args.out.resolve() / uuid.uuid4().hex[:12]
    root.mkdir(parents=True)
    exe_hash = sha256_file(args.exe)
    images, cases = {}, {}
    result = {"status": "INCOMPLETE", "source": source_identity(),
              "executable": str(args.exe.resolve()), "executable_sha256": exe_hash,
              "backend": args.backend, "scope": "stock SDK post-processing correctness; no timing claim",
              "cases": cases, "limitations": ["procedural PBR room, not an external engine scene",
                  "fixed jitter phase is repeatability evidence, not TAA convergence or motion quality",
                  "pass presence and pixels do not independently prove physical lighting fidelity"]}
    output = root / "suite.json"
    try:
        for label in ("empty", "base", "repeat", *EFFECTS[1:]):
            target = root / label
            target.mkdir()
            effect = "base" if label in ("empty", "repeat") else label
            scene_name = "empty" if label == "empty" else effect
            scene = ROOT / "tests/consumers/benchmark/resources/postfx" / (scene_name + ".scene.json")
            config = target / "profile.ini"
            config.write_text("[renderer]\nvsync=false\n\n[performance]\nmachine_class=full\njob_workers=2\n",
                              encoding="utf-8", newline="\n")
            env = {"JCE_BACKEND": args.backend, "JCE_BENCH_POST_EFFECT": effect,
                   "JCE_STARTUP_SCENE": "postfx/" + scene_name + ".scene.json",
                   "JCE_BENCH_CONFIG_FILE": str(config),
                   "JCE_KPI_GAME_SHOTS": "0.999|" + str(target / "frame.png"),
                   "JCE_KPI_GAME_CAPTURE_SIZE": "1280x720", "JCE_GPU_TIER": "high"}
            (target / "intent.json").write_text(json.dumps(env, indent=2) + "\n",
                                                encoding="utf-8", newline="\n")
            process = run(env, target / "engine.log", frames=181, exe=args.exe.resolve())
            log = (target / "engine.log").read_text(encoding="utf-8")
            views = validate_log(log, effect)
            images[label] = read_png(target / "frame.png")
            cases[label] = {"process": process, "views": views, "directory": str(target),
                            "scene_sha256": sha256_file(scene),
                            "image_sha256": sha256_file(target / "frame.png"),
                            "log_sha256": sha256_file(target / "engine.log")}
            if sha256_file(args.exe) != exe_hash:
                raise ValueError("SDK consumer changed during matrix")
            print(json.dumps({"backend": args.backend, "case": label, "status": "CAPTURED"}), flush=True)
        result.update(image_metrics(images))
        validate_metrics(result)
        result["status"] = "POSTFX_LIVENESS_PASS"
    except Exception as error:
        result.update(status="FAIL", error=str(error))
        raise
    finally:
        output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(json.dumps({"output": str(output), "status": result["status"]}), flush=True)


if __name__ == "__main__":
    main()
