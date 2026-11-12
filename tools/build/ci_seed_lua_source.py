"""Seed Conan's documented, hash-addressed source cache on isolated CI runners.

This does not modify a Conan recipe, dependency source checkout or package.
It fetches the same immutable Lua 5.4.8 release with Conan's own verified get().
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

LUA_548_SHA256 = "4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae"
# Identical archive bytes are mandatory; Conan validates against LUA_548_SHA256.
SOURCES = (
    "https://www.tecgraf.puc-rio.br/lua/ftp/lua-5.4.8.tar.gz",
    "https://www.lua.org/ftp/lua-5.4.8.tar.gz",
)
CONF_NAME = "core.sources:download_cache"


def ci_paths(env: dict[str, str]) -> tuple[Path, Path]:
    """Refuse to mutate a developer's normal Conan home."""
    if env.get("GITHUB_ACTIONS", "").lower() != "true" or env.get("CI", "").lower() != "true":
        raise ValueError("Lua source prefetch is CI-only")
    if not env.get("GITHUB_WORKSPACE") or not env.get("CONAN_HOME"):
        raise ValueError("GITHUB_WORKSPACE and CONAN_HOME must be explicit")
    workspace = Path(env["GITHUB_WORKSPACE"])
    home = Path(env["CONAN_HOME"])
    if workspace.is_symlink() or home.is_symlink():
        raise ValueError("CI paths must not be symbolic links")
    if home.resolve() != (workspace.resolve() / ".conan2"):
        raise ValueError("refusing to change a non-workspace Conan home")
    return home, home / "verified-sources"


def set_source_cache(home: Path, cache: Path) -> None:
    """Preserve unrelated Conan configuration; refuse a conflicting value."""
    home.mkdir(parents=True, exist_ok=True)
    config = home / "global.conf"
    if config.is_symlink() or cache.is_symlink():
        raise ValueError("Conan configuration path is redirected")
    current = config.read_text(encoding="utf-8") if config.exists() else ""
    if len(current) > 100_000:
        raise ValueError("Conan global.conf exceeds the CI small-file budget")
    cache.mkdir(parents=True, exist_ok=True)
    expected = cache.resolve().as_posix()
    matched = [line.split("=", 1)[1].strip() for line in current.splitlines()
               if line.strip() and not line.lstrip().startswith("#")
               and line.partition("=")[0].strip() == CONF_NAME]
    if matched:
        if len(matched) != 1 or matched[0].strip('"') != expected:
            raise ValueError("existing Conan source-cache setting conflicts with CI pin")
        return
    updated = current + ("\n" if current and not current.endswith("\n") else "") + f"{CONF_NAME}={expected}\n"
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=home, prefix=".global.conf-",
                                     suffix=".tmp", delete=False, newline="\n") as stream:
        stream.write(updated)
        temporary = Path(stream.name)
    try:
        temporary.replace(config)
    finally:
        temporary.unlink(missing_ok=True)


def verified_cache_blob(cache: Path, sha: str = LUA_548_SHA256) -> bool:
    path = cache / sha
    if not path.exists():
        return False
    if path.is_symlink() or not path.is_file():
        raise ValueError("Conan source-cache entry has an invalid file type")
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while block := handle.read(64 * 1024):
            digest.update(block)
    if digest.hexdigest() != sha:
        raise ValueError("Conan source-cache entry SHA256 mismatch; refusing stale/corrupt data")
    # Conan's own get() produces metadata alongside the content-addressed blob.
    if not (cache / (sha + ".json")).is_file():
        raise ValueError("Conan source-cache blob lacks Conan-generated provenance metadata")
    return True


def ephemeral_recipe() -> str:
    # Source archive is not shipped here. Conan downloads, verifies and caches it.
    return '''from conan import ConanFile
from conan.tools.files import get

class JceSourceSeed(ConanFile):
    name = "jce-ci-source-seed"
    version = "0.0.1"

    def source(self):
        get(self, url={urls!r}, sha256={sha!r}, strip_root=True)
'''.format(urls=list(SOURCES), sha=LUA_548_SHA256)


def seed(env: dict[str, str]) -> None:
    home, cache = ci_paths(env)
    set_source_cache(home, cache)
    if verified_cache_blob(cache):
        print("Lua 5.4.8 Conan source cache: verified SHA256 (reused)")
        return
    conan = shutil.which("conan")
    if not conan:
        raise ValueError("Conan executable not found (must install Conan before source seed)")
    with tempfile.TemporaryDirectory(prefix="jce-ci-lua-source-") as dirname:
        temporary = Path(dirname)
        (temporary / "conanfile.py").write_text(ephemeral_recipe(), encoding="ascii")
        # `conan source .` loads a local recipe, which did not populate Conan's
        # source backup cache on hosted runners (all three OSes observed).
        # `conan create` exports a real recipe reference before running
        # source(); only that lifecycle is documented to seed the cache.
        subprocess.run([conan, "create", ".", "--test-folder="],
                       cwd=temporary, env=env, timeout=240, check=True)
    if not verified_cache_blob(cache):
        raise ValueError("Conan create did not populate the documented SHA256 source cache")
    print("Lua 5.4.8 Conan source cache: verified SHA256 (freshly seeded)")


def main() -> int:
    try:
        seed(dict(os.environ))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"CI Lua source prefetch FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
