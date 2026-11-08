# Toolchain discovery

Use scripts/jce.py --help and subcommand help for supported arguments. Do not hardcode a developer's compiler, JDK, NDK, SDK, signing identity or asset location in a reusable script.

On Windows, the build driver selects the MSVC environment. Conan profiles own dependency settings; generated toolchains, recipes and package identities must agree with the requested variant and graphics tier. Do not reuse a graph stamp from retired source-patching hooks.

JAVA_HOME and other explicit toolchain inputs are supplied by the caller. Respect CONAN_HOME so local and CI dependency caches can be isolated. Unknown installed JCE hooks must stop synchronization instead of being overwritten.

Use an installed, verified Gradle for Android; original SDL Java is fetched from the fixed upstream cache rather than copied into public source. First-party platform packaging lives under tools/build/, with manual forwarding launchers under scripts/.

The source export, build tree, SDK install and final package are distinct directories. Record which one a command consumes. A compiler discovery or source gate does not prove a platform binary was produced.

## Current editor and SDK build interfaces

The complete embedded or adjacent builder payload for zero-user-environment builds is not currently delivered. The public `jce_toolchain_refresh/get/set_override` interfaces discover installed host tools or accept explicit paths. Discovery uses PATH, conventional install locations and platform SDK markers; Windows also uses Visual Studio discovery and its CMake/Ninja copies. Preferences > Toolchains persists explicit overrides. Build Profiles resolves its SDK from the SDK Root input, the project SDK field or JCE_SDK_DIR; it does not automatically choose a matching SDK beside the editor. A standalone editor embeds runtime assets, not the compiler or SDK.

Discovery queries own both stdout and stderr pipes so they do not require an
inherited stderr handle from a windowed caller. The existing cooperative budget
is two seconds for generic tool version queries and three seconds for Visual Studio
queries. Retain at most 1023 stdout bytes plus its NUL in a 1024-byte buffer;
continue draining excess stdout and all stderr with a fixed 256-byte scratch.
Only a successfully exited, drained query with exit code zero supplies the first
nonempty stdout line. Stderr cannot supply a tool path or version. Optional
version failure keeps a verified executable/SDK marker root. This is discovery
admission; the public process owner separately resolves each Windows output
stream once per spawn. Capture true keeps APP. Capture false inherits a valid
parent handle, including a file/pipe, while NULL/INVALID/stale handles use SDL
NULL/NUL. POSIX inheritance is unchanged. No public struct/ABI change, console
allocation, parent-handle mutation or BACKGROUND substitution is introduced.
A missing-handle diagnostic records effective policy and actual spawn status;
arguments/environment payloads are excluded. `process.spawn` and its four stdio
mode/reason counters describe the CPU attempt, not successful child completion.
The process owner copies the real spawn error before property/argv cleanup;
Shader Graph `compile_sc` includes that error rather than just a generic message.

`test_jce_process_stdio` compiles the complete process owner with real SDL
property bags and typed counted Windows/process ports; invalid inheritance must
refuse spawn. Old-inheritance and missing-error-copy controls compile and fail
ordinary assertions. They are not native GUI or compiler execution evidence.
The public-only `test_jce_process_gui_stdio.c` is both a CMake Windows GUI target
and reusable installed-SDK consumer source. Default execution requires actual
absent parent stdout/stderr; a launcher providing handles returns SKIP 77.
An optional `--inherited` mode compares real launcher-provided handles. Its
self-child writes both streams, exits 23, and exercises all four capture
combinations plus repeated poll/destroy and force-kill/reap. Pass an owned
receipt path and inspect actual stream bytes, exit codes and state; rebuilding
this source with the selected installed SDK is a separate gate. Verify those
native controls and actual shaderc/compiler use after SDK refresh. Source and
counted-port controls do not certify an executed build or fixed GUI behavior.

Editor Build's private `jce_build_tool_probe` uses these public process/clock
owners with a cooperative two-second elapsed deadline checked before each
256-byte pipe read, before completion admission and after cleanup. Fair dual
streams retain independent 1023-byte stdout/stderr prefixes, while more than
64 KiB combined observed output refuses admission. Only an observed exit-zero,
drained query with a nonempty stdout line supplies a version; stderr and copied
spawn errors remain separate bounded failure diagnostics. Every spawned handle
uses public destroy/kill/reap even after explicit kill failure. The budget does
not hard-bound synchronous spawn or OS cleanup. Canonical static-name CPU query
spans disclose outcome/bytes/truncation/elapsed/kill acceptance, not compiler
success. `test_jce_build_tool_probe` compiles this whole owner with counted
process/clock ports and real trace controls; same-source deadline/stderr/output/
exit/cleanup/final-drain wire negatives must compile and fail ordinary assertions. Actual
GUI tools, builders and native child cleanup remain independent acceptance.

Windows project builds consume the same public discovery root as Preferences,
then call its `VC/Auxiliary/Build/vcvarsall.bat` before configure/build in the same
child command. A missing root can fall back to `cl` on PATH; that fallback does
not provide missing compiler headers, SDK libraries or a complete environment.
Keep installed-tool discovery separate from delivery of a builder payload.

```bash
python scripts/jce.py test --suite engine --target test_jce_toolchain_override --target test_jce_toolchain_override_old_stderr --filter "^test_jce_toolchain_override(_old_stderr)?$"
```

SDK project builds use the existing native build manager and require CMake/Ninja; Conan belongs to the engine-workspace dependency build, not ordinary installed-SDK consumption. Windows project builds use MSVC-compatible tools; x86_64, i686 and aarch64 are target selections, while ARM32 is explicitly refused. macOS admits x86_64/aarch64 and native Linux currently admits the host architecture only. These selections do not certify that a matching compiler or payload exists. The installed SDK supplies public headers, libraries, CMake helpers and build-host asset tools; asset tools alone are not a complete compiler environment.

Before cleaning or cooking, SDK admission checks the selected archive's `lib/<CONFIG>/jce-sdk-target.json` for compiler-derived system, architecture, pointer size, configuration, variant, delivery and CRT, plus the archive identity. GUI debug uses authoring; release/dist use embedded. Missing or conflicting metadata requires a matching SDK refresh. This admission, compiler discovery, source gates and successful SDK installation are separate from producing and running a target application. See [SDK consumption and delivery](user-project-sdk.md) and [build gates](build-and-gate.md).

## Complete target payload and directory ownership

A zero-environment delivery requires executable tools for the build host and a complete toolchain for each advertised target: CMake with its modules, Ninja, the selected C/C++ compiler, linker and platform resource tools, target headers/sysroot, system import/static libraries, CRT/C++ runtime, build-host cook/pack/embed/shader tools, and an ABI-matching SDK. Windows MSVC-compatible candidates require the Windows and MSVC runtime/library closure; GCC/Clang targets require their corresponding target sysroot/runtime; Android uses NDK, Web uses Emscripten, and Apple targets use the platform SDK. Existing MSVC archives cannot be relabeled as LLVM-MinGW/Zig-compatible. Verify compiler ABI, target architecture, CRT, provenance and redistribution permissions for the actual payload. Missing components or unsupported targets must remain unavailable with a specific build error; a host PATH fallback cannot certify zero-environment delivery.

Reuse the existing build/cook/package authorities. Native project caches live under `<project>/build/<platform>-<arch>/`, linked artifacts under its `<variant>/`, intermediate objects/generated content under `build/<variant>/`, and receipts under `reports/`. Final applications use `<project>/dist/apps/<app>/<platform>-<arch>-<variant>/`; SDK installation and editor delivery remain distinct products. The existing Dist publisher audits the built executable and an owned reports candidate, then publishes by host atomic rename; failed publication keeps the old artifact and refuses unknown output contents. Keep tools, caches, temporary candidates and reports out of the final application directory. Qualify zero-environment delivery by compiling, linking and running the exact target from an isolated OS-only environment and relocating the published application.

## Stock runtime and explicit-key relocation

A published stock runtime may move together with its unchanged manifest and
verified relative files. `stock-app seal --pak-key-file` still requires an
explicit external key; its bounded input must decode from exactly 64 hexadecimal
digits to 32 bytes and match the SHA-256 bound to the verified producer and
protection metadata. The recorded absolute producer key path remains provenance,
not a required location for the caller's identical key. Recheck decoded-key
identity after tool execution. Wrong, malformed or changed keys still refuse
sealing; never log or copy raw key contents into a public receipt.

Relocation does not relax runtime/tool hashes, target and language admission,
selected Bundle identity, authenticated outer-archive deep verification or
output publication checks. An unchanged runtime/key relocated seal is separate
from running the resulting single executable. Neither is delivery of the full
compiler, target sysroot/CRT or zero-environment native project builder.

## Selected archive admission

Dist SDK-consumer assets use the public Bundle packer's cooked scene graph, including explicit dynamic roots and recursive dependency declarations. `jce_pak --selected-archive` reads one immutable blob, opens it through the public Bundle/Archive owners, skips the reserved Bundle manifest and deep-verifies the final archive. Equal logical addresses merge only when their bytes match; differing bytes fail instead of choosing an arbitrary override. The emitted BOM records selected provenance and authenticated deep-verification. Stock trees remain an explicit safe superset; this is not proof of globally minimal assets or runtime parity. The public loader still accepts legacy Bundle manifests, so standalone tool admission is not a modern typed-graph certificate. Actual consumer controls and Editor/EXE/reopened-Bundle verification remain separate gates.

Offline selected-Bundle inspection must use the explicit exact build target,
not the tool executable's host. The shared driver passes --bundle-target from
delivery.bundle_platform together with the content-category --platform. Web
defaults to exact web; legacy desktop defaults to the compiled host; typed
mobile/console without an exact target refuses. Category/target contradictions
refuse. Public jce_bundle_file_inspect_memory/file never mounts a filesystem and
shares manifest/dependency/payload/sidecar checks. Runtime/catalog APIs remain
compiled-platform-only. Requested auto admits portable auto content only;
unknown expected tokens refuse even for portable content. A Windows host
accepting Web content for offline packaging does not certify browser execution.

The inspector's borrowed Pak is marked irreversibly inspection-only. Canonical
SDK named/legacy filesystem mounts and Pak fallback attachment reject this same
handle, including restoration of the canonical three-function provider through
the old public setter. Unknown opaque custom providers retain their ABI and are
not interpreted as native Pak layouts. Custom decoding or independently opening
bytes is outside this admission contract; inspection is not a security sandbox.
The actual Bundle inspection mount fixture and its omitted-mark/mount/fallback
owner negatives qualify these CPU paths separately from browser execution.
NULL-provider named/legacy mounts refuse before canonical restoration. Pak
admission uses finite cycle detection, and fallback push rejects intersecting
(shared-tail) chains in linear time before mutation. Exact-owner no-provider
and shared-tail omission controls must fail ordinary assertions, not timeouts.
