"""Regenerate exact stock BRDF using the shared host-tool build/cache contract."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import struct
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.build import jce

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def validate(data):
    if len(data) != 256 * 256 * 8:
        raise ValueError("stock payload length mismatch")
    for scale, bias, blue, alpha in struct.iter_unpack("<4e", data):
        if not (0 <= scale <= 4 and 0 <= bias <= 4 and blue == 0 and alpha == 1):
            raise ValueError("invalid BRDF channels")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--baker", type=Path)
    args = parser.parse_args()
    baker = args.baker
    if baker is None:
        baker = jce.ensure_host_tools(jce.msvc_env(jce.resolve_target(None)),
                                      ("jce_ibl_bake",))["jce_ibl_bake"]
    baker = baker.resolve(strict=True)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(baker), "--output", str(args.out.resolve())], check=True)
    validate(args.out.read_bytes())
    sources = [ROOT / "engine/src/renderer" / name for name in
               ("jce_ibl_sampling.h", "jce_ibl_brdf.h")]
    manifest = dict(schema=1, algorithm="split-sum GGX importance sampling",
        size=256, samples=1024, channels="RGBA16F", byte_order="little",
        texel_coordinates="(coordinate + 0.5) / size",
        half_policy="existing truncation; subnormals become signed zero",
        data_sha256=digest(args.out), bytes=args.out.stat().st_size,
        kernel_sha256={p.name:digest(p) for p in sources},
        baker_sha256=digest(baker), generator_sha256=digest(Path(__file__)),
        reference_host=platform.platform(),
        portability="fixed baked bytes; libm regeneration may vary by host")
    args.out.with_suffix(args.out.suffix + ".json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps(manifest, indent=2))
if __name__ == "__main__":
    main()
