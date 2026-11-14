"""Prepare canonical desktop presets for native IDE Configure/Build/Debug.

Observed tool paths stay in the existing local cache and VS Code settings.
Preparation/configuration is separate from a native build or IDE execution.
"""
from __future__ import annotations

from contextlib import nullcontext
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from tools.build import ide_jsonc as local
from tools.build import ide_cache as cache

def _tool_paths(driver, env, target):
    path = next((value for key, value in env.items() if key.upper() == "PATH"), "")
    result = {}
    for name in ("cl", "link"):
        executable = shutil.which(name, path=path)
        if not executable:
            raise ValueError("IDE preparation cannot find " + name +
                " in the shared MSVC environment. Install the matching C++ build tools "
                "and make CMake, Ninja and Conan available to scripts/jce.py.")
        result[name] = Path(executable).resolve().as_posix()
    if not driver._msvc_env_ready(env):
        raise ValueError("IDE preparation needs cl, INCLUDE and LIB from the shared MSVC owner")
    expected_arch = driver._ARCH_CLDIR[target["arch"]]
    environment_arch = next((value.lower() for key, value in env.items()
                             if key.upper() == "VSCMD_ARG_TGT_ARCH"), None)
    if environment_arch is not None and environment_arch != expected_arch:
        raise ValueError("MSVC developer-shell target " + environment_arch + " differs from " +
                         expected_arch + "; rerun scripts/jce.py ide from a normal CMD")
    for name in ("cl", "link"):
        suffix = re.search(r"[/\\]bin[/\\]Host(?:x64|x86|arm64)[/\\]([^/\\]+)[/\\]" +
                           name + r"(?:\.exe)?$", result[name], re.IGNORECASE)
        if suffix is None or suffix.group(1).lower() != expected_arch:
            raise ValueError("MSVC " + name + " path does not match target " + expected_arch +
                             "; rerun scripts/jce.py ide from a normal CMD")
    # A developer shell for a newer VS can otherwise bypass msvc_env's activation.
    # This is a path/profile admission, not an executed compiler ABI certificate.
    match = re.search(r"[/\\]MSVC[/\\]([0-9.]+)[/\\]", result["cl"], re.IGNORECASE)
    if match and not match.group(1).startswith(driver._VC_TOOLSET_PREFIX):
        raise ValueError("selected MSVC " + match.group(1) + " does not match the shared "
            "Conan profile family " + driver._VC_TOOLSET_PREFIX +
            "; run this command from a normal CMD and install that toolset if absent. "
            "The Visual Studio IDE version does not change the repository's Conan profile.")
    selected_vs = None
    bundled = {"cmake": "CMake/bin/cmake.exe", "ninja": "Ninja/ninja.exe"}
    for name in ("cmake", "ninja", "conan"):
        executable = shutil.which(name, path=path)
        if not executable and name in bundled:
            if selected_vs is None:
                observed = next((value for key, value in env.items()
                                 if key.upper() == "VSINSTALLDIR"), None)
                installed = Path(observed) if observed else driver.find_vs_install(target["arch"])
                if installed is not None:
                    selected_vs = Path(installed).resolve()
                    if not Path(result["cl"]).is_relative_to(selected_vs / "VC/Tools/MSVC"):
                        raise ValueError("VS bundled build tools belong to a different compiler installation")
            if selected_vs is not None:
                candidate = selected_vs / "Common7/IDE/CommonExtensions/Microsoft/CMake" / bundled[name]
                if candidate.is_file():
                    executable = str(candidate.resolve())
                    directory = str(candidate.parent)
                    path = directory + os.pathsep + path
                    path_key = next((key for key in env if key.upper() == "PATH"), "PATH")
                    env[path_key] = path
                    driver.log("IDE prerequisite from selected VS installation: " + executable)
        if not executable:
            raise ValueError("IDE preparation cannot find " + name +
                " in PATH or the selected Visual Studio CMake tools. "
                "Install that C++ CMake component, and keep Conan available to scripts/jce.py.")
        result[name] = Path(executable).resolve().as_posix()
    for name in ("cmake", "ninja", "conan"):
        completed = subprocess.run([result[name], "--version"], env=env,
            cwd=driver.ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=30, text=True, encoding="utf-8", errors="replace")
        if completed.returncode:
            raise ValueError("IDE prerequisite cannot execute: " + result[name])
        lines = completed.stdout.splitlines()
        driver.log("IDE prerequisite: " + (lines[0] if lines else name + " --version OK"))
    return result


def _admit_preset(driver, name):
    from tools.build.layout_contract import check_presets
    raw = local.read_file(driver.ROOT, "CMakePresets.json")
    if raw is None:
        raise ValueError("source-root CMakePresets.json is missing")
    data = json.loads(raw.decode("utf-8-sig"))
    errors = check_presets(driver.ROOT, data)
    if errors:
        raise ValueError("preset layout disagrees with the shared owner: " + errors[0])
    names = {entry["name"] for entry in data["configurePresets"]}
    if name not in names or "jce-ide-" + name in names:
        raise ValueError("IDE preset is missing or conflicts with a repository preset")
    return data


def _retire_presets(before, repository):
    """Hide unchanged historical aliases; never create a second visible lane."""
    if before is None:
        return None, []
    # CMake accepts strict JSON. The bounded JSONC parser also rejects duplicate
    # keys that json.loads alone would silently coalesce.
    data = json.loads(before.decode("utf-8-sig"))
    local.Document(before)
    if (not isinstance(data, dict) or type(data.get("version")) is not int
            or data["version"] < 1):
        raise ValueError("CMakeUserPresets.json needs an integer schema version")
    for field in ("configurePresets", "buildPresets"):
        entries = data.get(field, [])
        if not isinstance(entries, list):
            raise ValueError("CMakeUserPresets.json requires array: " + field)
        names = {entry["name"] for entry in repository.get(field, [])}
        for entry in entries:
            if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
                raise ValueError("CMakeUserPresets.json has an invalid " + field + " entry")
            if entry["name"] in names:
                raise ValueError("duplicate CMake preset name across repository/user files: " + entry["name"])
            names.add(entry["name"])
    raw, retired, preserved = before, set(), []
    canonical = {entry["name"] for entry in repository["configurePresets"]}
    for field, prefix in (("configurePresets", "jce-ide-"),
                          ("buildPresets", "build-jce-ide-")):
        for entry in data.get(field, []):
            name = entry["name"]
            if not name.startswith(prefix):
                continue
            source = name.removeprefix(prefix)
            known = (source in canonical and entry.get("inherits") == source
                     if field == "configurePresets" else
                     entry.get("configurePreset") == "jce-ide-" + source
                     and "jce-ide-" + source in retired)
            if not known or not local.owned_entry(entry):
                preserved.append(name)
                continue
            retired.add(name)
            if entry.get("hidden") is not True:
                updated = local.stamp_entry(dict(entry, hidden=True))
                raw = local.named_entry(raw, field, "name", updated, replace_owned=True)
    json.loads(raw.decode("utf-8-sig"))
    return raw, preserved


def _legacy_vscode_entries():
    """Exact R14/R15 output shapes, only for byte-preserving guarded migration."""
    label = "JCE: Build native Editor (release)"
    tasks, launches = [], []
    for arch, key in (("x86", "windows-x86"), ("x64", "windows-x64"),
                      ("arm64", "windows-arm64")):
        tasks.append({"label": label, "type": "process", "command": "py",
            "args": ["-3", "${workspaceFolder}/scripts/jce.py", "editor", "--arch", arch,
                     "--variant", "release"],
            "options": {"cwd": "${workspaceFolder}"}, "problemMatcher": "$msCompile",
            "group": {"kind": "build", "isDefault": False}})
        launches.append({"name": "JCE Editor (Native Release)", "type": "cppvsdbg", "request": "launch",
            "program": "${workspaceFolder}/build/desktop/" + key + "/release/jce_editor.exe",
            "args": [], "cwd": "${workspaceFolder}", "console": "integratedTerminal",
            "preLaunchTask": label})
    return tasks, launches


def _vscode_changes(root, target, tools, shader_settings):
    changes = {}

    def stage(relative, transform, default):
        before = local.read_file(root, relative)
        after = transform(before if before is not None else default)
        changes[relative] = (before, after)

    values = {"cmake.useCMakePresets": "always", "cmake.configureOnOpen": False,
              "cmake.useVsDeveloperEnvironment": "always",
              "cmake.buildBeforeRun": True,
              "cmake.sourceDirectory": "${workspaceFolder}", "cmake.cmakePath": tools["cmake"]}
    values.update(shader_settings)
    stage(".vscode/settings.json", lambda raw: local.set_object(raw, values), b"{}\n")

    def cpp(raw):
        doc = local.Document(raw)
        configurations = doc.root.members.get("configurations")
        if configurations is None or not isinstance(configurations.value, list):
            raise ValueError("c_cpp_properties.json requires a configurations array")
        found = [node for node in configurations.items if isinstance(node.value, dict)
                 and node.value.get("name") == "Win32"]
        if len(found) != 1:
            raise ValueError("c_cpp_properties.json requires exactly one Win32 configuration")
        return doc.encode(doc.set_values(found[0], {"configurationProvider": "ms-vscode.cmake-tools"}))

    stage(".vscode/c_cpp_properties.json", cpp,
          b'{"version":4,"configurations":[{"name":"Win32"}]}\n')
    legacy_tasks, legacy_launches = _legacy_vscode_entries()
    task = {"label": "JCE: Build active CMake preset", "type": "cmake", "command": "build",
            "preset": "${command:cmake.activeBuildPresetName}", "problemMatcher": "$msCompile",
            "group": {"kind": "build", "isDefault": False}}
    stage(".vscode/tasks.json", lambda raw: local.migrate_named_entry(
              raw, "tasks", "label", task, legacy_tasks),
          b'{"version":"2.0.0","tasks":[]}\n')
    # launchTargetPath owns the one build-before-debug operation. The getter
    # only resolves the matching target directory; no preLaunchTask builds twice.
    launch = {"name": "JCE: Debug selected CMake target", "type": "cppvsdbg", "request": "launch",
              "program": "${command:cmake.launchTargetPath}", "args": [],
              "cwd": "${command:cmake.getLaunchTargetDirectory}", "externalConsole": False}
    stage(".vscode/launch.json", lambda raw: local.migrate_named_entry(
              raw, "configurations", "name", launch, legacy_launches),
          b'{"version":"0.2.0","configurations":[]}\n')
    return changes


def _verify_configuration(driver, target, args, tree, tools, toolchain):
    cache = local.read_file(driver.ROOT, (tree / "CMakeCache.txt").relative_to(driver.ROOT).as_posix())
    if cache is None:
        raise ValueError("CMake reported success without a CMakeCache.txt")
    expected = {"CMAKE_HOME_DIRECTORY": driver.ROOT.as_posix(),
                "CMAKE_TOOLCHAIN_FILE": toolchain.as_posix(),
                "CMAKE_MAKE_PROGRAM": tools["ninja"],
                "CMAKE_C_COMPILER": tools["cl"], "CMAKE_CXX_COMPILER": tools["cl"]}
    values = {}
    keys = set(expected) | {"CMAKE_GENERATOR", "CMAKE_BUILD_TYPE", "JCE_BUILD_EDITOR",
                            "JCE_BUILD_VARIANT", "JCE_DELIVERY_PROFILE"}
    for line in cache.decode("utf-8-sig").splitlines():
        if line.startswith(("#", "//")) or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.split(":", 1)[0]
        if key in keys:
            if key in values:
                raise ValueError("duplicate configured cache field: " + key)
            values[key] = value
    for key, expected_path in expected.items():
        if not values.get(key) or Path(values[key]).resolve() != Path(expected_path).resolve():
            raise ValueError("configured cache differs from the IDE request: " + key)
    expected_values = {"CMAKE_GENERATOR": "Ninja",
        "CMAKE_BUILD_TYPE": "Debug" if args.variant == "debug" else "Release",
        "JCE_BUILD_EDITOR": "ON", "JCE_BUILD_VARIANT": args.variant,
        "JCE_DELIVERY_PROFILE": "authoring"}
    for key, value in expected_values.items():
        if values.get(key) != value:
            raise ValueError("configured cache differs from the IDE request: " + key)
    for filename in ("build.ninja", "compile_commands.json"):
        if not (tree / filename).is_file():
            raise ValueError("CMake reported success without expected output: " + str(tree / filename))


def _shader_settings(driver, args):
    if not args.shader_build_dir:
        return {}, "NOT_REQUESTED"
    from tools.build.ide_modkit import resolve_settings
    build = Path(args.shader_build_dir)
    if not build.is_absolute():
        build = driver.ROOT / build
    try:
        settings = resolve_settings(driver.ROOT, build)
        executable = Path(settings["modkit.shaderc.path"])
        if not driver._shaderc_runs(executable):
            raise ValueError("shaderc --version could not execute: " + str(executable))
        return settings, "LOCATED_AND_VERSION_PROBED_NOT_EDITOR_VALIDATED"
    except (OSError, ValueError) as exc:
        driver.log("WARN: Modkit configuration unavailable: " + str(exc))
        target = driver.resolve_target(args.arch)
        driver.log("CMake preparation remains valid. Configure/Build JCE_Editor with preset " +
                   driver.preset_name(target, args.variant) + ", then rerun ide --vscode "
                   "--arch " + target["eng_arch"] + " --variant " + args.variant +
                   " --shader-build-dir " +
                   driver.preset_binary_dir(target, args.variant).relative_to(driver.ROOT).as_posix() +
                   ". Existing Modkit settings were kept.")
        return {}, "UNAVAILABLE"


def prepare(driver, args):
    rebuild_dependencies = driver.conan_rebuild_names(getattr(args, "rebuild_dependency", ()) or ())
    conan_options = {"rebuild_dependencies": rebuild_dependencies} if rebuild_dependencies else {}
    if getattr(args, "rollback_local", None):
        if (args.vscode or args.shader_build_dir or args.prepare_only or args.arch or
                args.variant != "release" or rebuild_dependencies):
            raise ValueError("--rollback-local cannot be combined with preparation options")
        metadata_resources = cache.restore_resources(driver.ROOT, args.rollback_local)
        resources = [driver.ROOT / "CMakeUserPresets.json", driver.ROOT / ".vscode", *metadata_resources]
        scope = nullcontext() if driver.DRY_RUN else driver._cache_scope(resources)
        with scope:
            restore = cache.restore if metadata_resources else local.restore
            result = restore(driver.ROOT, args.rollback_local, dry_run=driver.DRY_RUN)
        driver.log("IDE " + result["status"] + ": " + str(result["files"]) + " local files; receipt " + result["backup"])
        return
    if driver.HOST != "windows":
        raise ValueError("IDE preparation currently supports a native Windows MSVC host")
    target = driver.resolve_target(args.arch)
    if target.get("cross") or target.get("emscripten"):
        raise ValueError("IDE preparation requires the native host architecture")
    if args.shader_build_dir and not args.vscode:
        raise ValueError("--shader-build-dir requires --vscode")
    name = driver.preset_name(target, args.variant)
    repository = _admit_preset(driver, name)
    tree = driver.preset_binary_dir(target, args.variant)
    resources = [tree, driver.conan_dir(target), driver.ROOT / "CMakeUserPresets.json"]
    if args.vscode:
        resources.append(driver.ROOT / ".vscode")
    scope = nullcontext() if driver.DRY_RUN else driver._cache_scope(resources)
    with scope:
        env = dict(driver.msvc_env(target))
        env.setdefault("CMAKE_POLICY_VERSION_MINIMUM", "3.5")
        env.setdefault("CMAKE_BUILD_PARALLEL_LEVEL", driver.build_jobs(env))
        if driver.DRY_RUN:
            driver.log("IDE DRY RUN: prepare " + name + " with the shared Conan/MSVC owners")
            driver.conan_install(target, "Debug" if args.variant == "debug" else "Release", env, **conan_options)
            driver.log("IDE DRY RUN: retain canonical preset; no configuration files changed. "
                       "Observed compiler/Ninja paths are admitted during real preparation.")
            if not args.prepare_only:
                driver.run(["cmake", "--preset", name], env=env, cwd=driver.ROOT)
            return
        tools = _tool_paths(driver, env, target)
        flags = {"JCE_BUILD_EDITOR": "ON", "JCE_BUILD_VARIANT": args.variant}
        for flag in driver._delivery_module().cmake_flags("authoring",
                restore_authoring=driver._delivery_module().restore_authoring(tree / "CMakeCache.txt")):
            key, value = flag.removeprefix("-D").split("=", 1)
            flags[key] = value
        before = local.read_file(driver.ROOT, "CMakeUserPresets.json")
        after, preserved = _retire_presets(before, repository)
        for entry in preserved:
            driver.log("IDE preserved custom or modified historical preset: " + entry +
                       "; select the canonical preset " + name)
        # Validate every local JSONC input before dependency generation or writes.
        changes = {"CMakeUserPresets.json": (before, after)}
        if args.vscode:
            changes.update(_vscode_changes(driver.ROOT, target, tools, {}))
        config = "Debug" if args.variant == "debug" else "Release"
        expected_toolchain = driver.toolchain_path(target, config)
        metadata = cache.Recovery(driver.ROOT, tree, expected_toolchain, tools["cl"])
        toolchain = driver.conan_install(target, config, env, **conan_options)
        if toolchain.resolve() != expected_toolchain.resolve():
            raise ValueError("Conan returned another IDE toolchain path")
        if not toolchain.is_file():
            raise ValueError("shared Conan owner did not generate the selected toolchain: " + str(toolchain))
        shader_status = "NOT_REQUESTED"
        if args.vscode:
            settings, shader_status = _shader_settings(driver, args)
            key = ".vscode/settings.json"
            previous, planned = changes[key]
            changes[key] = (previous, local.set_object(planned, settings))
        transaction = local.Transaction(driver.ROOT, changes)
        metadata_applied = settings_applied = False
        try:
            metadata.apply()
            metadata_applied = True
            if metadata.changes:
                driver.log("IDE CMake metadata backup: " + str(metadata.backup))
                for reason in metadata.reasons:
                    driver.log("IDE CMake metadata refresh: " + reason)
            transaction.apply()
            settings_applied = True
            if not args.prepare_only:
                variables = {"CMAKE_MAKE_PROGRAM": tools["ninja"],
                             "CMAKE_C_COMPILER": tools["cl"], "CMAKE_CXX_COMPILER": tools["cl"]}
                variables.update(flags)
                driver.run([tools["cmake"], "--preset", name] +
                           ["-D" + key + "=" + value for key, value in variables.items()],
                           env=env, cwd=driver.ROOT)
                _verify_configuration(driver, target, args, tree, tools, toolchain)
            metadata.commit(configured=not args.prepare_only)
            transaction.commit()
        except BaseException as failure:
            conflicts = []
            for applied, owner in ((settings_applied, transaction), (metadata_applied, metadata)):
                if applied:
                    try:
                        owner.rollback()
                    except (OSError, ValueError) as error:
                        conflicts.append(str(error) + "; backup " + str(owner.backup))
            if conflicts:
                raise ValueError("IDE preparation failed (" + str(failure) +
                    "); recovery preserved newer files: " + " | ".join(conflicts)) from failure
            raise
        status = "PREPARED_NOT_CONFIGURED" if args.prepare_only else "CONFIGURED_NOT_BUILT"
        driver.log("IDE " + status + ": " + name)
        if metadata.changes:
            driver.log('Undo retired CMake metadata: py -3 scripts/jce.py ide --rollback-local "' + str(metadata.backup) + '"')
        if transaction.changes:
            driver.log("IDE local configuration backup: " + str(transaction.backup))
            driver.log('Undo local IDE files: py -3 scripts/jce.py ide --rollback-local "' + str(transaction.backup) + '"')
        if args.vscode:
            driver.log("VS Code local settings prepared; Modkit=" + shader_status)
        if args.prepare_only:
            driver.log("Dependencies/settings are prepared; run native IDE Configure next. "
                       "An existing embedded cache is restored to authoring only by ide without --prepare-only.")
        driver.log("Select Configure Preset '" + name + "' and Build Preset 'build-" + name +
                   "' in Visual Studio / VS Code; use native Configure/Build and select JCE_Editor to debug. "
                   "This is the authoring lane; standalone single-executable delivery remains scripts/jce.py editor.")
