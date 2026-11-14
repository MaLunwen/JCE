#!/usr/bin/env python3
"""Compile exact collector/ecull source regions with controlled CPU ports.

This is a narrow source-owner control, not a full renderer/SDK build or a
native performance benchmark. Generated files stay in the selected build dir.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path


def write_if_changed(path: Path, data: bytes) -> None:
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def region(text: str, begin: str, end: str) -> str:
    if text.count(begin) != 1 or text.count(end) != 1:
        raise ValueError(f"Source boundary is not unique: {begin!r} / {end!r}")
    a, b = text.index(begin), text.index(end)
    if b <= a:
        raise ValueError("Source boundary order changed")
    return text[a:b]


def replace_once(text: str, before: str, after: str) -> str:
    if text.count(before) != 1:
        raise ValueError(f"Mutation target not unique: {before!r}")
    return text.replace(before, after, 1)


def mutate(source: str, name: str) -> str:
    if name == "current":
        return source
    if name == "old_unconditional_generation":
        return replace_once(source, "if (!same_layout) s_col_list_gen++;", "s_col_list_gen++;")
    if name == "missing_bounds_guard":
        return replace_once(source,
            "                sr->model_bounds_focus_collect_valid &&\n"
            "                s_col_focus_revision == sr->model_bounds_focus_revision &&\n", "")
    if name == "missing_count_guard":
        return replace_once(source,
            "collect_ctx.complete && list.count == collect_ctx.prior_count;",
            "collect_ctx.complete;")
    if name == "missing_order_guard":
        return replace_once(source,
            "    if (ctx->same_layout &&\n"
            "        (list->count >= ctx->prior_count || list->entities[list->count] != e))\n"
            "        ctx->same_layout = false;\n", "")
    if name == "cache_partial_collection":
        return replace_once(source, "s_col_valid  = collect_ctx.complete;", "s_col_valid  = true;")
    if name in ("old_default_note", "old_default_entry", "old_default_reset"):
        return source
    if name == "missing_default_drain":
        return replace_once(source,
            "    if (!sr->model_bounds_enabled) {\n"
            "        /* A successful load or eviction can change bounds without changing\n"
            "         * entity transforms. Coalesce publications until this boundary; never\n"
            "         * clear tables while a synchronous model callback is using them. */\n"
            "        sr_model_bounds_drop_tables(sr);\n"
            "        sr_model_bounds_retire_geometry(sr);\n"
            "        return;\n"
            "    }\n", "")
    if name == "old_default_focus_hit":
        source = replace_once(source,
            "                         s_col_owner == sr && sr->model_bounds_focus_collect_valid &&\n"
            "                         s_col_focus_revision == sr->model_bounds_focus_revision &&\n", "")
        return replace_once(source,
            "            cache_hit = s_col_owner == sr && sr->model_bounds_focus_collect_valid &&\n"
            "                        s_col_focus_revision == sr->model_bounds_focus_revision &&\n",
            "            cache_hit = (!sr->model_bounds_enabled ||\n"
            "                         (s_col_owner == sr && sr->model_bounds_focus_collect_valid &&\n"
            "                          s_col_focus_revision == sr->model_bounds_focus_revision)) &&\n")
    if name == "missing_cold_bounds_guard":
        return replace_once(source,
            "        sr->model_bounds_focus_collect_valid && !sr->model_bounds_deps.fallback) {",
            "        !sr->model_bounds_deps.fallback) {")
    if name == "missing_all_proof_guard":
        return replace_once(source,
            "                         s_col_owner == sr && sr->model_bounds_focus_collect_valid &&\n"
            "                         s_col_focus_revision == sr->model_bounds_focus_revision &&\n", "")
    if name == "old_invalidation_gate":
        return replace_once(source,
            "    if (sr->model_bounds_enabled) {\n"
            "        sr_anim_instances_destroy(sr);\n"
            "    }\n"
            "    sr_model_bounds_reset(sr);",
            "    if (sr->model_bounds_enabled) {\n"
            "        sr_anim_instances_destroy(sr);\n"
            "        sr_model_bounds_reset(sr);\n"
            "    }")
    raise ValueError(name)


def mutate_bounds(source: str, name: str) -> str:
    if name == "old_default_note":
        return replace_once(source,
            "    sr->model_bounds_geometry_dirty = true;\n",
            "    if (!sr->model_bounds_enabled) return;\n"
            "    sr->model_bounds_geometry_dirty = true;\n")
    if name == "old_default_entry":
        return replace_once(source,
            "void sr_model_bounds_enter(JceSceneRenderer *sr, JceScene *scene)\n{\n",
            "void sr_model_bounds_enter(JceSceneRenderer *sr, JceScene *scene)\n{\n"
            "    if (!sr->model_bounds_enabled) return;\n")
    if name == "old_default_reset":
        return replace_once(source,
            "void sr_model_bounds_reset(JceSceneRenderer *sr)\n{\n",
            "void sr_model_bounds_reset(JceSceneRenderer *sr)\n{\n"
            "    if (!sr->model_bounds_enabled) return;\n")
    return source


def write_extracts(source: str, metadata: str, bounds: str, out: Path) -> None:
    pieces = {
        "collect_callbacks.inc": region(source, "bool sr_entity_list_reserve(",
            "/* ── Forward declarations for Phase 3 helpers"),
        "collect_metadata.inc": region(metadata, "void sr_ecull_metadata_begin(",
            "/* Diagnostic subdivision is opt-in"),
        "collect_repair.inc": region(source, "static uint64_t sr_ecull_mix64(",
            "/* wcache-hit fast path:") + region(source,
            "static bool sr_ecull_repair_dirty(", "/* ── Does any water body"),
        "collect_frame.inc": region(source, "    /* Collect entities.  STATIC",
            "    uint64_t _t0_animsel ="),
        "collect_freeze.inc": region(source, "    bool ecull_frozen = false;",
            "    /* Component/enable edits"),
        "collect_model_bounds.inc": region(bounds, "static bool model_subscriber_alive(",
            "void sr_model_bounds_track("),
        "collect_model_drain.inc": region(source,
            "typedef struct {\n    JceSceneRenderer *sr;\n    JceScene *scene;\n    const EntityList *frozen;",
            "static bool sr_model_bounds_repair_one(") + region(source,
            "static void sr_model_bounds_drain(", "/* Observe each newly terminal job"),
        "collect_ecull_boundary.inc": region(source,
            "    /* Animation or a synchronous resolver may publish after collection. */",
            "    /* CSM shadow-cache caster-state key:"),
        "collect_model_invalidate.inc": region(source,
            "void jce_scene_renderer_invalidate_model_cache(",
            "void jce_scene_renderer_get_lod_stats("),
    }
    for name, text in pieces.items():
        write_if_changed(out / name, text.encode("utf-8"))


def run(command: list[str], directory: Path) -> dict:
    result = subprocess.run(command, cwd=directory, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    return {"command": command, "returncode": result.returncode,
            "stdout": result.stdout, "stderr": result.stderr}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--reference-root", type=Path,
        help="Explicit pinned unchanged-source tree for a partial patch checkout")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--prepare-only", action="store_true",
        help="Generate strict source regions for the canonical CMake test targets")
    args = parser.parse_args()
    root = args.root.resolve()
    reference = args.reference_root.resolve() if args.reference_root else root
    build = args.build_dir.resolve() if args.build_dir else root / "build/validation/collect-layout"
    build.mkdir(parents=True, exist_ok=True)
    owner = root / "engine/src/middleware/scene/jce_scene_renderer.c"
    ecull = reference / "engine/src/middleware/scene/jce_sr_ecull.c"
    key = reference / "engine/src/middleware/scene/jce_sr_metadata_key.h"
    bounds = root / "engine/src/middleware/scene/jce_sr_model_bounds.c"
    fixture = root / "tests/middleware/scene/test_jce_collect_layout.c"
    source = owner.read_text(encoding="utf-8")
    metadata = ecull.read_text(encoding="utf-8")
    bounds_source = bounds.read_text(encoding="utf-8")
    variants = ["current", "old_unconditional_generation", "missing_bounds_guard",
                "missing_count_guard", "missing_order_guard", "cache_partial_collection",
                "old_default_note", "old_default_entry", "old_default_reset",
                "missing_default_drain", "old_default_focus_hit", "old_invalidation_gate",
                "missing_cold_bounds_guard", "missing_all_proof_guard"]
    report = {"schema": 1, "scope": "CPU source-owner controls; no native GPU/FPS claim",
        "inputs": {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                   for p in (owner, ecull, key, bounds, fixture)}, "runs": []}
    if args.prepare_only:
        for variant in variants:
            out = build / "native" / variant
            out.mkdir(parents=True, exist_ok=True)
            write_extracts(mutate(source, variant), metadata,
                mutate_bounds(bounds_source, variant), out)
            write_if_changed(out / key.name, key.read_bytes())
        return 0
    failures = []
    for language, compiler, standard in (("c99", args.cc, "c99"), ("cpp20", args.cxx, "c++20")):
        compiler = shutil.which(compiler)
        if not compiler:
            failures.append(f"Compiler unavailable: {language}")
            continue
        for variant in variants:
            out = build / language / variant
            out.mkdir(parents=True, exist_ok=True)
            write_extracts(mutate(source, variant), metadata,
                mutate_bounds(bounds_source, variant), out)
            shutil.copyfile(key, out / key.name)
            shutil.copyfile(fixture, out / fixture.name)
            binary = out / "collect_layout_test"
            command = [compiler, "-x", "c" if language == "c99" else "c++",
                f"-std={standard}", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-Wno-missing-field-initializers",
                "-I", str(out), str(out / fixture.name), "-o", str(binary)]
            compile_result = run(command, out)
            row = {"language": language, "variant": variant, "compile": compile_result}
            report["runs"].append(row)
            if compile_result["returncode"]:
                failures.append(f"Compilation failed: {language}/{variant}")
                continue
            row["execute"] = run([str(binary)], out)
            if variant == "current":
                if row["execute"]["returncode"]:
                    failures.append(f"Current controls failed: {language}")
                row["bypasses"] = [run([str(binary), flag], out)
                    for flag in ("collect-off", "metadata-off")]
                if any(r["returncode"] for r in row["bypasses"]):
                    failures.append(f"Bypass controls failed: {language}")
            elif row["execute"]["returncode"] != 1 or "ASSERTION FAILED" not in row["execute"]["stderr"]:
                failures.append(f"Negative control did not fail an ordinary assertion: {language}/{variant}")
            print(language, variant, "compile", compile_result["returncode"],
                  "run", row["execute"]["returncode"])
    report["failures"] = failures
    report["status"] = "PASS" if not failures else "FAIL"
    report_path = build / "report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(report["status"], report_path)
    for failure in failures:
        print(failure)
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
