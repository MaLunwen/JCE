"""Pack controlled shader inputs using verified stock and the public PAK CLI."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
from tools.assets.shader_stock import verify_stock, inside, xxh32

SUFFIX = {"d3d11": "dx11", "d3d12": "dx11", "vulkan": "spv", "gl": "glsl", "gles": "essl"}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8", newline="\n")


def verify_pack(bom, archive, expected):
    """Require the public packer's deep receipt to match every staged source byte."""
    header, totals, entries = bom.get("header"), bom.get("totals"), bom.get("entries")
    count = len(expected)
    if (bom.get("schema") != "jce.pakbom.v1" or not isinstance(bom.get("file"), str)
            or Path(bom["file"]).resolve() != archive.resolve()
            or bom.get("file_size") != archive.stat().st_size
            or bom.get("sources_verified") is not True
            or not isinstance(header, dict) or header.get("header_verified") is not True
            or header.get("entry_count") != count or not isinstance(totals, dict)
            or totals.get("entries") != count or totals.get("verify_ran") is not True
            or totals.get("verified_count") != count or totals.get("corrupt_count") != 0
            or totals.get("skipped_count") != 0 or not isinstance(entries, list)
            or len(entries) != count):
        raise ValueError("packed archive deep verification/count mismatch")
    seen, original_size = set(), 0
    for entry in entries:
        if not isinstance(entry, dict):
            raise ValueError("invalid packed archive entry")
        name, crc = entry.get("path"), entry.get("content_crc")
        if not isinstance(name, str) or name not in expected or name in seen:
            raise ValueError("packed archive addresses differ from staged selection")
        raw = expected[name].read_bytes()
        if (entry.get("verified") is not True or type(entry.get("original_size")) is not int
                or entry["original_size"] != len(raw) or not isinstance(crc, str)
                or not re.fullmatch("0x[0-9a-fA-F]{8}", crc) or xxh32(raw) != int(crc, 16)):
            raise ValueError("packed archive source byte verification mismatch")
        seen.add(name)
        original_size += len(raw)
    if seen != set(expected) or totals.get("original_size") != original_size:
        raise ValueError("packed archive byte totals mismatch")
    sources = bom.get("sources")
    if not isinstance(sources, list) or len(sources) != count:
        raise ValueError("packed archive source provenance incomplete")
    seen.clear()
    for source in sources:
        if not isinstance(source, dict):
            raise ValueError("invalid packed archive source")
        name = source.get("path")
        if (not isinstance(name, str) or name not in expected or name in seen
                or source.get("selected") is not False or not isinstance(source.get("source"), str)
                or Path(source["source"]).resolve() != expected[name].resolve()
                or source.get("size") != expected[name].stat().st_size):
            raise ValueError("packed archive source provenance mismatch")
        seen.add(name)


def prepare(stock, packer, out, backend):
    stock, packer, out = (Path(p).resolve() for p in (stock, packer, out))
    if not out.is_relative_to(ROOT / "build") or out.exists() or out.is_relative_to(stock):
        raise ValueError("fresh fixture output below repository build/ is required")
    if not packer.is_file():
        raise ValueError("explicit current public PAK executable required")
    catalog = verify_stock(stock)
    suffix = SUFFIX[backend]
    available = {entry["address"] for entry in catalog["files"]}
    required = {f"vs_grass_{suffix}.bin", f"fs_grass_{suffix}.bin", f"fs_grass_shadow_{suffix}.bin"}
    if not required <= available:
        raise ValueError("verified stock lacks current grass colour/depth stages")
    if sum(entry["size"] for entry in catalog["files"]) > 134217728:
        raise ValueError("fixture stock exceeds the explicit 128-MiB input budget")
    out.mkdir(parents=True, exist_ok=False)
    record = {"schema": 1, "state": "INPUTS_PREPARING", "backend": backend,
              "stock_manifest_sha256": sha(stock / "manifest.json"),
              "stock_profiles": catalog["profiles"], "packer_sha256": sha(packer),
              "stock_files": catalog["files"], "archives": {}, "boot_resource_root": "full",
              "scope": "native diagnostic inputs, not single-executable shipment acceptance"}
    write(out / "inputs.json", record)
    drops = {"full": set(), "missing_colour": {f"fs_grass_{suffix}.bin", f"fs_grass_shadow_{suffix}.bin"},
             "missing_depth": {f"fs_grass_shadow_{suffix}.bin"}}
    # One verified stock copy serves boot plus all controlled PAK inputs.
    for entry in catalog["files"]:
        destination = out / "full" / "shaders" / entry["address"]
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(inside(stock, entry["address"]), destination)
        if sha(destination) != entry["sha256"]:
            raise ValueError("copied stock bytes changed")
    anchor = out / "anchor"
    anchor.mkdir()
    (anchor / "anchor.txt").write_text("stable public SDK fixture anchor\n", encoding="utf-8", newline="\n")
    for variant in ("anchor", "missing_colour", "missing_depth", "full"):
        command = [str(packer), "--resource-dir", str(out / ("anchor" if variant == "anchor" else "full")),
                   "--pak-file", str(out / (variant + ".pak")),
                   "--header-file", str(out / (variant + ".h")),
                   "--manifest-file", str(out / (variant + ".cmake")),
                   "--json", str(out / (variant + "-bom.json")),
                   "--obj-format", "none", "--level", "3"]
        for omitted in sorted(drops.get(variant, set())):
            # The public CLI suffix has a 31-byte payload; these basenames fit.
            command += ["--exclude-suffix", omitted]
        process = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, encoding="utf-8", errors="replace", check=False)
        (out / (variant + "-pack.log")).write_text(process.stdout, encoding="utf-8", newline="\n")
        record["archives"][variant] = {"path": variant + ".pak", "command": command,
                                      "returncode": process.returncode,
                                      "omitted": sorted(drops.get(variant, set()))}
        write(out / "inputs.json", record)
        if process.returncode:
            raise ValueError("public PAK CLI failed; retained partial inputs")
        archive = out / (variant + ".pak")
        bom = json.loads((out / (variant + "-bom.json")).read_text(encoding="utf-8"))
        expected = {"anchor.txt": anchor / "anchor.txt"} if variant == "anchor" else {
            "shaders/" + entry["address"]: out / "full" / "shaders" / entry["address"]
            for entry in catalog["files"] if entry["address"] not in drops[variant]
        }
        verify_pack(bom, archive, expected)
        record["archives"][variant].update(sha256=sha(archive), bytes=archive.stat().st_size,
                                          bom_sha256=sha(out / (variant + "-bom.json")))
    if verify_stock(stock) != catalog or sha(packer) != record["packer_sha256"]:
        raise ValueError("stock or packer changed during staging")
    record["state"] = "PACKED_AND_BYTE_VERIFIED"
    write(out / "inputs.json", record)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("stock-root", "packer", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--backend", choices=tuple(SUFFIX), required=True)
    args = parser.parse_args()
    prepare(args.stock_root, args.packer, args.out, args.backend)


if __name__ == "__main__":
    main()
