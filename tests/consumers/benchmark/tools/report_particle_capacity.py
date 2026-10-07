#!/usr/bin/env python3
"""Revalidate typed GPU population controls; no performance ratios."""
import argparse
import json
from pathlib import Path
import re
from PIL import Image, ImageDraw
from particle_capacity_suite import CASES, asset_for, validate_counts
from particle_suite import scene
from gbuffer_asset_suite import image, sha256_file, write
from report_lights import BACKENDS, checked
from tools.jce_determinism import assert_backend


def load(path):
    value = json.loads(path.read_text(encoding="utf-8"))
    if value.get("status") != "GPU_CAPACITY_CONTROLS_PASS" or set(value.get("cases", {})) != set(CASES):
        raise ValueError("complete successful six-case capacity suite required")
    if value.get("backend") not in BACKENDS:
        raise ValueError("unknown backend")
    checked(Path(value["executable"]), value["executable_sha256"])
    computed = {}
    for label, config in CASES.items():
        row = value["cases"][label]
        files = row["files"]
        if set(files) != {"fixture.particles.json", "scene.json", "engine.log", "target.png"}:
            raise ValueError("missing raw capacity evidence")
        for raw in files.values(): checked(Path(raw["path"]), raw["sha256"])
        asset = Path(files["fixture.particles.json"]["path"])
        if json.loads(asset.read_text()) != asset_for(label) or \
                json.loads(Path(files["scene.json"]["path"]).read_text()) != scene(asset, config[0], True):
            raise ValueError("capacity fixture differs")
        process = row["process"]
        log = Path(files["engine.log"]["path"]).read_text(encoding="utf-8")
        if process["process"].get("returncode") != 0 or process["process"].get("timed_out") or "] ERROR -" in log:
            raise ValueError("capacity consumer failed")
        if assert_backend(process.get("stdout", "")+"\n"+log, value["backend"]) != process.get("backend_actual"):
            raise ValueError("actual backend differs")
        marker = re.findall(r"benchmark-particles: frame=50 components=(\d+) loaded=(\d+) cpu_handles=(\d+) gpu_requested=(\d+)", log)
        if len(marker) != 1 or int(marker[0][0]) != config[0] or int(marker[0][3]) != config[0]:
            raise ValueError("component routing differs")
        expected = {"JCE_BACKEND": value["backend"], "JCE_GPU_TIER": "high",
            "JCE_SCENE": files["scene.json"]["path"], "JCE_BENCH_PARTICLE_OBSERVE": "1",
            "JCE_GPU_PARTICLES_FORCE": "1", "JCE_BENCH_ASSET_STILL": "1",
            "JCE_BENCH_SHOT_FRAME": "50", "JCE_BENCH_PLAYER_SHOT": files["target.png"]["path"],
            "JCE_BENCH_PARTICLE_GPU_STATS": "1"}
        if row["environment"] != expected: raise ValueError("capacity environment differs")
        if image(Path(files["target.png"]["path"])).shape != (720,1280,4):
            raise ValueError("invalid capacity capture")
        computed[label] = validate_counts(label, log)
        if computed[label] != row.get("actual_gpu_alive") or row.get("expected_per_emitter") != config[-1]:
            raise ValueError("stored actual counts differ")
    return value, computed


def matrix(loaded):
    if len(loaded) != 4 or {v[0]["backend"] for v in loaded} != set(BACKENDS) or \
            len({v[0]["executable_sha256"] for v in loaded}) != 1:
        raise ValueError("four distinct backends with one executable required")
    return loaded


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", type=Path, action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    loaded = matrix([load(path) for path in args.suite])
    args.out.mkdir(parents=True, exist_ok=False)
    canvas = Image.new("RGB", (1100, 430), "#111820")
    draw = ImageDraw.Draw(canvas)
    draw.text((20,15), "Actual GPU particle populations at source frame 50 (not GPU timings)", fill="white")
    lines = ["# Actual GPU capacity evidence", "", "24 cases independently rehashed and exact counts recomputed from typed raw RGBA logs.",
        "Correctness only; rounded65 deliberately measures effective rounded allocation 128, not requested cap 65.",
        "Retirement/refill use frame-50 observations, not a claimed full lifecycle trace. CPU parity and R11 timings remain unverified.", "",
        "| Backend | Case | Counts per emitter | Total alive |", "|---|---|---|---:|"]
    records = []
    for row_index, (value, computed) in enumerate(loaded):
        y = 70+row_index*80
        draw.text((15,y), value["backend"], fill="white")
        for col, label in enumerate(CASES):
            total = computed[label]["total_alive"]
            x = 135+col*155
            draw.text((x,y-24), label, fill="white")
            draw.rectangle((x,y,x+max(1,total/512*130),y+20), fill="#47b6aa")
            draw.text((x,y+25), str(total), fill="white")
            counts = [r["alive"] for r in computed[label]["records"]]
            lines.append("| %s | %s | %s | %u |" % (value["backend"],label,counts,total))
        records.append({"backend": value["backend"], "executable_sha256": value["executable_sha256"],
            "source_claim": value.get("source"), "source_at_end_claim": value.get("source_at_end"),
            "source_changed": value.get("source_changed"), "computed": computed})
    canvas.save(args.out / "alive-counts.png")
    lines += ["", "Source metadata are retained claims; executable SHA is the immutable measurement identity.", "", "![Actual alive populations](alive-counts.png)"]
    (args.out / "REPORT.md").write_text("\n".join(lines)+"\n",encoding="utf-8",newline="\n")
    write(args.out / "recomputed.json", {"scope": "actual GPU population correctness only", "evidence": records,
        "inputs": [{"path": str(p.resolve()), "sha256": sha256_file(p)} for p in args.suite]})


if __name__ == "__main__": main()
