#!/usr/bin/env python3
"""Check native project layout authority and its manager/UI consumption.

This is a source wiring gate. Actual helper behavior is tested by
 test_jce_project_build_target and test_jce_build_package; this cannot certify
 a GUI action, compiler output or successful publication.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Mapping

ROOT = Path(__file__).resolve().parents[2]
OWNERS = {
    "manager": "editor/src/core/jce_build_manager.cpp",
    "panel": "editor/src/panels/jce_panel_build_profiles.cpp",
    "target": "editor/src/core/jce_project_build_target.cpp",
    "header": "editor/src/core/jce_project_build_target.h",
    "package": "editor/src/core/jce_build_package.cpp",
}
# Strings/characters are single tokens; comment text cannot act as a caller.
_TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/|\s+|[A-Za-z_]\w*|[0-9]+|[^\s]')


def tokens(source: str) -> tuple[str, ...]:
    return tuple(m.group() for m in _TOKEN.finditer(source)
                 if not m.group().isspace() and not m.group().startswith(("//", "/*")))


def occurrences(body: tuple[str, ...], fragment: str) -> list[int]:
    needle = tokens(fragment)
    return [i for i in range(len(body) - len(needle) + 1)
            if body[i:i + len(needle)] == needle]


def closing(body: tuple[str, ...], start: int, left: str, right: str) -> int:
    depth = 0
    for i in range(start, len(body)):
        if body[i] == left:
            depth += 1
        elif body[i] == right:
            depth -= 1
            if depth == 0:
                return i
    raise ValueError(f"unclosed {left}")


def function_body(source: str, name: str) -> tuple[str, ...]:
    stream = tokens(source)
    bodies = []
    for i, token in enumerate(stream[:-1]):
        if token != name or stream[i + 1] != "(":
            continue
        end = closing(stream, i + 1, "(", ")")
        if end + 1 < len(stream) and stream[end + 1] == "{":
            stop = closing(stream, end + 1, "{", "}")
            bodies.append(stream[end + 2:stop])
    if len(bodies) != 1:
        raise ValueError(f"expected one definition of {name}, found {len(bodies)}")
    return bodies[0]


def containing_block(body: tuple[str, ...], anchor: str) -> tuple[str, ...]:
    hits = occurrences(body, anchor)
    if len(hits) != 1:
        raise ValueError(f"expected one request anchor: {anchor}")
    candidates = []
    for i, token in enumerate(body):
        if token == "{":
            stop = closing(body, i, "{", "}")
            if i < hits[0] < stop:
                candidates.append(body[i + 1:stop])
    if not candidates:
        raise ValueError(f"no request block around: {anchor}")
    return min(candidates, key=len)


def inspect_sources(sources: Mapping[str, str]) -> list[str]:
    failures: list[str] = []
    cache: dict[tuple[str, str], tuple[str, ...]] = {}

    def body(owner: str, function: str) -> tuple[str, ...]:
        key = owner, function
        if key not in cache:
            try:
                cache[key] = function_body(sources[owner], function)
            except (KeyError, ValueError) as error:
                failures.append(f"{OWNERS[owner]}: {error}")
                cache[key] = ()
        return cache[key]

    def need(stream: tuple[str, ...], fragment: str, label: str) -> None:
        if not occurrences(stream, fragment):
            failures.append(label)

    # These are the existing path owners, not another path implementation.
    need(body("target", "jce_project_build_root"),
         'return root + "build/" + jce_project_build_platform_tag(platform) + "-" + arch;',
         "build root must use the selected platform/arch without a variant")
    need(body("target", "jce_project_build_output_dir"),
         'return build_root + "/" + variant;', "variant output must remain inside the build root")
    need(body("header", "jce_project_build_variant"),
         'return dist ? "dist" : "release";', "UI Dist selection must determine the requested variant")
    need(body("target", "jce_project_build_exe_name"),
         'std::string name = (configured && configured[0]) ? std::string(configured) : target;',
         "artifact naming must honor the edited executable with target fallback")
    need(body("target", "jce_project_build_exe_name"),
         'if (!has_exe_suffix(name)) name += ".exe";', "Windows executable suffix must use the naming owner")
    need(body("package", "jce_build_package_default_path"),
         'return normalize(root + "/dist/apps/" + app + "/" + platform + "-" + arch + "-" + variant, false, out, error);',
         "final package must use the normalized dist/apps app/platform-arch-variant layout")

    start = body("manager", "jce_build_manager_start_project_build")
    for fragment, label in (
        ('const std::string arch = selected_arch;', "manager must consume the admitted selected architecture"),
        ('const std::string variant = (cfg->variant && cfg->variant[0]) ? std::string(cfg->variant) : "release";', "manager must consume the requested variant"),
        ('exe_name = jce_project_build_exe_name(target, cfg->exe_name);', "manager must consume edited artifact naming"),
        ('build_dir = jce_project_build_root(project, build_platform, arch);', "manager must call the shared build root owner"),
        ('output_dir = jce_project_build_output_dir(build_dir, variant);', "manager must call the shared variant output owner"),
        ('reports_dir = project_reports_dir(build_dir);', "reports must stay in the build tree"),
        ('project_intermediate_dir(build_dir, variant)', "intermediates must stay in the selected build tree"),
        ('jce_build_package_admit(project, sdk,', "package output must pass the existing ownership admission"),
        ('p.build_dir = build_dir;', "build root must reach the queued request"),
        ('p.output_dir = output_dir;', "variant output must reach the queued request"),
        ('p.archive_dir = archive_dir;', "archive cache must reach the queued request"),
        ('p.exe_name = exe_name;', "edited artifact name must reach the queued request"),
        ('p.variant = variant;', "selected variant must reach the queued request"),
        ('p.package_out_dir = package_out_dir;', "admitted package output must reach its separate queued field"),
    ):
        need(start, fragment, label)
    finalize = body("manager", "finalize_project_build_pipeline")
    for fragment, label in (
        ('a += " -B " + qtok(p.build_dir);', "configure must use the build cache root"),
        ('a += " -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=" + qtok(p.output_dir);', "runtime output must use the variant build output"),
        ('a += " -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=" + qtok(p.output_dir);', "library output must use the variant build output"),
        ('a += " -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=" + qtok(p.archive_dir);', "archive output must use its build cache directory"),
        ('g_finish.artifact_a = p.output_dir + PATH_SEP_CHR_LOCAL + p.exe_name;', "artifact verification must follow actual variant output and executable name"),
        ('g_finish.out_dir = p.package_out_dir;', "publication destination must remain separate from the build output"),
    ):
        need(finalize, fragment, label)
    need(body("manager", "run_finish_plan"),
         'jce_build_package_publish_dist(exe, g_finish.package_reports_dir, out, g_finish.exe_name, audit, nullptr, &package_error)',
         "Dist publication must consume the existing verified publisher")

    preview = body("panel", "guess_output_exe")
    for fragment, label in (
        ('target = s_bp.build_target[0] ? s_bp.build_target :', "UI preview must consume edited target"),
        ('exe = s_bp.build_exe[0] ? s_bp.build_exe : jp->output_exe;', "UI preview must consume edited executable"),
        ('arch_sel = s_arch_cli[', "UI preview must consume architecture selection"),
        ('? s_bp.arch_idx : 0]', "UI preview must index the selected architecture"),
        ('arch = arch_sel ? arch_sel : host_arch_triplet();', "UI preview must use selected architecture with host fallback"),
        ('return jce_project_build_output_dir(jce_project_build_root("", jce_project_build_host_platform(), arch), jce_project_build_variant(s_bp.use_dist)) + "/" + jce_project_build_exe_name(target, exe);',
         "UI preview must consume the same root/output/variant/executable owners as the manager"),
    ):
        need(preview, fragment, label)
    draw = body("panel", "draw_profiles_tab")
    need(draw, 'predicted = guess_output_exe(p.preset);', "visible build preview must consume the actual prediction")
    need(draw, 'jce_build_package_default_path(s_bp.project_root, jp->name && jp->name[0] ? jp->name : "game", jce_project_build_package_platform(jce_project_build_host_platform()), arch, jce_project_build_variant(s_bp.use_dist), &package_path, &path_error)',
         "visible package preview must use the shared app/arch/variant publication owner")
    try:
        package_preview = containing_block(draw, 'std::string package_path, path_error;')
        need(package_preview, '? s_arch_cli[s_bp.arch_idx] : host_arch_triplet();',
             "visible package preview must consume the selected architecture")
    except ValueError as error:
        failures.append(f"visible package preview: {error}")
    need(draw, 'package_path += "/" + predicted.substr(slash == std::string::npos ? 0 : slash + 1);',
         "visible package preview must use the predicted executable basename")
    for anchor, label in (('pcfg.label = p.preset.c_str();', "native build"),
                          ('pcfg.label = "package-game";', "native package")):
        try:
            request = containing_block(draw, anchor)
        except ValueError as error:
            failures.append(f"{label}: {error}")
            continue
        for fragment in ('pcfg.target = tgt;', 'pcfg.exe_name = exe;',
                         'pcfg.variant = jce_project_build_variant(s_bp.use_dist);',
                         'jce_build_manager_start_project_build(&pcfg)'):
            need(request, fragment, f"{label}: edited request must consume {fragment}")
        need(request, 'tgt = s_bp.build_target[0] ? s_bp.build_target :', f"{label}: target edit must reach request")
        need(request, 'exe = s_bp.build_exe[0] ? s_bp.build_exe :', f"{label}: executable edit must reach request")
        if label == "native build":
            need(request, 'pcfg.arch = arch;', "native build: selected architecture must reach request")
            need(request, '? s_arch_cli[s_bp.arch_idx] : nullptr;', "native build: architecture edit must reach request")
            need(request, 'pcfg.package_out_dir = nullptr;', "plain build must not publish into a package directory")
        else:
            need(request, '? s_arch_cli[s_bp.arch_idx] : host_arch_triplet();',
                 "native package: destination architecture must use the selected architecture")
            need(request, 'pcfg.arch = (s_bp.arch_idx > 0) ? s_arch_cli[s_bp.arch_idx] : nullptr;',
                 "native package: architecture edit must reach request")
            need(request, 'jce_build_package_default_path(root, name, plat, arch_cli, jce_project_build_variant(s_bp.use_dist), &out_dir, &path_error)',
                 "native package: selected app/arch/variant must reach the shared publication owner")
            need(request, 'pcfg.package_out_dir = out_dir.c_str();', "native package: default publication output must reach request")
    return failures


def load_sources() -> dict[str, str]:
    return {owner: (ROOT / path).read_text(encoding="utf-8") for owner, path in OWNERS.items()}


def main() -> int:
    try:
        failures = inspect_sources(load_sources())
    except (OSError, UnicodeError) as error:
        failures = [str(error)]
    if failures:
        for failure in failures:
            print(failure)
        return 1
    print("project build layout authority and manager/UI wiring OK (source gate)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
