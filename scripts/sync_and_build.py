#!/usr/bin/env python3
"""Sync local changes to remote Linux host and run build-linux-x64.sh."""
import os
import sys
import posixpath

# Add scripts dir to path so we can import ssh_helper
sys.path.insert(0, os.path.dirname(__file__))
from ssh_helper import SSHSession

HOST = "192.168.10.10"
USER = "lunwen"
PASSWORD = "1014"
REMOTE_ROOT = "/home/lunwen/Code/JCE"
LOCAL_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Files to sync (relative to repo root)
SYNC_FILES = [
    "scripts/lib/jce_beep.sh",
    "scripts/lib/jce_common.sh",
    "scripts/linux/build-linux-x64.sh",
    "scripts/linux/build-host-tools.sh",
    "scripts/linux/build-editor.sh",
    "scripts/linux/build-linux-arm64.sh",
    "scripts/ssh_helper.py",
    "CMakeLists.txt",
    "engine/CMakeLists.txt",
    "editor/CMakeLists.txt",
    "engine/src/os/core/jce_crash_handler.c",
    "engine/src/os/platform/jce_host_shell.c",
    "engine/src/middleware/scene/jce_scene_components_json.c",
    "engine/src/renderer/jce_lowlevel.c",
    "engine/src/renderer/jce_scene_renderer.c",
    "engine/src/renderer/jce_renderer_caps.c",
    "editor/src/core/jce_editor_state.cpp",
    "editor/src/core/jce_hotkeys.h",
    "editor/src/panels/jce_panel_assets_grid.cpp",
]


def sync_files(session: SSHSession):
    print("=== Syncing files to remote ===")
    with session._client.open_sftp() as sftp:
        for rel_path in SYNC_FILES:
            local = os.path.join(LOCAL_ROOT, rel_path.replace("/", os.sep))
            remote = posixpath.join(REMOTE_ROOT, rel_path)
            if not os.path.isfile(local):
                print(f"  SKIP (not found): {rel_path}")
                continue
            # Ensure remote directory exists
            remote_dir = posixpath.dirname(remote)
            _mkdir_p(sftp, remote_dir)
            sftp.put(local, remote)
            print(f"  UP: {rel_path}")

    # Fix permissions on shell scripts
    code, _, err = session.run(
        f"find {REMOTE_ROOT}/scripts -name '*.sh' | xargs chmod +x 2>/dev/null; echo done"
    )
    print("  Permissions fixed.")


def _mkdir_p(sftp, remote_dir):
    """Create remote directory recursively (SFTP has no makedirs)."""
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


def run_build(session: SSHSession):
    print("\n=== Running build-linux-x64.sh on remote ===")
    cmd = f"cd {REMOTE_ROOT} && bash scripts/linux/build-linux-x64.sh 2>&1"
    code, out, err = session.run(cmd, timeout=600, stream=True)
    if err:
        print(err, end="", file=sys.stderr)
    return code


def main():
    print(f"Connecting to {USER}@{HOST}...")
    with SSHSession(HOST, USER, PASSWORD, connect_timeout=15) as s:
        # Ensure remote repo dir exists
        code, out, _ = s.run(f"test -d {REMOTE_ROOT} && echo exists || echo missing")
        if "missing" in out:
            print(f"ERROR: Remote directory {REMOTE_ROOT} does not exist.")
            print("Please clone the repo on the remote host first.")
            sys.exit(1)

        sync_files(s)
        exit_code = run_build(s)

    if exit_code == 0:
        print("\n[SUCCESS] Build completed successfully.")
    else:
        print(f"\n[FAILED] Build failed with exit code {exit_code}.")
        sys.exit(exit_code)


if __name__ == "__main__":
    main()
