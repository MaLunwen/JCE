#!/usr/bin/env python3
"""Compile the actual classic shader and a source-derived GPU power witness.

Uses the existing project-shader fixture and SDK compile/cache/lint recipe.
Compilation is portability evidence only. Numeric results require running the
witness or the production material on an actual GPU; this tool never does so.
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROFILES = ("dx11", "spv", "glsl", "essl", "mtl")


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def power_function(source, legacy):
    if legacy:
        match = re.search(r"float\s+highlight\s*=\s*(pow\(max\(dot\(N,H\),0\.0\),shine\))", source)
        if not match:
            raise ValueError("legacy source does not contain the original directional lobe")
        original = match.group(1)
        expression = original.replace("max(dot(N,H),0.0)", "base").replace("shine", "exponent")
        return ("float classic_lobe_power(float base, float exponent)\n{\n"
                "    return " + expression + ";\n}\n"), original
    match = re.search(r"float\s+classic_lobe_power\s*\([^)]*\)\s*\{", source)
    if not match:
        raise ValueError("fixed source lacks classic_lobe_power")
    depth = 1
    end = match.end()
    while end < len(source) and depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if depth:
        raise ValueError("unbalanced production power helper")
    if source.count("classic_lobe_power(") != 4:
        raise ValueError("all three shine lobes must consume the one helper")
    return source[match.start():end] + "\n", None


def stage_source(source_path, directory, legacy):
    # One byte snapshot binds helper extraction, compiled source and identity,
    # even if an author saves the live file during evidence collection.
    data = Path(source_path).read_bytes()
    function, original = power_function(data.decode("utf-8"), legacy)
    (Path(directory) / "fs_classic_lit.sc").write_bytes(data)
    return function, original, hashlib.sha256(data).hexdigest()


def witness(function):
    # Runtime uniforms avoid compile-time folding of the zero exponent.
    return """$input v_texcoord0
#include <bgfx_shader.sh>
uniform vec4 u_powerBases;
uniform vec4 u_powerExponents;
""" + function + """
void main()
{
    float base = u_powerBases.x;
    float exponent = u_powerExponents.x;
    if (v_texcoord0.x >= 0.75) {
        base = u_powerBases.w;
        exponent = u_powerExponents.w;
    } else if (v_texcoord0.x >= 0.5) {
        base = u_powerBases.z;
        exponent = u_powerExponents.z;
    } else if (v_texcoord0.x >= 0.25) {
        base = u_powerBases.y;
        exponent = u_powerExponents.y;
    }
    float value = classic_lobe_power(base, exponent);
    gl_FragColor = vec4(value, value, value, 1.0);
}
"""


def execute(command, output, commands):
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=180)
    output.write_text(result.stdout + result.stderr, encoding="utf-8", newline="\n")
    commands.append({"argv": list(map(str, command)), "exit_code": result.returncode,
                     "log": str(output), "log_sha256": sha(output)})
    if result.returncode:
        raise RuntimeError("command failed; see " + str(output))
    return result.stdout + result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--shaderc", type=Path, required=True)
    parser.add_argument("--bgfx-include", type=Path, required=True)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ninja", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--legacy", action="store_true")
    args = parser.parse_args()
    if args.jobs < 1 or args.jobs > 8:
        parser.error("jobs must be 1..8")
    if args.out.exists() and any(args.out.iterdir()):
        parser.error("out must be empty; preserve existing evidence in another directory")
    if not args.shaderc.is_file() or not (args.bgfx_include / "bgfx_shader.sh").is_file():
        parser.error("a runnable host shaderc and its matching bgfx include are required")
    args.out.mkdir(parents=True, exist_ok=True)
    args.out = args.out.resolve()
    sources = args.out / "source"
    sources.mkdir(exist_ok=True)
    function, original, source_sha256 = stage_source(args.source, sources, args.legacy)
    shutil.copyfile(ROOT / "engine/shaders/pbr/varying_pbr.def.sc", sources / "varying.def.sc")
    shutil.copyfile(ROOT / "engine/shaders/postfx/vs_postfx.sc", sources / "vs_classic_power_witness.sc")
    (sources / "fs_classic_power_witness.sc").write_text(witness(function), encoding="utf-8", newline="\n")
    manifest = {
        "schema": 1, "status": "UNMEASURED", "mode": "legacy" if args.legacy else "fixed",
        "source_path": str(args.source.resolve()), "source_sha256": source_sha256,
        "shaderc": str(args.shaderc.resolve()), "shaderc_sha256": sha(args.shaderc),
        "bgfx_include": str(args.bgfx_include.resolve()),
        "recipe": "cmake/JCESDKHelpers.cmake jce_add_project_shaders -> tools/compile_shaders.cmake",
        "profiles": list(PROFILES), "optimisation": 3,
        "defines": ["BGFX_CONFIG_MAX_BONES=128"],
        "witness_helper": {"provenance": "legacy production expression with argument renaming" if args.legacy
                            else "exact production helper definition", "legacy_expression": original,
                            "definition_sha256": hashlib.sha256(function.encode()).hexdigest()},
        "gpu_witness": {"status": "NOT_RUN", "vertex": "vs_classic_power_witness",
                        "fragment": "fs_classic_power_witness", "target": "raw RGBA32F recommended; retain non-finite values",
                        "u_powerBases": [0.0, 0.5, 0.0, 0.5], "u_powerExponents": [0.0, 0.0, 32.0, 4.0],
                        "quarters": "left to right at x=0.125,0.375,0.625,0.875", "expected_fixed_red": [1.0,1.0,0.0,0.0625]},
        "commands": [], "limitations": ["compile portability is not GPU numeric correctness",
            "legacy witness is a source-derived formula diagnostic, not the full material",
            "production material and actual driver outcomes require separate GPU evidence"]}
    try:
        manifest["shaderc_version"] = execute([str(args.shaderc), "--version"], args.out / "shaderc-version.log", manifest["commands"])
        configure = [args.cmake, "-S", str(ROOT / "tests/cmake/project_shaders"),
                     "-B", str(args.out / "build"), "-G", "Ninja",
                     "-DCMAKE_MAKE_PROGRAM=" + str(args.ninja.resolve()),
                     "-DJCE_SOURCE_DIR=" + str(ROOT),
                     "-DJCE_SHADERC_EXECUTABLE=" + str(args.shaderc.resolve()),
                     "-DBGFX_SHADER_INCLUDE_PATH=" + str(args.bgfx_include.resolve()),
                     "-DJCE_SHADER_PROFILES=" + ";".join(PROFILES),
                     "-DPROJECT_SHADER_SOURCE_DIR=" + str(sources),
                     "-DPROJECT_SHADER_INCLUDE_DIR=" + str(ROOT / "engine/shaders/pbr") + ";" + str(ROOT / "engine/shaders"),
                     "-DPROJECT_SHADER_OUTPUT_REGEX=/assets/shaders/(fs_classic_lit|fs_classic_power_witness|vs_classic_power_witness)_(dx11|spv|glsl|essl|mtl)\\.bin$",
                     "-DJCE_SHADER_LINT_PYTHON=" + sys.executable]
        execute(configure, args.out / "configure.log", manifest["commands"])
        execute([args.cmake, "--build", str(args.out / "build"), "--target", "FixtureShadersAll", "--parallel", str(args.jobs)],
                args.out / "compile.log", manifest["commands"])
        blobs = args.out / "build/assets/shaders"
        expected = [(name + "_" + profile + ".bin") for name in
                    ("fs_classic_lit", "fs_classic_power_witness", "vs_classic_power_witness") for profile in PROFILES]
        if sorted(p.name for p in blobs.glob("*.bin")) != sorted(expected):
            raise RuntimeError("compiled output set does not match all requested profiles")
        manifest["outputs"] = {name: {"sha256": sha(blobs / name), "bytes": (blobs / name).stat().st_size}
                               for name in expected}
        manifest["source_snapshots"] = {p.name: sha(p) for p in sorted(sources.iterdir())}
        manifest["status"] = "COMPILE_PORTABILITY_PASS"
    except Exception as error:
        manifest["status"] = "FAILED"
        manifest["error"] = str(error)
        raise
    finally:
        (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"status": manifest["status"], "manifest": str(args.out / "manifest.json"),
                      "gpu_numeric_validation": "NOT_RUN"}))


if __name__ == "__main__":
    main()
