# Generated output layout

Generated files are local output, never source authority. Unit-test source
remains under tests/ or tools/tests/; test work and new validation output belong
under ignored build/. Reuse a compatible build/cache owner before creating a
new tree. The embedded native Editor uses one canonical cache per target and variant;
legacy object paths remain untouched but are no longer auto-selected.

| Role | Location and lifetime |
| --- | --- |
| Persistent build and dependency caches | The native embedded-editor default is `build/desktop/<target>/obj/<variant>` (objects) and `build/desktop/<target>/conan/<variant>` (Conan-generated toolchain). Historical `<target>-release` / `<target>-standalone` object trees are not auto-adopted; leave them untouched. Other existing Conan/source caches retain their owners. Reuse only if the full owner identity admits it. |
| Generated test work | build/tests/ and existing owner-selected build/ children contain fixtures, temporary projects, test logs and test executables. Keep each run scoped to its own work directory; do not duplicate a complete source/build/SDK tree for a trial. |
| Bounded validation and raw evidence | New Core-5 output defaults to build/validation/benchmark/. Existing unique run/control directories, suite IDs and immutable content-key fixtures remain authoritative. Keep requested sample/time/size budgets, failed controls and availability distinctions; the directory is not a validity claim or an automatic retention service. |
| Gate diagnostics | The source runner defaults to build/reports/lint-failure.txt and the audit runner to build/reports/architecture-audit-failure.txt. The lint runner retains its existing failure-write/success-clear behavior for its current log only. Audit --log retains its explicit path and write-on-success behavior. |
| Delivery receipts | Keep manifests, hashes, provenance, relocation/audit results and failure diagnostics in the existing owner's build/ or explicitly selected evidence root. Link them to the exact final artifact and configuration instead of copying the payload for every receipt. |
| Final deliveries | Preserve `dist/editor/<named-target-executable>`, `dist/sdk/<target-tuple>`, and `<project>/dist/apps/<app>/<target-tuple>`. Their names and platform-specific payload/container rules remain owned by the existing delivery implementation. |

## Reuse and evidence boundaries

A CMake output executable is mutable: a later link may replace or modify it.
It is not an immutable artifact store. Do not hardlink immutable evidence or a
historical baseline to that mutable executable. Reuse an already verified
immutable artifact by identity when its owner supports that operation; otherwise
retain only the exact independently required artifact and its receipt. This
contract adds no artifact store, alternate build recipe or cache owner.

Default validation does not copy PDBs, the installed SDK, dependency caches or a
whole build/source tree into each run directory. Keep those files in their
original owners; create a separately requested delivery payload only when its
actual acceptance requires it. Count final application size separately from
debug symbols, static SDK libraries, caches and retained evidence.

## Migration and retention

This is prospective. Existing reports/, build/ output trees, explicit --out or
--log roots and referenced history stay where they are. Do not rename a CMake
cache directory, rewrite its absolute paths, recreate deleted history, or move
an existing Conan/source cache to make the tree look uniform. An explicit
caller-selected output root continues to override a default.

Retention and cleanup are explicit user-owned decisions. There is no automatic
history migration, retention sweep, cache deletion or new cleanup command in
this contract. The existing current lint failure-log lifecycle is unchanged.
Before an authorized cleanup, distinguish disposable test work from compatible
caches, exact historical artifacts and referenced raw evidence.

## Default single-executable policy

Editor and application shipments default to one native executable. The
--standalone switch is a compatibility alias for the same embedded policy;
--delivery-profile authoring explicitly selects the optional development bundle.
Editor executables use build/desktop/<target>/<variant>. New native Release
objects and Conan generators use obj/ and conan/ under the target. XP uses
windows-x86-xp or windows-x64-xp, with obj/ separate from the GUI candidate in
dist/. Direct SDK desktop project builds use their project's matching layout.
Configure pins the requested release/dist variant. Historical `<target>-release` and `<target>-standalone` native Editor
object caches are ignored, without moving or deleting them.
Skip-build requires the exact configured variant, embedded shipping flags and
configured output location. Foreign source/profile/CRT/compiler closures refuse.
Windows editor packaging verifies ordinary/delayed PE imports, embedded assets
and notices for both variants. Dist requires GUI PE subsystem 2. Release allows
the console subsystem. Console SDK inspectors retain their own subsystem.
Other-platform dependency/relocation validation remains explicitly unavailable.

## Source-aware cache cleanup

Existing Editor and SDK `--clean` routes share the bounded cache-ownership
inspection in tools/build/standalone.py. Inspect every requested root before
removing any root. A nested `jce_project.json` protects declared project source,
including malformed or unreadable manifests. Noncanonical/aliased roots,
unreadable traversal, directory/unknown Windows reparse points, changed directory
identity and exhausted entry/depth budgets refuse cleanup. Inspection never
follows child symlinks. This is a refusal guard, not an automatic source mover,
retention sweep, general-purpose source detector or concurrent filesystem lock.
Keep source projects outside disposable caches in new layouts; preserve existing
nested projects in place until their owner explicitly approves a migration.

## Unified build entry and progress (v0.14.1)

The documented Windows editor/host-tools/SDK/project/package shims, and the
Windows Android/Web public batch shims, enter `scripts/jce.py`. Linux and macOS
editor/host-tools/SDK/project/package shims already enter the same driver.
`jce.py` reports invocation BEGIN/END and per-command BEGIN/END, exit code,
elapsed time and a heartbeat (normally every 12 seconds) while retaining live
stdout/stderr. Set `JCE_BUILD_PROGRESS_INTERVAL` in seconds to adjust it.

The XP Editor has an additional stage recorder (full logs and receipt.json),
and does not claim static PE analysis is the same as executing on XP guests.
Web/Android platform-specific build recipes remain in their existing BAT files;
the new driver subcommands delegate to those recipes rather than duplicating
Conan/CMake policies. The macOS universal/iOS specialist scripts preserve their
existing target-specific orchestrators. The unified entry is a presentation
and exit-code contract, not evidence that every target has passed compilation.

For Windows XP x86/x64, `libhevc` uses an XP-compatible event/critical-section
condition-variable fallback. Modern Windows retains the existing native CV
branch. Cross compilation is separate from a target VM execution proof.

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

## Build top-level ownership and test-root consolidation (v0.14.1 v9)

The machine-readable authority for build/ top-level entries and the registered
Python probe producers is `contracts/build-output-roots.json`. The source gate is
`tools/lint/check_build_output_roots.py`; run it with `--check-existing` to
inspect a populated local build tree. The same check is in the public Lint
aggregate, after ordinary tests run, so unregistered top-level paths no longer
silently accumulate.

### Canonical generated tree

```
build/
  .locks/                     shared process locks (never remove while in use)
  host/                       native host CMake and its tools/
  host-conan/                 host dependency generators
  desktop/<target>/           MSVC/LLVM/Linux/macOS native target owner
    obj/<variant>/            CMake/Ninja objects
    conan/<variant>/          variant-specific Conan generators
    <variant>/                built editor, e.g. jce_editor.exe
  compat/                     LLVM-MinGW-XP and legacy platform caches
  conan-config/               managed Conan hook/config overlays
  shader-cache/               content-addressed shader cache
  automation/                 editor/game automation runtime state
  tests/<probe>-tests/        temporary test compiler and source workspaces
  tests/_legacy/              moved prior top-level test work (manual review)
  validation/                 receipts, logs, source backups, benchmarks
  reports/                    current diagnostics, not compiled output
  sdk/, web/, android/        other explicitly selected platform outputs
```

The v9 change is deliberately limited to *test workspace producers*.
Do not relocate `host/`, `host-conan/`, `desktop/`, `compat/`, `conan-config/`,
`shader-cache/`, `automation/`, `.locks/`, or a `CMakeCache.txt` by renaming
its parent: CMake/Conan and SDK receipt identities include absolute paths.
Nor does v9 change final `dist/` deliverables. The root set includes
only explicit platform owners known to the contract; a new owner must add
its definition and negative control before creating an additional top level.

### Migration and retention

The installer supports explicit `--organize-existing` on an idle checkout.
It inspects all existing *top-level* `build/*-tests` directories; empty roots
are removed, nonempty roots are moved without overwriting into
`build/tests/_legacy/<original-name>/`. Links and Windows reparse points
are refused. The historical data is not claimed safe to delete and no
compiler/process is terminated. Review retained files yourself later.
The default installer does not migrate any data. All build source files are
backed up in `build/validation/build-layout-v9/source-backups/` before changes.

Public Lint checks both static producer locations and the populated build root.
A fresh clone with no build/ directory passes this layout gate without inventing
or creating output. Native build, Python tests, ABI audit, and real XP guest
execution are distinct verifications; this directory policy certifies none of
them by itself.

## Desktop preset-cache normalization (v0.14.8 CI #59 follow-up; v10)

The **preset/SDK/test/authoring** path family and the existing **embedded
Editor/XP** family now have explicit, non-overlapping owners. The single entry
for builds remains `python scripts/jce.py`; Windows/Linux/macOS public shims
forward to it, while Android/Web/iOS specialist flows remain in their own
platform namespaces. `CMakePresets.json` and `tools/build/jce.py` use the same
preset path contract, verified by `tools/build/layout_contract.py --check`.

For desktop target `windows-x64` (other native OS/architectures use the same
shape):

```
build/desktop/windows-x64/
  obj/preset/release/        # CMake preset release (not embedded editor)
  obj/preset/dist/           # CMake preset dist; NOT shared with release
  obj/preset/debug/          # Debug test runner / CMake + Ninja
  obj/preset/release-sdk/    # SDK Release
  obj/preset/dist-sdk/       # SDK Dist
  obj/preset/asan/           # Windows x64 ASan preset
  conan/preset/              # Conan generators separated by build/{Release,Debug}
  obj/release/               # Embedded editor Release CMake objects
  obj/dist/                  # Embedded editor Dist CMake objects
  conan/release/             # Embedded editor Release generators
  conan/dist/                # Embedded editor Dist generators
  authoring/obj/<variant>/   # Optional authoring editor CMake trees
  release/                   # Editor executable output
  dist/                      # Editor executable output
```

XP x86/x64 uses `build/desktop/windows-<arch>-xp/obj/<variant>` and
`conan/<variant>`; its LLVM-MinGW/MSVCRT compatibility store and CMake caches
are not touched. Native application SDK consumers retain their separate
project-owned `build/desktop/<target>/<variant>` output. Web/Android/iOS and
host-tools caches retain their explicit owners; this change does not rebase
those toolchains.

**Migration is read-only.** Historical sibling trees such as
`windows-x64-debug`, `windows-x64-conan`, `windows-x64-sdk`, and the old
`windows-x64/CMakeCache.txt` are still on disk but no longer the default
preset targets. NEVER rename a directory holding `CMakeCache.txt` or Conan
absolute-path generators. Do not delete or overwrite legacy data as part of
installing this patch. A fresh configure may recompile dependencies and CMake
objects; that is expected and is not a reported build failure.

Run:

```
python tools/build/layout_contract.py --check --inventory
python -m unittest discover -s tools/tests -p test_build_layout_contract_v10.py -v
python scripts/jce.py test --configure --variant debug --arch x64 --jobs 2
```

`--inventory` classifies local historical roots without modifying them. The
unit-test and layout gates prove only path/wiring correctness, NOT native
compilation, SDK delivery, XP guest execution, or the full CI matrix. Native
unit tests remain required, with real failure statuses retained. The GitHub
Actions push/scheduled groups are independent so a scheduled audit cannot
abort the same-commit interactive push. Windows-heavy native matrix jobs now
allow up to 120 minutes; the job status still reflects real compiler and test
results and does not skip failed checks.

## Complete build-directory ownership (v11)

All first-party **new** build locations have one physical owner and no shared
CMake binary directory across target/mode/generator. The directory contract is
checked by `python tools/build/full_layout_contract.py --check`; to inspect
actual top-level and target directories without modifying them, use
`python tools/build/full_layout_contract.py --inventory --json`.

`build/desktop/<target>/obj/preset/<kind>` + `conan/preset` owns the CMake
presets, while `obj/{release,dist}` + `conan/{release,dist}` retains embedded
Editor ownership, and `authoring/` is the opt-in authoring Editor owner.
`windows-x64-xp` and `windows-x86-xp` retain separate LLVM-MinGW/MSVCRT
`obj/<variant>`/`conan/<variant>`; their compatibility stores stay in
`build/compat`. `macos-universal/` is a lipo output, not a CMake tree.

Other explicitly distinct roles:

```
build/host/                                 Native host CMake, tools (stable)
build/host-conan/                           Native host Conan (stable)
build/mobile/android-<arch>/obj/preset/release/       Android Ninja preset
build/mobile/android-<arch>/obj/platform/release/     Android CLI builder
build/mobile/android-<arch>/conan/{preset,platform}/   Distinct generators
build/mobile/android-<arch>/gradle/app/                Gradle compiled outputs
build/mobile/android-<arch>/staging/{jniLibs,assets}/  Gradle inputs
build/mobile/android-<arch>/release/                   APK output copy
build/mobile/ios-arm64/obj/preset/release/             iOS Ninja preset
build/mobile/ios-arm64/obj/xcode/release/              iOS Xcode project
build/mobile/ios-arm64/conan/{preset,xcode}/           Independent generators
build/web/wasm/obj/preset/{release,dist}/              Web CMake presets
build/web/wasm/obj/platform/{release,dist}/            Web BAT producer
build/web/wasm/obj/sdk/{release-sdk,dist-sdk,debug}/    Web SDK driver
build/web/wasm/conan/{preset,platform,sdk}/             Independent generators
build/jni/desktop/obj/preset/release/                   JNI Ninja preset
build/jni/desktop/conan/preset/                         JNI Conan generators
build/desktop/<target>/smoke/{c99,scripting}/<variant>/ Native SDK smoke
build/web/wasm/smoke/{c99,scripting}/<variant>/        WASM SDK smoke
build/consumers/stock-runtime/windows-x64-dist/        Stock consumer
build/sdk/                                         Isolated SDK staging
build/tests/                                       Test working trees
build/validation/                                  Evidence and patch backups
build/reports/                                     Diagnostics
build/.locks/                                      Active leases; do not delete
build/conan-config/                                Conan hooks/overlays
build/shader-cache/                                Content-addressed shaders
build/automation/                                  Runtime IPC/automation
```

The Android Gradle plugin now receives staged `jniLibs`/`assets` from its
specific target's `staging/` rather than writing them to the checked-in Gradle
project. Its default source layout is still available for Android Studio when
the explicit JCE build environment is absent. The generated Android
`local.properties` and Gradle's own `.gradle/` metadata are tool-specific
working files, not JCE's CMake/Conan/Gradle binary tree.

Final releases stay outside build/ under `dist/editor`, `dist/sdk` or the
consumer's `dist/apps`, and project-owned application caches stay in that
project's build/ tree. The Conan binary package store (`CONAN_HOME`) and SDK/
NDK/Xcode toolchain installations are external caches, not copies of JCE's
`build/` directory. Nothing is moved into JCE automatically.

**Migration:** Existing flat caches such as `build/desktop/windows-x64-debug`,
`build/desktop/windows-x64-conan`, `build/web/wasm-conan`,
`build/mobile/android-arm64-conan`, or CMakeCache.txt at a target root remain
in place. CMakeCache files embed absolute source/build and generator identities;
renaming or silently adopting them is unsafe. Start fresh in the new owner
and only remove a historical cache after verifying its source/receipt owner
and separately authorizing removal. Native compilation, linker, runtime,
Android APK installation, iOS signing, browser operation, and XP VM tests are
**not** established by this read-only layout gate.
