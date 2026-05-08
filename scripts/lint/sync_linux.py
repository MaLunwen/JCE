#!/usr/bin/env python3
"""Sync local changes to Linux host and run build-linux-x64.sh (+ JNI stage)."""
import os
import sys
import posixpath

sys.path.insert(0, os.path.dirname(__file__))
from ssh_helper import SSHSession

HOST = "192.168.10.9"
USER = "lunwen"
PASSWORD = "1014"
REMOTE_ROOT = "/home/lunwen/Code/JCE"
LOCAL_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Individual source files that have changed and need pushing
SYNC_FILES = [
    "scripts/lib/jce_beep.sh",
    "scripts/lib/jce_common.sh",
    "scripts/linux/build-linux-x64.sh",
    "scripts/linux/build-linux-arm64.sh",
    "scripts/linux/build-host-tools.sh",
    "scripts/linux/build-editor.sh",
    "CMakeLists.txt",
    "engine/CMakeLists.txt",
    "editor/CMakeLists.txt",
    "caged_kingdom/CMakeLists.txt",
    "conan/profiles/linux-x64",
    "conan/profiles/linux-arm64",
    "conan/hooks/hook_cmake_policy_fix.py",
    "conan/hooks/hook_bgfx_wasm_fix.py",
    "conan/hooks/hook_arm64_pkgconfig_fix.py",
    "engine/src/os/core/jce_crash_handler.c",
    "engine/src/os/platform/jce_single_instance.c",
    "engine/src/os/platform/jce_host_shell.c",
    "engine/src/renderer/jce_renderer_caps.c",
    "engine/src/renderer/jce_lowlevel.c",
    "engine/src/renderer/jce_scene_renderer.c",
    "engine/src/middleware/ui/jce_game_hud.c",
    "engine/src/middleware/ui/jce_ui_rmlui.cpp",
    "engine/src/middleware/scene/jce_scene_components_json.c",
    "engine/src/middleware/scene/jce_scene.c",
    "editor/src/io/jce_editor_file_util.h",
    "editor/src/core/jce_editor_play.cpp",
    "editor/src/io/jce_editor_scene_serial.cpp",
    "editor/src/ui/jce_editor_style.cpp",
    "editor/src/core/jce_editor_state.cpp",
    "editor/src/core/jce_hotkeys.h",
    "editor/src/panels/jce_panel_assets_grid.cpp",
    "caged_kingdom/src/game/ck_app.c",
]

# Entire local directories to mirror recursively to the remote
SYNC_DIRS = [
    "engine/include",
    "engine/src",
    "editor/src",
    "caged_kingdom/src",
    "caged_kingdom/include",
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
    print("=== Syncing files to Linux remote ===")
    TEXT_EXTS = {".sh", ".py", ".c", ".cpp", ".h", ".cmake", ".txt", ".md", ""}

    def _upload(sftp, local_path, remote_path):
        ext = os.path.splitext(local_path)[1].lower()
        if ext in TEXT_EXTS or not ext:
            with open(local_path, "rb") as f:
                data = f.read().replace(b"\r\n", b"\n")
            import io
            sftp.putfo(io.BytesIO(data), remote_path)
        else:
            sftp.put(local_path, remote_path)

    with session._client.open_sftp() as sftp:
        # Individual files
        for rel_path in SYNC_FILES:
            local = os.path.join(LOCAL_ROOT, rel_path.replace("/", os.sep))
            remote = posixpath.join(REMOTE_ROOT, rel_path)
            if not os.path.isfile(local):
                print(f"  SKIP (not found): {rel_path}")
                continue
            _mkdir_p(sftp, posixpath.dirname(remote))
            _upload(sftp, local, remote)
            print(f"  UP: {rel_path}")

        # Entire directories (recursive mirror)
        for rel_dir in SYNC_DIRS:
            local_dir = os.path.join(LOCAL_ROOT, rel_dir.replace("/", os.sep))
            remote_dir = posixpath.join(REMOTE_ROOT, rel_dir)
            print(f"  DIR: {rel_dir}/")
            count = 0
            for dirpath, _dirs, files in os.walk(local_dir):
                rel_from_root = os.path.relpath(dirpath, LOCAL_ROOT)
                remote_subdir = posixpath.join(
                    REMOTE_ROOT,
                    rel_from_root.replace(os.sep, "/")
                )
                _mkdir_p(sftp, remote_subdir)
                for fname in files:
                    lfile = os.path.join(dirpath, fname)
                    rfile = posixpath.join(remote_subdir, fname)
                    _upload(sftp, lfile, rfile)
                    count += 1
            print(f"       {count} files uploaded")

    session.run(
        f"find {REMOTE_ROOT}/scripts -name '*.sh' | xargs chmod +x 2>/dev/null; echo done"
    )
    print("  Permissions fixed.")


def install_hook(session: SSHSession):
    print("\n=== Installing Conan hook on remote ===")
    hook_src = posixpath.join(REMOTE_ROOT, "conan/hooks/hook_cmake_policy_fix.py")
    _, home, _ = session.run("echo $HOME")
    home = home.strip()
    hooks_dir = f"{home}/.conan2/extensions/hooks"
    session.run(f"mkdir -p {hooks_dir}")
    session.run(f"cp {hook_src} {hooks_dir}/hook_cmake_policy_fix.py")
    print(f"  Installed -> {hooks_dir}/hook_cmake_policy_fix.py")


def run_build(session: SSHSession, arch: str = "x64"):
    script = f"build-linux-{arch}.sh"
    classifier = "linux-x86_64" if arch == "x64" else "linux-aarch64"
    print(f"\n=== Running {script} on remote ===")
    cmd = (
        f"export PATH=\"$HOME/.local/bin:$PATH\"; "
        f"export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64; "
        f"cd {REMOTE_ROOT} && bash scripts/linux/{script} 2>&1"
    )
    code, _, err = session.run(cmd, timeout=3600, stream=True)
    if err:
        print(err, end="", file=sys.stderr)
    return code, classifier


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--sync-only", action="store_true",
                    help="Sync files without running build")
    ap.add_argument("--build-only", action="store_true",
                    help="Run build without syncing files")
    ap.add_argument("--clean", action="store_true",
                    help="Pass --clean to the build script")
    ap.add_argument("--arm64", action="store_true",
                    help="Build linux-aarch64 instead of linux-x64")
    args = ap.parse_args()

    arch = "arm64" if args.arm64 else "x64"

    print(f"Connecting to {USER}@{HOST} ...")
    with SSHSession(HOST, USER, PASSWORD, connect_timeout=15) as s:
        code, out, _ = s.run(f"test -d {REMOTE_ROOT} && echo exists || echo missing")
        if "missing" in out:
            print(f"ERROR: Remote directory {REMOTE_ROOT} does not exist.")
            sys.exit(1)

        if not args.build_only:
            sync_files(s)
            install_hook(s)

        if args.sync_only:
            print("\n[DONE] Sync complete (--sync-only).")
            return

        if args.clean:
            script = f"build-linux-{arch}.sh"
            print(f"\n=== Cleaning remote build dirs ({arch}) ===")
            s.run(
                f"export PATH=\"$HOME/.local/bin:$PATH\"; "
                f"cd {REMOTE_ROOT} && bash scripts/linux/{script} --clean 2>&1",
                timeout=30
            )

        code, classifier = run_build(s, arch)
        if code == 0:
            print(f"\n[SUCCESS] Linux {arch} build complete.")
            _, out, _ = s.run(
                f"ls -lh {REMOTE_ROOT}/build/jni/natives/{classifier}/ 2>/dev/null || echo 'not staged yet'"
            )
            print(f"JNI natives: {out.strip()}")
        else:
            print(f"\n[FAILED] Build exited with code {code}.")
            sys.exit(code)


if __name__ == "__main__":
    main()
