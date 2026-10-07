"""Public bundle command composition; no asset/content graph implementation."""
from pathlib import Path


def command(args, executable):
    if not args.project and not (args.resource_root and args.out and
                                 (args.scenes_dir or args.scene)):
        raise ValueError("bundle requires --project or explicit scenes/resource/out paths")
    if args.single_file and len(args.scene or []) != 1:
        raise ValueError("--single-file requires exactly one --scene")
    if args.id and not args.single_file:
        raise ValueError("--id requires --single-file")
    result = [str(executable)]
    for option, value in (("--project", args.project), ("--scenes-dir", args.scenes_dir),
                          ("--resource-root", args.resource_root), ("--out", args.out)):
        if value:
            result += [option, str(Path(value).resolve())]
    for scene in args.scene or []:
        if not (scene.endswith(".scene") or scene.endswith(".scene.json")) or not Path(scene).is_file():
            raise ValueError("selected scene unavailable: " + scene)
        result += ["--scene", str(Path(scene).resolve())]
    result += ["--target", args.target]
    for name in ("cook_assets", "single_file", "quiet"):
        if getattr(args, name):
            result.append("--" + name.replace("_", "-"))
    if args.id:
        result += ["--id", args.id]
    return result


def execute(args, driver):
    # Validate before booting any compiler/cache. Explicit SDK must never fall
    # back to a source build, which would hide an incomplete installed SDK.
    command(args, "jce_bundle_pack")
    if args.sdk:
        sdk = Path(args.sdk).resolve()
        exe = sdk / "bin" / ("jce_bundle_pack.exe" if driver.HOST == "windows" else "jce_bundle_pack")
        if not exe.is_file():
            raise ValueError("installed SDK bundle tool unavailable: " + str(exe))
    else:
        exe = driver.ensure_host_tools({}, ("jce_bundle_pack",))["jce_bundle_pack"]
    driver.run(command(args, exe), cwd=driver.ROOT)
