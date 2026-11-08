"""Authenticated DIST assets for the installed-SDK regression consumer.

The fixture stages bounded declared inputs; the existing application builder
owns graph discovery, cooking, encryption, provenance and key-share generation.
This helper neither packages a second archive nor proves GUI/device delivery.
"""
from pathlib import Path
import importlib.util


def fixture_module():
    source = Path(__file__).resolve().parents[2] / "tests/sdk_smoke/stage_dist_fixture.py"
    spec = importlib.util.spec_from_file_location("jce_sdk_smoke_fixture", source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def prepare(driver, target, sdk, build_dir, environment, *, fixture="c99"):
    # The smoke lease owns only this SDK and scratch. Never acquire/rebuild an
    # alternative host cache or accept target binaries as build-host tools.
    if fixture not in ("c99", "scripting"):
        raise ValueError("unknown SDK smoke fixture")
    selected = driver._sdk_host_tools(sdk, target)
    if set(selected) != {"jce_cook", "jce_pak", "jce_bin2obj"}:
        raise ValueError("DIST smoke requires all three executable host tools in the selected SDK")
    suffix = ".exe" if driver.HOST == "windows" else ""
    folders = ("bin/host", "bin") if target.get("cross") or target.get("emscripten") else ("bin", "bin/host")
    bundle = next((Path(sdk) / folder / ("jce_bundle_pack" + suffix)
                   for folder in folders
                   if (Path(sdk) / folder / ("jce_bundle_pack" + suffix)).is_file()), None)
    if bundle is None or not driver._tool_runs(bundle):
        raise ValueError("DIST smoke requires an executable public Bundle resolver in the selected SDK")
    selected["jce_bundle_pack"] = bundle
    owner = fixture_module()
    source = "sdk_smoke" if fixture == "c99" else "sdk_smoke_scripting"
    project, manifest = owner.stage_dist_fixture(driver.ROOT / "tests" / source / "assets", build_dir, fixture=fixture)
    flags = driver._dist_prebuilt_assets(project, target, build_dir, manifest,
                                        environment, sdk, selected)
    owner.validate_dist_proof(build_dir, fixture=fixture)
    # The fixture has no loose-asset staging target. Keep application-only
    # flags out of its configure command rather than producing warning noise.
    return [flag for flag in flags if not flag.startswith("-DJCE_PROJECT_STAGE_LOOSE_ASSETS=")]


def verify(build_dir, *, fixture="c99"):
    return fixture_module().validate_dist_proof(build_dir, fixture=fixture)
