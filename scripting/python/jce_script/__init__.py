"""jce_script — the JCE scripting surface, for Python.

HAND-WRITTEN.  The 71 entry points, their ctypes declarations and their type
stubs are generated from `contracts/script-api.json` into
`_generated.py` / `_generated.pyi`; what lives here is the loading and lifetime
policy around them, which no manifest describes.

    import jce_script

    lib = jce_script.load_library()          # finds jce_script_api.dll/.so
    api = jce_script.attach(handle, lib)     # a JceScriptApi* the engine made
    api.set_position(entity, 1.0, 2.0, 3.0)
    pos = api.get_position(entity)           # (x, y, z), or None

WHERE THE HANDLE COMES FROM, stated rather than implied.  `jce_script_api`
contains no engine code at all — it is pure marshalling glue over the
`JceScriptHost` callback table the RUNTIME supplies (scripting/c_abi's own
CMakeLists says so, and that is what makes "the surface is all the shared
object exports" a structural claim rather than an aspiration).  So a bare
`python.exe` that loads this package can call nothing useful: there is no host
behind the handle.  This is the in-process binding transport of owner decision
1, and the handle is created by the process that has the engine in it.

`open_host` exists for the one caller that HAS a `JceScriptHost` already: a C
extension, a test harness, or an embedder that built the table itself.  It is
not a way to build a host FROM Python — the struct's field order is an engine
private, and script-api.json deliberately publishes no layout for it.
"""

from __future__ import annotations

import ctypes
import os
import sys
from pathlib import Path

from ._generated import (
    Api,
    ENTRY_COUNT,
    ENTRY_NAMES,
    MissingExportError,
    NOT_EXPOSED,
    OWNED_STRING_INITIAL_CAPACITY,
    OWNED_STRING_MAX_ATTEMPTS,
    SCRIPT_API_VERSION,
    _Entries,
)

__all__ = [
    "Api",
    "ENTRY_COUNT",
    "ENTRY_NAMES",
    "LibraryNotFound",
    "MissingExportError",
    "NOT_EXPOSED",
    "OWNED_STRING_INITIAL_CAPACITY",
    "OWNED_STRING_MAX_ATTEMPTS",
    "SCRIPT_API_VERSION",
    "ScriptApiError",
    "VersionMismatch",
    "attach",
    "close",
    "library_names",
    "load_library",
    "open_host",
]


class ScriptApiError(RuntimeError):
    """Anything that goes wrong loading or opening the scripting library."""


class LibraryNotFound(ScriptApiError):
    """jce_script_api was not found on any searched path."""


class VersionMismatch(ScriptApiError):
    """The loaded library is older than the manifest this package was built
    from.

    Reported with BOTH numbers, because the C ABI header's compatibility rule
    is asymmetric and a single number cannot say which side is behind:
    an older binding against a newer library is a strict subset and loads; a
    newer binding against an older library cannot degrade, because it would
    call entries that are not there."""


def library_names() -> tuple[str, ...]:
    """The platform's spellings of the shared library's file name."""
    if sys.platform == "win32":
        return ("jce_script_api.dll",)
    if sys.platform == "darwin":
        return ("libjce_script_api.dylib", "jce_script_api.dylib")
    return ("libjce_script_api.so", "jce_script_api.so")


def _candidates(path: str | os.PathLike[str] | None) -> list[Path]:
    if path is not None:
        return [Path(path)]
    out: list[Path] = []
    env = os.environ.get("JCE_SCRIPT_API")
    if env:
        out.append(Path(env))
    here = Path(__file__).resolve().parent
    for base in (here, here.parent, Path.cwd()):
        out += [base / n for n in library_names()]
    # Bare names last, so the loader's own search path is a fallback rather
    # than the first thing tried: a stale copy earlier on PATH would otherwise
    # win over the one shipped beside the package.
    out += [Path(n) for n in library_names()]
    return out


def load_library(path: str | os.PathLike[str] | None = None) -> ctypes.CDLL:
    """Load jce_script_api and check its version against this package's.

    Search order: explicit `path`, then $JCE_SCRIPT_API, then beside this
    package, then the working directory, then the loader's own search path.
    """
    tried: list[str] = []
    lib = None
    for cand in _candidates(path):
        try:
            lib = ctypes.CDLL(str(cand))
            break
        except OSError as exc:
            tried.append(f"{cand}: {exc}")
    if lib is None:
        raise LibraryNotFound(
            "jce_script_api was not found. Set $JCE_SCRIPT_API to the built "
            "shared library, or pass load_library(path=...). Tried:\n  "
            + "\n  ".join(tried))

    fn = _require(lib, "jce_script_api_version")
    fn.restype = ctypes.c_uint32
    fn.argtypes = []
    version = fn()
    if version < SCRIPT_API_VERSION:
        raise VersionMismatch(
            f"jce_script_api implements script_api_version {version}, but this "
            f"package was generated from {SCRIPT_API_VERSION}. A newer binding "
            f"cannot run against an older library: it declares entry points "
            f"the library does not export.")
    return lib


def _require(lib: ctypes.CDLL, name: str):
    try:
        return getattr(lib, name)
    except AttributeError:
        raise MissingExportError(
            f"{name} is not exported by the loaded library — it does not look "
            f"like jce_script_api") from None


_ENTRIES: dict[int, _Entries] = {}


def _entries(lib: ctypes.CDLL) -> _Entries:
    """One _Entries per loaded library, cached.

    Resolving 74 symbols is cheap, but re-resolving them per Api would also
    re-assign argtypes on the SAME ctypes function objects, which is shared
    mutable state on the CDLL. Doing it once is both faster and single-writer.
    """
    key = id(lib)
    got = _ENTRIES.get(key)
    if got is None:
        got = _Entries(lib)
        _ENTRIES[key] = got
    return got


def attach(handle: int, lib: ctypes.CDLL | None = None) -> Api:
    """Wrap a JceScriptApi* the engine already opened.

    Does NOT take ownership: the process that called jce_script_api_open owns
    the handle and must close it. Passing 0 is refused rather than deferred —
    every entry point guards a NULL handle, so a zero handle would silently
    make the whole surface a no-op that looks like a host with nothing wired.
    """
    if not handle:
        raise ScriptApiError(
            "attach(0): a null JceScriptApi* would make every call a silent "
            "no-op returning the absent value")
    if lib is None:
        lib = load_library()
    return Api(handle, _entries(lib), lib)


def open_host(host_ptr: int, host_size: int,
              lib: ctypes.CDLL | None = None) -> Api:
    """Open a handle over a caller-owned JceScriptHost.

    `host_size` MUST come from the caller's own sizeof(JceScriptHost) and must
    never be summed by hand: the library copies min(host_size, its own sizeof)
    over a zeroed table, and that is the entire mechanism that lets a host
    built against an older header stay safe. A size larger than the caller's
    real struct makes the library read past it.

    The returned Api must be closed with jce_script.close().
    """
    if lib is None:
        lib = load_library()
    entries = _entries(lib)
    handle = entries._meta_open(ctypes.c_void_p(host_ptr),
                               ctypes.c_size_t(host_size),
                               ctypes.c_uint32(SCRIPT_API_VERSION))
    if not handle:
        raise ScriptApiError(
            f"jce_script_api_open returned NULL for host=0x{host_ptr:x} "
            f"size={host_size} script_api_min={SCRIPT_API_VERSION} — the host "
            f"is NULL, the size is 0, or this package is newer than the "
            f"library")
    return Api(int(handle), entries, lib)


def close(api: Api) -> None:
    """Release a handle opened by open_host().

    The Api's handle is zeroed afterwards, so a second close is a no-op rather
    than a double free — jce_script_api_close(NULL) is documented as a no-op,
    and without the zeroing the second call would hand the library a freed
    pointer that it cannot tell from a live one.
    """
    if not api._h or not api._h.value:
        return
    api._f._meta_close(api._h)
    api._h = ctypes.c_void_p(None)
