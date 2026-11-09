# Windows target compatibility

Language level and operating-system compatibility are separate contracts.
The engine and native runtime use C99; the Editor uses C++20 in both lanes.
A compiler accepting these languages does not establish the dependency, OS API,
CPU instruction or graphics requirements of the resulting application.

| Lane | Target | Compiler/runtime | Delivery |
| --- | --- | --- | --- |
| Default desktop | Windows 7 through Windows 11 target | MSVC-compatible ABI and static support libraries | Native single EXE |
| Explicit legacy x86 | Windows XP (NT 5.1) through Windows 11 target | LLVM-MinGW-XP, GNU ABI, static libc++/support, system MSVCRT | Native single EXE |
| Explicit legacy x64 | Windows XP x64 (NT 5.2) through Windows 11 target | LLVM-MinGW-XP, GNU ABI, static libc++/support, system MSVCRT | Native single EXE |

The plus in XP+ and Win7+ is an inclusive upward-compatibility requirement.
The XP lane must also run on Vista, 7, 8, 8.1, 10 and 11; the MSVC lane must
also run on 8, 8.1, 10 and 11. Vista is covered by the XP lane. Record OS,
service pack, architecture and any OS process-emulation layer separately;
coverage does not invent OS/architecture combinations that never existed.
Both lanes keep the same C99/C++20 source, public C ABI, SDK contracts and
application data semantics. Their compiler runtimes and archive ABIs differ:
never interchange GNU and MSVC target archives.

## Shared stack and qualification coverage

Prefer the same pinned upstream versions in both lanes. A necessary provider
exception must identify both versions, the actual unsupported API/build input,
feature effects and regression evidence, with a convergence path. Keep that
exception in packaging/owned adapters; never modify original upstream code or
silently lower the default lane to make version labels match. Current version
exceptions include QuickJS-NG (XP 0.4.1, default 0.16.2) and Abseil
(XP 20250814.2, default 20260107.1). Their specific boundaries are described
below. They remain explicit unification items, not a claim that all dependency
versions match. Shared versions do not imply identical configuration or ABI.

Each architecture needs minimum-OS execution and newer-OS regression,
including Win11, for the same immutable artifact. Include startup, scene/game
pixels, Play/Stop, selected language drivers, file/bundle operations, clean
shutdown and relocation. Static import/forwarder audits complement execution;
a successful run on Win11 cannot prove Win7 imports, and an XP run cannot prove
all newer drivers. Record missing coverage as unqualified. The MSVC compiler
and static runtime must be pinned and audited: an OS version macro alone is
not a runtime-compatibility guarantee.

These are target policies, not a statement that every listed device has passed
acceptance. Keep complete-application verification separate from a core probe,
successful dependency resolution, compilation or static import audit. Record
the actual guest image and architecture when testing legacy Windows. XP x64
uses NT 5.2, rather than the x86 NT 5.1 floor.

Microsoft's [XP toolset guidance](https://learn.microsoft.com/en-us/cpp/build/configuring-programs-for-windows-xp)
limits its legacy lane to the older compiler's implemented language features.
The selected [LLVM-MinGW-XP project](https://github.com/mon/llvm-mingw-xp)
provides a modern compiler and XP-adjusted support libraries, rather than a
general replacement for missing Windows APIs. The two-lane policy preserves
the Editor's language level while requiring explicit dependency/API validation.

## Build ownership

The default Windows build and its MSVC SDK archives retain their existing
owners. Explicit XP builds use `cmake/toolchains/windows-{x86,x64}-xp-llvm.cmake`
and `tools/build/xp_editor.py`. Build an XP GUI candidate from the repository
root with either command below.

```bat
scripts/windows/build-editor.bat --xp
scripts/windows/build-editor.bat --xp --arch x86 --toolchain-root "C:\SDKs\llvm-mingw-xp"
```

Use `--variant release` for a Release XP Editor (AAC/H.264/H.265 adapters
default ON in a fresh cache; `--patented-codecs off` persistently disables
them in that Release cache). Use `--variant dist` for a distributable
XP Editor that always compiles those adapters out, regardless of the
stored preference. `editor --xp` and `xp-editor` honor both selections.
Windows XP Release and Dist have separate owned build caches and
candidates (`obj/release` / `release/jce_editor.exe` versus
`obj/dist` / `dist/jce_editor.exe`); old Dist cache contents are not
reinterpreted as a Release artifact. Dist keeps its historical
published file name; Release uses a `-release.exe` suffix.
These flags select CMake policy; actual XP compatibility and codec
operation still require compiler and device verification. Distribution
rights for the enabled codecs require independent legal assessment.

The launcher defaults to x64. It selects the toolchain root from
`--toolchain-root`, then `JCE_XP_TOOLCHAIN_ROOT`, then the pinned
LLVM-MinGW-XP distribution beside the repository. It discovers a host
shaderc and refreshes the native PAK writer and binary embedder through the
shared build driver. The GUI candidate lands at
`build/desktop/windows-<arch>-xp/dist/jce_editor.exe`. A matching owned
legacy cache can keep its intermediates in place under `build/compat/`;
the next build writes its executable to the desktop candidate path.

`JCEWindowsTarget.cmake` validates the legacy compiler, pointer width, target
triple and Windows API floor. The producer verifies the actual compiler and
runtime content identity. Target dependency packages have an independent XP
ABI identity and package store. Build-host tools, including shaderc, the PAK
writer, binary embedder and protoc, must run on the build host and remain
separate from target libraries. Original SDK and upstream source/package bytes
must remain unchanged.

For example, resolve each target without compiling its dependencies:

```powershell
python scripts/jce.py xp-editor --arch x86 --toolchain-root "$env:JCE_XP_TOOLCHAIN_ROOT" --phase resolve --jobs 2
python scripts/jce.py xp-editor --arch x64 --toolchain-root "$env:JCE_XP_TOOLCHAIN_ROOT" --phase resolve --jobs 2
```

The subsequent `dependencies`, `configure`, `build` and `audit` phases reuse
matching owned state. Direct `xp-editor` configure/build calls require explicit
`--shaderc`, `--host-pak` and `--host-bin2obj` paths. To publish to
`dist/editor/`, run `python scripts/jce.py xp-editor --phase audit` with the
same `--arch` and `--toolchain-root` and supply `--export-inventory` from the
exact target guest. See `xp-editor --help` for the current options.

### Optional upstream graphics diagnostics

The XP producer accepts --bgfx-bx-debug auto|off|on. Auto preserves the
upstream build default. On selects its supported BX_CONFIG_DEBUG option in a
Release build without changing Release optimization; it also enables upstream
assertions, not just logging. Upstream Debug forces checks on even with the
option off. The Conan hook partitions a changed/unknown configuration and
reports request, option and compiled-expression knowledge separately from CPU
profiling and debug annotations. Custom toolchains remain unobserved until
actual provider compile commands are checked. Consumer definitions must not
contradict the known provider expression. Reuse a built diagnostic only with
the same explicit request at audit. Explicit diagnostics stay in the owned cache
and are never published through the normal release path. Their mandatory BX
C++ consumer definition is an explicit assertion; actual provider evidence
is separate. These controls do not establish shader linkage, pixels or performance.

## Graphics selection

The XP producer explicitly selects the separate `compatibility` graphics tier:
private provider selector 3, with OpenGL 2.1 and OpenGL ES 2.0 API floors.
The public API exposes compatibility as 4 and preserves its historical
`COUNT = 3` sentinel. Existing stable, modern and current tier values and
defaults remain unchanged. The Vulkan compatibility
floor remains 1.0. A floor is package/context policy, not a statement that every
shader, feature or platform has been qualified at that floor.

The selected tier partitions the bgfx binary identity. The root Conan generator
forces `JCE_SHADER_COMPATIBILITY` ON or OFF when reusing an object tree. The
shader cooker adds its compatibility definition only to GL/ESSL graphics
commands; other backend and compute commands retain their existing identity.
Installed SDK configuration carries the producer's shader policy to consumers.
GLES 2.0 still requires its own profile, stock and loader qualification; XP
OpenGL acceptance cannot establish that separate target.

Compatibility packages select the pinned upstream's existing 2D color blit
emulation, with a separate package-policy identity. This provides the FBO
attachment/copy path on drivers lacking copy_image; it does not provide
cube, array, 3D, depth or multisample copies. Native copies remain preferred
when available. The normal graphics tiers keep their previous defaults.
Actual format/capability admission, visible output and pixel readback must
still be tested; neither a build flag nor a ready-frame counter proves pixels.

### OS floor and renderer requirements are independent

Both Windows products use the shared Auto order D3D12, D3D11, Vulkan,
then OpenGL. The XP+ single EXE can move between systems. It checks the current
host each time Auto runs. System D3D and Vulkan libraries load optionally,
and each actual device or swap-chain failure advances to the next candidate.
D3D12 requires a compatible DXGI Factory4 host; D3D11 requires compatible
DXGI and a Shader Model 5 hardware device. Vulkan requires a system loader,
Win32 presentation support and an eligible physical device. On native XP,
the build retains its OpenGL 2.1 floor and compatibility stock cohort.
The MSVC defaults and its OpenGL build tiers are unchanged. Do not infer an
OpenGL version or a Direct3D feature level from an OS name.

The existing D3D stock cooker uses Shader Model 5.0. A D3D11 device accepting
feature level10_0 does not establish that those shaders can execute there.
DX10-class support would require a matched SM4 cohort and feature admission,
or a qualified supported fallback. There is no separate D3D10 backend claim.
See Microsoft's [feature-level table](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-devices-downlevel-intro).
The build lane, backend API, device feature level, shader cohort and optional
features are distinct recorded requirements.

## Runtime boundaries

Do not raise the legacy OS floor to make newer declarations visible. Optional
newer services use existing capability/refusal paths. For example, Windows CCD
display-ownership observation requires Windows 7 declarations and is unavailable
for an XP target; this retains the renderer's ordinary adapter fallback.
Platform-specific behavior stays behind the existing OS adapters and C ABI.

The explicit XP graph uses the compatibility graphics tier and embeds
`glsl`, `dx11`, and `spv` stock shaders. Both D3D backends use the same
Shader Model 5 assets. The compatibility stock restrictions apply to GL
and ES. D3D11, D3D12, DXGI and Vulkan remain outside the ordinary and delayed
XP PE imports. Verify each backend with the exact delivered EXE and actual
driver, including GL fallback on XP.
Embedded Lua/JavaScript and native C/C++ are the selected shipping languages;
an unavailable required language must fail packaging rather than disappear.
Optional private AI dispatch and external Python/Java/C# runtimes are excluded
from this compact XP shipment. Their exclusion does not disable ordinary public
SDK transports or claim support for an untested backend.

The XP graph uses official QuickJS-NG 0.4.1 from an immutable source pin;
modern targets retain 0.16.2. The legacy provider does not provide Atomics or
Worker support. Its explicit exception capability lets the existing backend
consume an already-observed exceptional result without the newer
JS_HasException query; thrown null and undefined still require consumption.
This provider difference is not feature parity or complete Editor acceptance.
The package reports its actual version and retains its original MIT notice.

Flecs uses its official NO_HTTP, NO_REST and NO_OS_API_IMPL configuration in
the explicit XP graph. The private Scene adapter installs Core-backed OS
callbacks before the first world and refuses foreign or late ownership. This
keeps ordinary ECS functionality without changing upstream source or exporting
a second public scene API. Modern Flecs configuration remains unchanged.
The unused XP profiler client is disabled through its supported package option.
Zstd keeps compression/decompression with its internal threading and unused CLI
programs disabled through the official package options; modern targets retain
their existing defaults. JCE does not request the internal worker API.

The explicit XP graph selects official Abseil 20250814.2 within the supported
Protobuf dependency range; modern resolution is unchanged. Its optional ICU
path is header-gated, unlike the newer unconditional discovery implementation.
This is not complete Windows IANA time-zone discovery: the legacy MinGW
fallback can use UTC when no configured zone data is available. JCE public
local-time formatting continues to use the existing Core/CRT owner.

Native build-host tools use the observed installed MSVC toolset update in a
small owned profile. This separates incompatible artifacts hidden by Conan's
coarser compiler-version family. Explicit build profiles remain supported;
audit cannot create a missing default profile. Profile selection records input
identity, not execution or complete toolchain provenance.

Mimalloc retains its official CRT-TLS and RtlGenRandom configuration on XP.
The private exclusive-SRW adapter is allocation-free and independent of JCE's
allocator. Separate content-addressed codec and stdio header cohorts do not
broaden that consumer or relabel old dependency archives. Generated dependency
configuration pins reviewed versions, guards conflicting flags, and partitions
changed package identities while leaving modern builds unchanged.

Reviewed codec consumers admit infinite exclusive condition waits and
synchronous flags-zero once initialization with NULL context. Missing processor
group affinity returns unavailable without inventing a mask. Resource failure
inside a supported wait stops the current process because these codec callers
ignore wait errors. This is a private subset, not full Windows API equivalence.
A codec's generated declaration floor may be higher than its entry profile;
actual final import coverage remains mandatory.

For the reviewed x86 Meson codec, resource compilation explicitly uses the
same selected toolchain's i686 windres through package build configuration.
CMake's rc selection alone does not select Meson's resource compiler. This
consumer-specific binding participates in package identity while the global
profile and ABI remain unchanged. Final resource-object and executable
architecture still require actual validation.

The private stdio consumer port retains system-MSVCRT FILE objects and Unicode
paths for standard byte-file modes. It rejects unsupported secure/Unicode-stream
modes and does not emulate secureCRT sharing or invalid-parameter handlers.
BGFX instead uses miniz's supported MINIZ_NO_STDIO configuration for its unused
file-archive helpers; memory compression and image paths need separate checks.
Its BX C++ consumer probes the already-loaded Kernel32 for the optional large-page
minimum query, delegates the real function when present, and returns unsupported
zero when absent. Normal-page queries stay unchanged. This is not large-page
allocation qualification, and the strict final import policy stays in force.

The reviewed XP x86 Clang 22 BGFX configuration uses upstream BX's separate
translation units through BX_AMALGAMATED=OFF. It retains optimization and
features, avoids the observed amalgamated Settings frame-index failure, and
binds this configuration in package identity. No original source is patched.
A successful isolated object does not qualify the full provider or Editor.
The XP-only BGFX provider adapter removes the pinned BX target's SSE4.2
option, selects its reference SIMD implementation and keeps ASTC at SSE2.
The producer includes the reviewed CMake adapter and selector in the Conan
package identity. The Win7+ provider keeps its own build configuration.
The NT 5.x PE floor, the BGFX compiler target, guest execution and old-device
acceptance are separate checks. A 1-core, 512 MiB CPU test must record actual
guest-visible CPU and RAM. Use an instruction-enforcing emulator for an old CPU
control and a separate physical or hardware-accelerated GL test for pixels and
frame timing.

## Acceptance and evidence

1. Validate compiler/runtime, architecture, instruction and package identities.
2. Build the complete selected dependency graph and native Editor with stock
   shaders, assets and notices embedded.
3. Check GUI PE identity and every ordinary/delayed imported name and ordinal,
   following forwarders against exact guest DLL exports. Reject companion
   runtime DLLs and known post-XP imports.
4. Copy the sole EXE into a fresh guest directory, use a fresh isolated Editor
   profile, and verify startup, bounded execution, exit and relocation. Pinned
   SSH/SFTP receipts identify the artifact bytes and guest. Credentials remain
   runtime inputs and never enter receipts or source.
5. Verify actual GUI output, input, scripting and project/Bundle lifecycle on
   the target. Process exit and CPU-submission counters do not certify pixels,
   GPU completion, smooth playback or usability.

`xp-probe` exercises a limited core/C++ runtime subset. The separate resource
embedding fixture proves COFF/RC payload and subsystem linkage. Neither
configures or qualifies the full Editor. Generated logs, inventories, captures
and receipts belong under ignored `build/validation/`; this document defines
the reusable policy rather than storing machine-specific acceptance notes.

## Unified native Editor Release/Dist cache layout (v0.14.1)

For native single-executable Editor targets (ordinary Windows x64/x86 and the
separate LLVM-MinGW-XP x64/x86 lane), each Release/Dist variant owns its own
CMake tree and Conan-generator output:

    build/desktop/<target>/obj/<variant>/       # CMake + Ninja objects
    build/desktop/<target>/conan/<variant>/     # Conan generators + toolchain
    build/desktop/<target>/<variant>/jce_editor.exe

`<variant>` is either `release` or `dist`. The XP target retains its own
LLVM-MinGW/MSVCRT package store under `build/compat` and never links MSVC host
libraries into the guest Editor. Host-side generators under `build/host` are
not deliverables. `dist/editor/` remains the separately verified single-EXE
package location. No companion DLLs or game-asset directories are introduced.

The variant directories are independent, so a Release opt-out of AAC/H.264/H.265
cannot carry into Dist and Dist's forced OFF cannot alter Release's cached
preference. Release defaults ON for a fresh cache and permits explicit OFF;
Dist always forces OFF. Build/variant selection and codec preference are
independent inputs; this layout does not change vendor licensing obligations.

Historical trees such as `<target>/obj`, `<target>/conan`,
`<target>/obj-release`, `<target>/obj/conan` or `<target>-standalone` are NOT
renamed, deleted, or auto-adopted. A first new build uses the new paths and may
recompile. Remove historical trees only after separate user-directed review.

The XP `receipt.json`, optional real-time progress and guest compatibility qualification
remain separate; consistent folder names are NOT evidence of successful builds
or XP VM execution.
