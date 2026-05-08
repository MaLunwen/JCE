#!/usr/bin/env python3
"""Sync local changes to macOS host and run build-macos-{x64,arm64,ios-arm64}.sh."""
import os
import sys
import posixpath

sys.path.insert(0, os.path.dirname(__file__))
from ssh_helper import SSHSession

HOST = "192.168.10.9"
USER = "lunwen"
PASSWORD = "1014"
REMOTE_ROOT = "/Users/lunwen/code/JCE"
LOCAL_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

SYNC_FILES = [
    "scripts/lib/jce_beep.sh",
    "scripts/lib/jce_common.sh",
    "scripts/macos/build-macos-x64.sh",
    "scripts/macos/build-macos-arm64.sh",
    "scripts/macos/build-macos-universal.sh",
    "scripts/macos/build-host-tools.sh",
    "scripts/macos/build-editor.sh",
    "scripts/macos/build-ios-arm64.sh",
    "scripts/macos/ios/Info.plist",
    "scripts/macos/ios/ck_i1024.png",
    "CMakeLists.txt",
    "engine/CMakeLists.txt",
    "editor/CMakeLists.txt",
    "caged_kingdom/CMakeLists.txt",
    "conan/profiles/macos-x64",
    "conan/profiles/macos-arm64",
    "conan/profiles/ios-arm64",
    "conan/hooks/hook_cmake_policy_fix.py",
    "conan/hooks/hook_bgfx_wasm_fix.py",
    "conan/hooks/hook_arm64_pkgconfig_fix.py",
    "conan/hooks/hook_glib_macos_intl_fix.py",
    "engine/src/middleware/scene/jce_scene.c",
    "caged_kingdom/src/game/ck_app.c",
]

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
    print("=== Syncing files to macOS remote ===")
    TEXT_EXTS = {".sh", ".py", ".c", ".cpp", ".h", ".hpp", ".cmake",
                 ".txt", ".md", ".plist", ""}

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
        for rel_path in SYNC_FILES:
            local = os.path.join(LOCAL_ROOT, rel_path.replace("/", os.sep))
            remote = posixpath.join(REMOTE_ROOT, rel_path)
            if not os.path.isfile(local):
                print(f"  SKIP (not found): {rel_path}")
                continue
            _mkdir_p(sftp, posixpath.dirname(remote))
            _upload(sftp, local, remote)
            print(f"  UP: {rel_path}")

        for rel_dir in SYNC_DIRS:
            local_dir = os.path.join(LOCAL_ROOT, rel_dir.replace("/", os.sep))
            print(f"  DIR: {rel_dir}/")
            count = 0
            for dirpath, _dirs, files in os.walk(local_dir):
                rel_from_root = os.path.relpath(dirpath, LOCAL_ROOT)
                remote_subdir = posixpath.join(
                    REMOTE_ROOT, rel_from_root.replace(os.sep, "/")
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
    _, out, _ = session.run("echo $HOME")
    home = out.strip()
    hooks_dir = f"{home}/.conan2/extensions/hooks"
    session.run(f"mkdir -p {hooks_dir}")
    session.run(f"cp {hook_src} {hooks_dir}/hook_cmake_policy_fix.py")
    print(f"  Installed -> {hooks_dir}/hook_cmake_policy_fix.py")


def run_build(session: SSHSession, target: str = "x64"):
    if target in ("ios", "ios-arm64"):
        script = "build-ios-arm64.sh"
        # iOS codesign needs an unlocked keychain; forward the SSH password
        # (same as login password on this dev box) so the script's
        # `security unlock-keychain` runs non-interactively.
        env_prefix = f"KEYCHAIN_PASSWORD='{session.password}' " if hasattr(session, "password") and session.password else ""
    else:
        script = f"build-macos-{target}.sh"
        env_prefix = ""
    print(f"\n=== Running {script} on remote ===")
    cmd = f"cd {REMOTE_ROOT} && {env_prefix}bash scripts/macos/{script} 2>&1"
    code, _, err = session.run(cmd, timeout=3600, stream=True)
    if err:
        print(err, end="", file=sys.stderr)
    return code


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="x64",
                    choices=["x64", "arm64", "universal", "ios", "ios-arm64"])
    ap.add_argument("--sync-only", action="store_true")
    ap.add_argument("--build-only", action="store_true")
    args = ap.parse_args()

    print(f"Connecting to {USER}@{HOST} ...")
    with SSHSession(HOST, USER, PASSWORD, connect_timeout=15) as s:
        _, out, _ = s.run(f"test -d {REMOTE_ROOT} && echo exists || echo missing")
        if "missing" in out:
            print(f"ERROR: Remote directory {REMOTE_ROOT} does not exist.")
            sys.exit(1)

        if not args.build_only:
            sync_files(s)
            install_hook(s)

        if args.sync_only:
            print("\n[DONE] Sync complete (--sync-only).")
            return

        exit_code = run_build(s, args.target)

    if exit_code == 0:
        print(f"\n[SUCCESS] macOS {args.target} build completed.")
    else:
        print(f"\n[FAILED] Build failed with exit code {exit_code}.")
        sys.exit(exit_code)


if __name__ == "__main__":
    main()
