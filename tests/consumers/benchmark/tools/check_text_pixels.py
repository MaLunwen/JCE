#!/usr/bin/env python3
"""Capture public-Canvas multilingual labels with explicit host fonts.

Image liveness is automated; shaping, fallback and RTL require human review of
the retained capture. Font bytes are hashed but never copied or redistributed.
"""
import argparse
import json
from pathlib import Path
import sys
import uuid

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
from tools.benchmark_scene import write_scene
from tools.benchmark_suite import run_visual_witness, visual_delta_files, sha256_file, source_identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--font", required=True, type=Path)
    parser.add_argument("--fallback", type=Path)
    parser.add_argument("--backend", default="d3d11")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    for path in (args.exe, args.font, args.fallback):
        if path is not None and not path.is_file():
            parser.error("an explicit executable/font input is missing: " + str(path))
    directory = args.out.resolve() / uuid.uuid4().hex[:12]
    directory.mkdir(parents=True, exist_ok=False)
    scene = write_scene(directory / "fixture.scene.json", "drawcall", 1)
    base = {"JCE_SCENE": str(scene), "JCE_BACKEND": args.backend, "JCE_WINDOW_HIDDEN": "1",
            "JCE_BENCH_TEXT_CAPTURE": "1"}
    outputs = {}
    for mode in ("empty", "multilingual"):
        target = directory / mode
        target.mkdir()
        env = dict(base)
        if mode == "multilingual":
            env.update(JCE_BENCH_TEXT=mode, JCE_BENCH_TEXT_FONT=str(args.font.resolve()))
            if args.fallback:
                env["JCE_BENCH_TEXT_FALLBACK"] = str(args.fallback.resolve())
        witness = run_visual_witness(env, target, exe=args.exe.resolve(), timeout=600, host="player")
        if mode == "multilingual":
            log = (target / "visual.log").read_text(encoding="utf-8")
            if log.count("benchmark-text: mode=multilingual labels=4") != 1:
                raise ValueError("requested text fixture did not execute")
        outputs[mode] = {"image": str(target / "frame.png"), "sha256": sha256_file(target / "frame.png"),
                         "witness": witness}
    changed = visual_delta_files(outputs["empty"]["image"], outputs["multilingual"]["image"])
    result = {"status": "LIVENESS_PASS" if changed > 0.01 else "FAIL", "shaping_rtl_review": "UNVERIFIED",
              "changed_fraction": changed, "outputs": outputs, "source": source_identity(),
              "executable_sha256": sha256_file(args.exe), "backend_requested": args.backend,
              "font": {"path": str(args.font.resolve()), "sha256": sha256_file(args.font)},
              "fallback": {"path": str(args.fallback.resolve()), "sha256": sha256_file(args.fallback)} if args.fallback else None,
              "scope": "post -> bridge -> Canvas frame-20 witness; not isolated shaping/upload timing"}
    (directory / "result.json").write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
    print(json.dumps({"directory": str(directory), "status": result["status"], "changed_fraction": changed}, indent=2))
    return 0 if result["status"] == "LIVENESS_PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
