"""
Bump cmake_minimum_required(VERSION X.Y) to 3.5 for packages that declare
a version older than 3.5, which CMake 4.x no longer supports.

The bump is applied in post_source() so it takes effect before any recipe
patches run.  Packages whose own recipe patches touch cmake_minimum_required
(and would therefore conflict) are listed in SKIP_PACKAGES.
"""
import os
import re

SKIP_PACKAGES = {"zeromq"}

_PATTERN = re.compile(
    r'(?i)(cmake_minimum_required\s*\(\s*VERSION\s+)([\d]+(?:\.[\d]+)*)(\s*(?:\.[^\)]*)?)\)',
)


def _version_tuple(ver_str):
    parts = ver_str.split(".")
    try:
        return tuple(int(p) for p in parts[:2])
    except ValueError:
        return (0, 0)


def _patch_cmakelists(path, conanfile):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            content = f.read()
    except OSError:
        return

    def _replace(m):
        ver = m.group(2)
        if _version_tuple(ver) < (3, 5):
            return m.group(1) + "3.5" + m.group(3) + ")"
        return m.group(0)

    patched, count = _PATTERN.subn(_replace, content)
    if count > 0:
        with open(path, "w", encoding="utf-8") as f:
            f.write(patched)
        conanfile.output.info(
            f"[cmake_policy_fix] {os.path.basename(path)}: bumped cmake_minimum_required to 3.5"
        )


def _patch_tree(conanfile, folder_attr):
    if conanfile.name in SKIP_PACKAGES:
        return
    src = getattr(conanfile, folder_attr, None)
    if not src or not os.path.isdir(src):
        return
    for dirpath, _dirs, files in os.walk(src):
        for fname in files:
            if fname == "CMakeLists.txt":
                _patch_cmakelists(os.path.join(dirpath, fname), conanfile)


def post_source(conanfile):
    # Fires when sources are first downloaded — patches the source cache.
    _patch_tree(conanfile, "source_folder")


def pre_build(conanfile):
    # Fires after "Copying sources to build folder" but before build()/cmake.
    # Handles packages whose source was cached before this hook was installed.
    _patch_tree(conanfile, "source_folder")
