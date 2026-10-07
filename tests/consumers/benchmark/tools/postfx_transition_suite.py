"""Check stock SDK effect transitions against separately rendered steady controls."""
import argparse
import json
from pathlib import Path
import uuid

from postfx_suite import ROOT, read_png, sha256_file, source_identity, validate_log
from check_asset_pixels import full_pixel_difference
from tools.perf_bench import run

CASES = (("ssr", None), ("base", None), ("ssao", "ssr"),
         ("taa", "ssr"), ("base", "ssr"), ("ssr", "base"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "gl", "vulkan"), required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    exe = args.exe.resolve(strict=True)
    root = args.out.resolve() / uuid.uuid4().hex[:12]
    root.mkdir(parents=True)
    exe_hash = sha256_file(exe)
    images, cases = {}, {}
    result = {"status": "INCOMPLETE", "backend": args.backend,
              "source": source_identity(), "executable_sha256": exe_hash,
              "scope": "effect transition correctness; no timing claim", "cases": cases}
    try:
        for initial, final in CASES:
            label = initial + ("-to-" + final if final else "-steady")
            target = root / label
            target.mkdir()
            config = target / "profile.ini"
            config.write_text("[renderer]\nvsync=false\n", encoding="utf-8", newline="\n")
            env = {"JCE_BACKEND": args.backend, "JCE_STARTUP_SCENE": "postfx/ssr.scene.json",
                   "JCE_BENCH_POST_EFFECT": initial, "JCE_BENCH_CONFIG_FILE": str(config),
                   "JCE_KPI_GAME_SHOTS": "0.999|" + str(target / "frame.png"),
                   "JCE_KPI_GAME_CAPTURE_SIZE": "1280x720", "JCE_GPU_TIER": "high"}
            if final:
                env.update(JCE_BENCH_POST_EFFECT_AFTER=final, JCE_BENCH_POST_SWITCH_FRAME="30")
            (target / "intent.json").write_text(json.dumps(env, indent=2) + "\n",
                                                encoding="utf-8", newline="\n")
            process = run(env, target / "engine.log", frames=181, exe=exe)
            log = (target / "engine.log").read_text(encoding="utf-8")
            validate_log(log, final or initial, before=initial if final else None)
            if final and log.count("stock-transition: frame=30 effect=" + final) != 1:
                raise ValueError("effect transition did not occur at the requested frame")
            images[label] = read_png(target / "frame.png")
            if images[label][:2] != (1280, 720) or sha256_file(exe) != exe_hash:
                raise ValueError("viewport or executable changed")
            cases[label] = {"process": process, "image_sha256": sha256_file(target / "frame.png"),
                            "log_sha256": sha256_file(target / "engine.log")}
            print(label, "CAPTURED", flush=True)
        coverage = full_pixel_difference(images["ssr-steady"], images["base-steady"], 2)
        differences = {initial + "-to-" + final: full_pixel_difference(
            images[initial + "-to-" + final], images[final + "-steady"], 2)
            for initial, final in CASES if final}
        result.update(effect_fraction=coverage, transition_fractions=differences, rgb_threshold=2)
        if coverage <= .00001 or any(delta > .0001 for delta in differences.values()):
            raise ValueError("transition differs from steady control or SSR is ineffective")
        result["status"] = "POSTFX_TRANSITIONS_PASS"
    except Exception as error:
        result.update(status="FAIL", error=str(error))
        raise
    finally:
        (root / "suite.json").write_text(json.dumps(result, indent=2) + "\n",
                                          encoding="utf-8", newline="\n")
        print(root, result["status"], flush=True)


if __name__ == "__main__":
    main()
