"""Compare ordinary/batched model submissions and alternating light layers."""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[4]))
from check_asset_pixels import cell_pixel_changes, full_pixel_difference
from tools.benchmark_suite import sha256_file
from tools.jce_determinism import read_png


def validate_layer_deltas(deltas):
    if len(deltas) != 100 or any(not isinstance(value, int) or value < 0 for value in deltas):
        raise ValueError("the controlled 100-character layer grid is incomplete")
    if any(deltas[index] != 0 for index in range(0, 100, 2)) or \
            any(deltas[index] == 0 for index in range(1, 100, 2)):
        raise ValueError("layer-0 changed or layer-1 failed to respond to the mask")


def submission_difference(first, second):
    fraction = full_pixel_difference(first, second)
    if fraction >= 0.0001:
        raise ValueError("ordinary/batched parity exceeds the static witness pixel tolerance")
    ca, cb = first[2], second[2]
    deltas = [abs(first[3][i * ca + c] - second[3][i * cb + c])
              for i in range(first[0] * first[1]) for c in range(3)]
    return {"exact_rgb_equal": not any(deltas), "maximum_channel_difference": max(deltas),
            "nonidentical_channels": sum(value != 0 for value in deltas),
            "fraction_above_channel_delta_30": fraction, "allowed_fraction": 0.0001}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("masked", "solo", "all", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    paths = (args.masked, args.solo, args.all)
    docs = [json.loads(path.read_text(encoding="utf-8")) for path in paths]
    if any(doc["status"] != "LIVENESS_PASS" or doc["receiver_layers"] != [0, 1]
           for doc in docs) or len({doc["executable_sha256"] for doc in docs}) != 1:
        raise ValueError("matching executable and alternating-layer witnesses required")
    if [doc["directional_light_mask"] for doc in docs] != [1, 1, 0]:
        raise ValueError("masked/solo/all light masks do not match the control recipe")
    rows = []
    for name in ("frozen20", "moving20", "moving50"):
        samples = [doc["results"][name] for doc in docs]
        for sample in samples:
            if sha256_file(sample["image"]) != sample["image_sha256"]:
                raise ValueError("a raw capture changed")
        images = [read_png(sample["image"]) for sample in samples]
        parity = submission_difference(images[0], images[1])
        deltas = cell_pixel_changes(images[0], images[2], 100)
        validate_layer_deltas(deltas)
        rows.append({"sample": name, "submission_parity": parity,
                     "layer_mask_changed_pixels": deltas})
    result = {"status": "PIXEL_TOLERANCE_PASS", "inputs": [
        {"file": str(path.resolve()), "sha256": sha256_file(path)} for path in paths],
        "scope": "100 alternating layer-0/1 Fox actors; submission parity within existing static pixel tolerance, not exact identity; per-cell mask response",
        "samples": rows}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"status": result["status"], "output": str(args.out.resolve())}))


if __name__ == "__main__":
    main()
