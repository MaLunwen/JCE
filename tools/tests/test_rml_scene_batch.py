#!/usr/bin/env python3
"""Compile/execute the complete Rml scene overlay against counted CPU ports.

The tag registry and find callback/function are read from the actual tag owner.
No RmlUi, ECS, SDL or GPU is instantiated. A per-binding scan mutation must
compile and fail the unchanged operation-count assertions normally (exit 1).
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


def function(source: str, name: str) -> str:
    match = re.search(r"^(?:static )?(?:void|bool|uint16_t) " + re.escape(name) + r"\(", source, re.M)
    if match is None:
        raise RuntimeError("missing owner function: " + name)
    start = match.start()
    begin = source.index("{", start)
    depth = 1
    for at in range(begin + 1, len(source)):
        depth += (source[at] == "{") - (source[at] == "}")
        if depth == 0:
            return source[start:at + 1] + "\n"
    raise RuntimeError("unclosed owner function: " + name)


def prepare(root: Path, tag_owner: Path, build: Path) -> dict:
    header = root / "engine/include/jce/application/jce_rml_scene_overlay.h"
    source = header.read_text(encoding="utf-8")
    tags = tag_owner.read_text(encoding="utf-8")
    registry_start = tags.index("typedef struct {\n    char name[JCE_TAG_NAME_MAX];")
    registry_end = tags.index("static char       s_layers", registry_start)
    find_start = tags.index("typedef struct {", tags.index("Find helpers"))
    find_end = tags.index("int jce_scene_find_all_with_tag", find_start)
    tag_code = tags[registry_start:registry_end] + "\n"
    tag_code += function(tags, "ensure_tag_init") + function(tags, "jce_tag_intern")
    tag_code += function(tags, "jce_tag_remove") + tags[find_start:find_end]
    scans = (
        "    JceRmlSceneTargets targets;\n"
        "    jce_rml_scene_overlay_resolve(&targets,o,scene,(1u << 6) - 1u);\n"
    )
    lookup = "        JceEntity e = jce_rml_scene_overlay_target(&targets,i);\n"
    # Only the actual sync owner is changed. All dependency ports/assertions stay shared.
    sync_start = source.index("static void jce_rml_scene_overlay_sync(")
    sync_end = source.index("static void jce_rml_scene_overlay_frame(", sync_start)
    sync = source[sync_start:sync_end]
    if sync.count(scans) != 1 or sync.count(lookup) != 1:
        raise RuntimeError("per-binding control needs the exact sync owner wires")
    old_sync = sync.replace(scans, "").replace(lookup,
        "        JceEntity e = jce_scene_find_with_tag(scene,b->tag);\n")
    variants = {"current": source, "per_binding": source[:sync_start] + old_sync + source[sync_end:]}
    headers = re.findall(r"^#include <(jce/[^>]+)>$", source, re.M)
    for variant, content in variants.items():
        ports = build / variant / "ports"
        for name in headers:
            path = ports / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "rml_scene_batch_ports.h"\n', encoding="utf-8")
        out = ports / "jce/application/jce_rml_scene_overlay.h"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(content, encoding="utf-8")
        (ports / "rml_tag_lookup_owner.h").write_text(tag_code, encoding="utf-8")
    return {"header_sha256": hashlib.sha256(header.read_bytes()).hexdigest(),
            "tag_owner_sha256": hashlib.sha256(tag_owner.read_bytes()).hexdigest(),
            "scope": "complete overlay header and actual tag helpers; counted UI/ECS/input/clock ports"}


def main() -> None:
    default_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=default_root)
    parser.add_argument("--tag-owner", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--legacy-header", type=Path)
    args = parser.parse_args()
    root = args.source_root.resolve()
    build = (args.build_dir or root / "build/rml-scene-batch-controls").resolve()
    tag_owner = args.tag_owner or root / "engine/src/middleware/scene/jce_scene_tags_layers.c"
    receipt = prepare(root, tag_owner, build)
    receipt["fixture_sha256"] = {
        name: hashlib.sha256((root / "tests/application" / name).read_bytes()).hexdigest()
        for name in ("test_jce_rml_scene_batch.c", "rml_scene_batch_ports.h", "rml_scene_legacy_cases.h")
    }
    if args.prepare_only:
        print(json.dumps(receipt));return
    source = root / "tests/application/test_jce_rml_scene_batch.c"
    variants = ["current", "per_binding"]
    if args.legacy_header:
        shutil.copytree(build / "current/ports", build / "legacy/ports", dirs_exist_ok=True)
        shutil.copyfile(args.legacy_header, build / "legacy/ports/jce/application/jce_rml_scene_overlay.h")
        variants.append("legacy")
    results = []
    for lang, standard, command in [("c", "c99", "cc"), ("c++", "c++20", "c++")]:
        compiler = shutil.which(command)
        if compiler is None:
            raise SystemExit("UNAVAILABLE: " + command)
        for variant in variants:
            binary = build / variant / ("rml-batch-" + standard)
            cmd = [compiler, "-x", lang, "-std=" + standard, "-O2", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-function", "-I" + str(build / variant / "ports"),
                   "-I" + str(root / "tests/application"), str(source), "-o", str(binary)]
            compile_result = subprocess.run(cmd, text=True, capture_output=True, timeout=60)
            if compile_result.returncode:
                raise RuntimeError("compile failed: " + variant + "\n" + compile_result.stdout + compile_result.stderr)
            run = [str(binary)] + ([] if variant == "current" else ["--budget-only"])
            result = subprocess.run(run, text=True, capture_output=True, timeout=30)
            expected = 0 if variant == "current" else 1
            admitted = result.returncode == expected and (variant == "current" or "FAIL " in result.stderr)
            entry = {"variant": variant, "language": standard, "compile_command": cmd,
                     "run_command": run, "returncode": result.returncode, "expected": expected,
                     "accepted": admitted, "stdout": result.stdout, "stderr": result.stderr}
            results.append(entry)
            print(json.dumps(entry), flush=True)
    receipt["results"] = results
    receipt["accepted"] = all(item["accepted"] for item in results)
    (build / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    if not receipt["accepted"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
