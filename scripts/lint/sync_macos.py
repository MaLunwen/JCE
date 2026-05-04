#!/usr/bin/env python3
"""Sync local changes to macOS host and run build-macos-x64.sh."""
import os
import sys
import posixpath

sys.path.insert(0, os.path.dirname(__file__))
from ssh_helper import SSHSession

HOST = "192.168.10.10"
USER = "lunwen"
PASSWORD = "1014"
REMOTE_ROOT = "/Users/lunwen/code/JCE"
LOCAL_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SYNC_FILES = [
    "scripts/lib/jce_beep.sh",
    "scripts/lib/jce_common.sh",
    "scripts/macos/build-macos-x64.sh",
    "scripts/macos/build-macos-arm64.sh",
    "scripts/macos/build-macos-universal.sh",
    "scripts/macos/build-host-tools.sh",
    "scripts/macos/build-editor.sh",
    "scripts/macos/build-ios-arm64.sh",
    "CMakeLists.txt",
    "engine/CMakeLists.txt",
    "editor/CMakeLists.txt",
    "conan/profiles/macos-x64",
    "conan/profiles/macos-arm64",
    "conan/profiles/ios-arm64",
    "conan/hooks/hook_cmake_policy_fix.py",
    "conan/hooks/hook_bgfx_wasm_fix.py",
    "conan/hooks/hook_arm64_pkgconfig_fix.py",
    "conan/hooks/hook_glib_macos_intl_fix.py",
    "engine/src/os/core/jce_crash_handler.c",
    "engine/src/os/platform/jce_single_instance.c",
    "engine/src/os/platform/jce_host_shell.c",
    "engine/src/renderer/jce_renderer_caps.c",
    "engine/src/renderer/jce_lowlevel.c",
    "engine/src/renderer/jce_scene_renderer.c",
    "engine/src/middleware/ui/jce_game_hud.c",
    "engine/src/middleware/ui/jce_ui_rmlui.cpp",
    "editor/src/io/jce_editor_file_util.h",
    "editor/src/core/jce_editor_play.cpp",
    "editor/src/io/jce_editor_scene_serial.cpp",
    "editor/src/ui/jce_editor_style.cpp",
    "caged_kingdom/src/game/ck_engine_smoke.c",
    "caged_kingdom/CMakeLists.txt",
    "scripts/macos/ios/Info.plist",
    "scripts/macos/ios/ck_i1024.png",
    "engine/src/middleware/scene/jce_scene_components_json.c",
    "editor/src/core/jce_editor_state.cpp",
    "editor/src/core/jce_hotkeys.h",
    "editor/src/panels/jce_panel_assets_grid.cpp",
]


def _mkdir_p(sftp, remote_dir):
    parts = remote_dir.split("/")
    path = ""
    for part in parts:
        if not part:
            path = "/"
            continue
        path = posixpath.join(path, part)
        try:
            sftp.stat(path)
        except FileNotFoundError:
            sftp.mkdir(path)


def sync_files(session: SSHSession):
    print("=== Syncing files to macOS remote ===")
    with session._client.open_sftp() as sftp:
        for rel_path in SYNC_FILES:
            local = os.path.join(LOCAL_ROOT, rel_path.replace("/", os.sep))
            remote = posixpath.join(REMOTE_ROOT, rel_path)
            if not os.path.isfile(local):
                print(f"  SKIP (not found): {rel_path}")
                continue
            _mkdir_p(sftp, posixpath.dirname(remote))
            sftp.put(local, remote)
            print(f"  UP: {rel_path}")

    session.run(
        f"find {REMOTE_ROOT}/scripts -name '*.sh' | xargs chmod +x 2>/dev/null; echo done"
    )
    print("  Permissions fixed.")


def install_hook(session: SSHSession):
    """Copy hook into the active Conan extensions hooks directory."""
    print("\n=== Installing Conan hook on remote ===")
    hook_src = posixpath.join(REMOTE_ROOT, "conan/hooks/hook_cmake_policy_fix.py")
    code, out, _ = session.run("echo $HOME")
    home = out.strip()
    hooks_dir = f"{home}/.conan2/extensions/hooks"
    session.run(f"mkdir -p {hooks_dir}")
    session.run(f"cp {hook_src} {hooks_dir}/hook_cmake_policy_fix.py")
    print(f"  Installed -> {hooks_dir}/hook_cmake_policy_fix.py")


def clear_broken_packages(session: SSHSession):
    """Remove packages that were previously built without the hook."""
    print("\n=== Clearing affected Conan packages ===")
    pkgs = ["rmlui", "bullet3", "enet", "minizip", "ogg", "zlib"]
    for pkg in pkgs:
        code, out, _ = session.run(
            f"conan remove '{pkg}/*' -c 2>&1 | tail -3; echo __done__"
        )
        if "Removed" in out or "removed" in out:
            print(f"  Cleared: {pkg}")
        else:
            print(f"  (no cached packages for {pkg})")


def fetch_shaders(session: SSHSession):
    """Download *_mtl.bin shader files from macOS remote to local source tree."""
    print("\n=== Fetching Metal shader binaries from macOS remote ===")
    remote_shader_dir = posixpath.join(REMOTE_ROOT, "engine/resources/assets/shaders")
    local_shader_dir = os.path.join(LOCAL_ROOT, "engine", "resources", "assets", "shaders")
    os.makedirs(local_shader_dir, exist_ok=True)

    _, out, _ = session.run(f"find {remote_shader_dir} -name '*_mtl.bin' 2>/dev/null")
    remote_files = [f.strip() for f in out.splitlines() if f.strip().endswith("_mtl.bin")]

    if not remote_files:
        print("  No *_mtl.bin files found on remote (shaders not compiled yet?).")
        return

    with session._client.open_sftp() as sftp:
        fetched = 0
        for remote_path in remote_files:
            filename = posixpath.basename(remote_path)
            local_path = os.path.join(local_shader_dir, filename)
            try:
                sftp.get(remote_path, local_path)
                print(f"  DOWN: shaders/{filename}")
                fetched += 1
            except Exception as e:
                print(f"  SKIP {filename}: {e}")

    print(f"  Fetched {fetched} Metal shader file(s).")


def run_build(session: SSHSession, target: str = "x64"):
    script = f"build-macos-{target}.sh"
    classifier = {"x64": "darwin-x86_64", "arm64": "darwin-aarch64"}.get(target, "")
    print(f"\n=== Running {script} on remote ===")
    cmd = f"cd {REMOTE_ROOT} && bash scripts/macos/{script} 2>&1"
    code, _, err = session.run(cmd, timeout=3600, stream=True)
    if err:
        print(err, end="", file=sys.stderr)
    if code == 0 and classifier:
        _, out, _ = session.run(
            f"ls -lh {REMOTE_ROOT}/build/jni/natives/{classifier}/ 2>/dev/null || echo 'not staged yet'"
        )
        print(f"JNI natives ({classifier}): {out.strip()}")
    return code


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="x64", choices=["x64", "arm64", "universal"],
                    help="macOS build target (default: x64)")
    ap.add_argument("--skip-clear", action="store_true",
                    help="Skip clearing affected Conan packages")
    ap.add_argument("--sync-only", action="store_true",
                    help="Sync files without running build")
    ap.add_argument("--build-only", action="store_true",
                    help="Run build without syncing files")
    ap.add_argument("--clean", action="store_true",
                    help="Pass --clean to the build script first")
    ap.add_argument("--fetch-shaders", action="store_true",
                    help="Download *_mtl.bin shader files from Mac to local source tree, then exit")
    args = ap.parse_args()

    if args.fetch_shaders:
        print(f"Connecting to {USER}@{HOST} ...")
        with SSHSession(HOST, USER, PASSWORD, connect_timeout=15) as s:
            code, out, _ = s.run(f"test -d {REMOTE_ROOT} && echo exists || echo missing")
            if "missing" in out:
                print(f"ERROR: Remote directory {REMOTE_ROOT} does not exist.")
                sys.exit(1)
            fetch_shaders(s)
        print("\n[DONE] Metal shader fetch complete.")
        return

    print(f"Connecting to {USER}@{HOST} ...")
    with SSHSession(HOST, USER, PASSWORD, connect_timeout=15) as s:
        code, out, _ = s.run(f"test -d {REMOTE_ROOT} && echo exists || echo missing")
        if "missing" in out:
            print(f"ERROR: Remote directory {REMOTE_ROOT} does not exist.")
            sys.exit(1)

        if not args.build_only:
            sync_files(s)
            install_hook(s)

            if not args.skip_clear:
                clear_broken_packages(s)

        if args.sync_only:
            print("\n[DONE] Sync complete (--sync-only).")
            return

        if args.clean:
            script = f"build-macos-{args.target}.sh"
            print(f"\n=== Cleaning remote build dirs ({args.target}) ===")
            s.run(f"cd {REMOTE_ROOT} && bash scripts/macos/{script} --clean 2>&1", timeout=60)

        exit_code = run_build(s, args.target)

    if exit_code == 0:
        print(f"\n[SUCCESS] macOS {args.target} build completed.")
    else:
        print(f"\n[FAILED] Build failed with exit code {exit_code}.")
        sys.exit(exit_code)


if __name__ == "__main__":
    main()
