# Build and validation

Windows desktop dependency API floors are separate compilation inputs. The
existing CMake policy hook selects PhysFS WINVER/_WIN32_WINNT=0x0601 and records
the policy in its package identity. Refresh through an owned configuration home
sharing the original store; never rewrite the old package or merely relink it.
Validate actual compiler definitions and the new executable's required symbols.
The shared PE reader reports normal/delayed symbols and unknown ordinals; its
four-name Windows compatibility refusal is bounded, not full target validation.

Use the supported CLI and target options; scripts/jce.py forwards to tools/build/jce.py.

After selecting an isolated SDK cache, keep both `--build-dir` and
`--install-dir` explicit on subsequent builds. Omitting them selects the
compatibility defaults, which may reconfigure the desktop Editor cache and
publish a different SDK prefix. Confirm the actual configure/install paths
before assigning a run to the intended artifact's qualification.

```bash
python scripts/jce.py --help
python scripts/jce.py targets
python scripts/jce.py lint
python scripts/jce.py test --arch x64 --jobs 8
python scripts/jce.py test --configure --variant debug --arch x64 --jobs 2
```

The lint command runs source gates and the architecture audit. Missing private suites are unavailable, not PASS. Run focused checks during iteration and the required complete suites before delivery.


The native stock CMake controls need an explicit wrapper input. Run
`python tools/lint/run_all.py --stock-wrapper-exe <native-jce_bin2obj-path>`
and `python tools/audit/run_architecture_audit.py` for the complete gate pair.
The source aggregator does not guess build/dist artifacts. Without the wrapper
that subject is NOT_MEASURED/SKIP; it is not a native-wrapper PASS. Actual controls
also require CMake. `scripts/jce.py lint` retains this explicit-input omission.

Public-header changes require an SDK refresh and consumer smoke validation. Binding changes require both generators in check mode. A committed ABI check inspects HEAD; a pending working-tree baseline does not make an older HEAD pass.

For the current SDK/tool discovery boundary and complete host/target payload requirements, see [toolchain discovery](toolchain-reference.md). Keep source checks, archive admission, actual target builds and zero-environment delivery as separate evidence.

Fetch original dependencies only from immutable pins in contracts/vendor-sources.json. Verify existing originals; never repair an upstream checkout or Conan package. Configuration-only Conan hooks respect CONAN_HOME. A cached binary build does not prove pristine source or cross-platform compatibility.

Native MSVC activation preserves the existing JCE_SHADERC_EXECUTABLE and
BGFX_SHADERC host-tool selectors in both pristine and already-activated shells.
Use the supported selector when choosing a validated host shader compiler
independently of target runtime build type. Keep the full compiler, argument
and include-path/content cache key; changing the tool or include root can
require a first compile. Validate actual tool selection and target outputs
separately from environment forwarding.

The bgfx CPU profiler is a dependency compile choice, off by default. For an
explicit diagnostic build, set `JCE_BGFX_CPU_PROFILER=on`; explicit `off` restores
the default. Empty/other values are rejected. The shared bgfx Conan arguments
pass `user.jce:bgfx_cpu_profiler=off|on`, which enters the package identity and
the existing generation-reuse fingerprint. The configuration hook merges the
real `tools.build:cxxflags` definition `-DBGFX_CONFIG_PROFILER=0|1`, preserving
caller flags and refusing conflicting definitions.
The same definition also uses Conan's `tools.build:defines` directory channel
to survive upstream Apple/Ninja's global C++ flags reset. Enabled packages add
a CPU-profiler configuration revision to their identity; off adds no CPU revision.
The independent annotation policy below also partitions package identities.
A CMake cache variable,
runtime debug flag or registered callback alone cannot enable these scopes.
Generated package metadata exposes `JCE_BGFX_CPU_PROFILER_COMPILED=0|1`; an
absent macro means unknown, and absent callbacks do not prove compiled-off.

The optional GPU annotation selector is `JCE_BGFX_DEBUG_ANNOTATION=auto|on|off`,
defaulting to `auto`. The shared driver passes `user.jce:bgfx_debug_annotation`
through the same Conan/fingerprint and clean-MSVC environment paths. Auto keeps
a caller's upstream CMake option, otherwise upstream's OFF default; explicit
on/off select that option and refuse conflicts. They do not override the
upstream target expression: annotations compile when the target is not
WindowsStore and either the configuration is Debug or the option is ON.
Consequently Debug plus off still compiles annotations, and WindowsStore plus
on still excludes them. The hook preserves unrelated flags and typed caller
options, refusing conflicting reserved compiler/directory defines or undefines.
Only changed effective compilation or unresolved inputs add annotation option,
compiled-state and policy-revision package identity keys. Default auto, equivalent
off, Debug's forced ON and WindowsStore retain exact historical package IDs;
request metadata alone never rebuilds bgfx. Equivalent enabled selections share
one package ID. Existing sources/packages are never repaired.
Package metadata exports `JCE_BGFX_DEBUG_ANNOTATION_REQUEST` (-1 auto/0 off/1 on),
`_OPTION` and `_COMPILED` (-1 unknown/0/1). Missing metadata and unresolved
custom-toolchain/configuration inputs stay unknown. The existing renderer
attempt log/trace publishes these fields; runtime debug flags, CPU callbacks
and compiled support do not prove a native GPU marker or timestamp. Confirm
actual compile definitions and named-pass captures separately for each backend;
diagnostic artifacts must not silently replace normal-performance baselines.

If an installed first-party hook differs, keep the original home/hook intact.
Native default editor/SDK/host-tool builds now select a small owned
`build/conan-config/native-<identity>` overlay automatically for this conflict,
without requiring a caller CONAN_HOME. Its identity derives from the original
home and current hook/policy bytes; the same tuple reuses the same directory and
effective package store. Explicit caller homes retain unknown-hook refusal.
Configuration/profile/temporary-file aliases and unsafe inputs refuse before
publication. No package/source/auth/database tree is copied.
Installed-hook lint uses the same read-only `select_hook_home` policy and checks
the actual selected overlay. It creates nothing: missing hooks are reported as
not installed, never as build-execution proof. Explicit or selected-overlay
stale hooks still fail, including retired source-writing hooks.
Use `tools/build/conan_config.py`'s existing
`prepare_configuration_home(base_home, overlay_home)` for a separate owned
Native configuration home, then pass its returned `home` as `CONAN_HOME` only
to the selected build's existing hook/export/install flow. Native preserves
the actual settings/settings_user bytes and adds no compiler version. The
overlay shares the original effective `core.cache:storage_path`; it does not
copy authentication or package/source/database contents. New hooks go into the
overlay, with the existing credential, size, overlap and ownership checks.
Web instead uses `prepare_toolchain_home` with the probed actual Clang major;
never supply a fabricated major to prepare a Native overlay.

Conan generation reuse automatically includes a digest of the effective
`CONAN_HOME`, including the default home. Native same-home configuration
refreshes invalidate reuse without a manually supplied digest; Web retains its
existing toolchain identity input. The bounded scan reads only `settings.yml`,
`settings_user.yml`, `global.conf` and files under `profiles`: at most 1 MiB per
file, 4 MiB of profile contents and 4096 profile directory/file entries. Missing
inputs have explicit values; permission, directory-scan, type, redirection or
budget failures refuse reuse. Package, authentication and database contents
are excluded, as are profile includes outside this selected scope. The digest
describes the files read during the scan, with no atomic-snapshot guarantee
while another process changes configuration. Keep that configuration unchanged
during generation.

Retain actual dependency compilation and linked-artifact identity alongside
any observed runtime callbacks. The compile selector is separate from runtime
capture controls: configuration metadata, passing CPU fixtures and callback
wiring do not prove that a sampling session ran or met a performance target.
An SDK consumer must rebuild through `app --sdk <selected-prefix>` to link the
selected diagnostic package; environment flags cannot change an old EXE.
The embedded Editor uses its existing source-closure build, not an SDK-prefix
switch. Preserve compile inputs and verify the linked executable's actual
capability/callback observations before attributing backend CPU time.

Fresh test trees use `test --configure --variant debug|release`. The CLI defaults
to checking host packages; CI explicitly sets JCE_SYSTEM_PACKAGE_MODE=install
and JCE_SYSTEM_PACKAGE_SUDO=1. Native tool names and Debug CRT policy follow
the target platform.
Native Linux dependency metadata requires a working pkg-config executable.
Check actual execution, especially when WSL inherits Windows tool directories.
CMAKE_BUILD_PARALLEL_LEVEL controls both compiler builds and Conan's host/build
dependency contexts; the shared default is eight. Native host asset-tool refresh
uses the same Conan budget, and clean MSVC activation preserves the selector.
CTest has a separate
--jobs/CTEST_PARALLEL_LEVEL budget. Preserve these limits on resource-constrained
hosts and exclude concurrent builds from formal performance measurements.

For a controlled native embedded SDK configuration, `sdk` and `package sdk`
accept paired `--build-dir <build/isolated-cache>` and
`--install-dir <dist/sdk/isolated-prefix>` with
`--embed-engine-shaders auto|on|off`. Custom paths must be independent strict
children of those roots and cannot overlap default SDKs or shared build caches.
Custom `--clean` is refused. Explicit OFF requires this pair and removes stock
shader capability; the consumer supplies required shaders. Native auto/on
explicitly restores shipping ON. Authoring, Web and cross targets reject custom
paths or non-auto selection. The shared static producer and kernel leases remain
authoritative. A bounded atomic request/state stamp precedes configure so the
same owner can retry an ordinary first failure; a present CMake cache must fully
match source, output, toolchain, variant, delivery and shader identity. Installed
reuse also requires the existing compiler-generated target metadata and archive,
plus matching `sdk-build-options.json`. Request stamps and counted command-port
tests do not certify archive ABI, native execution or pixels.

Supported build entry points live in scripts/. Compiler and dependency commands
are constructed by the shared tools/build implementation; do not hand-write a
second build recipe. Reuse a compatible target/variant/toolchain cache. Isolate
only real ABI/configuration differences and avoid full source/build copies per
trial. Check disk headroom and distinguish shipped application size from PDBs,
static SDK libraries, dependency caches and retained evidence. Generated test
artifacts go under ignored build/; source tests stay under tests/. Keep original
dependencies and referenced raw evidence unchanged. GPU validation must not
steal focus; use hidden/nonfocused windows only when rendering is necessary.

Native Dist content preparation uses the same public Bundle graph as admission.
The Editor passes the selected SDK stock root to the private graph adapter;
shader fallback preserves existing mounted/project precedence and resolves
relative SDK configuration to an absolute host path. Retain real producer ERROR
messages and dependency chains in terminal headless results. A successful SDK
packer run does not prove the Editor build route or the delivered application.

Dist name confidentiality is declared separately from project payload security.
The optional `confidential_asset_paths` node in `jce_project.json` is read through
public capped host IO and strict JSON before graph preparation: 1 MiB per manifest,
256 canonical relative addresses, 1023 bytes per address, and 64 KiB in total
including one terminator per address. Duplicate policy keys/addresses, empty or
non-string entries, NUL, wildcard, escape, noncanonical spelling and addresses
outside the selected graph refuse admission. All project Dist PAK entries still
require encryption, authentication, verified decoded bytes and graph parity;
GUI and single-file requirements remain. An explicitly protected name literal
outside that PAK is fatal. Ordinary selected API address literals are recorded
separately in `address_literals_outside_project_pak`; their presence does not
establish plaintext payload leakage or runtime use. Public project save preserves
the original optional node without a public ABI field or private path-schema
validation. Its preservation reader refuses raw/active encoded NUL, duplicate
policy keys and non-JSON suffixes without rewriting disk bytes; literal escaped
backslash text such as JSON `"\\u0000"` remains preservable.

Bundle and Dist share the private prepared-config adapter through public Bundle
`always_included` roots and resolver composition. Existing canonical
`render_settings.json`, `physics_layers.json`, `settings/project-settings.json`,
`settings/render_pipeline.rp.json` and
`settings/input_actions.json` become required roots and retain descriptor/LUT
closure. Absent optional files are not invented. Public VFS -> source -> resolver
precedence remains; prepared canonical fallbacks precede project Settings/.jce
fallbacks and never override canonical source bytes. The manager supplies its
actual prepared root and Dist avoids a second input-actions append. Verify source
versus prepared conflicts, prepared versus authored fallbacks, closure and a
selected source disappearing after binding. Project settings use the authored
`.jce/project-settings.json` fallback; physics layers use their prepared canonical
file. Missing optional files stay absent, while an omitted present project-settings
root is a failure even when all scene assets were packaged. The graph/audit/project fixtures
and actual adapter missing-wire/omitted-root controls establish their CPU scope;
inspect the current EXE's secure entries and reopen its current Bundle separately,
then validate relocation, settings, input and pixels with matching artifact hashes.

DIST stock shader packaging supplies `--strip-debug-paths` to the existing PAK
producer. This removes the recoverable stock debug path table while retaining
hash addressing. Stock remains a public archive; this flag does not encrypt its
payload, select a minimal stock closure or establish a size target. Inspect the
actual rebuilt archive and embedded bytes. Source policy or a passing fixture
cannot relabel an older EXE/Bundle or certify current release acceptance.

Headless terminal results add the accepted build's private `log_path`, `log_status`,
`log_complete`, truncation/incomplete flags and independent diagnostic error.
The bounded atomic log is normalized owner-observed text, not byte-exact child
streams or an EOF receipt. Read the actual named file and match its bytes before
using it; a declared artifact/audit path need not exist after a failed build.
Diagnostic failure preserves the real build state, exit and error; refused
starts cannot certify a previous transcript.

The installed SDK deferred scripting hook preserves the target's verified
language/factory declarations and original single generated shim source.
Required native metadata cannot invent factory providers; missing or mismatched
declarations still fail. Configure controls and SDK smoke must be followed by
the actual project build and runtime checks.

Web application policy must precede every prebuilt OBJ/ASM/C asset return.
Release and dist both emit one HTML; single-file admission rejects external JS,
wasm and data before delivery. Validate the actual recipe, not just flags in a
manual asset branch. Match consumer variant and the installed SDK variant.

Web DIST prebuilt assets use `JCE_PROJECT_PREBUILT_ASSETS_PAK` with the existing
`/game_assets.pak` MEMFS boot path; an ASM blob alone cannot boot Web. Native
prebuilt ASM explicitly enables ASM. Clear all stale prebuilt selections on
every app configure, including a switch back to release.

Before each prebuilt pack, an explicitly authored root
`Settings/RenderPipeline.rp.json` is synchronized byte-for-byte to cooked
`settings/render_pipeline.rp.json`, matching the normal CMake path. Source
changes must reach the next archive; repeat packaging keeps unchanged content.
Without a root RP, preserve an independently supplied cooked canonical policy.
Do not add a second copy of the canonical resource key to the boot root.
Verify actual PAK entry size/CRC and uniqueness; desired JSON alone does not
prove that the executed GPU path enabled the requested feature.

`app --registry-proof <receipt>` verifies an existing embedded Web artifact
without rebuilding; DIST fresh key shares would otherwise change its hash.
SDK/profile, current closure and actual nonce/request/hash checks still apply.
It rejects missing artifacts and clean/configure-only/native conflicts.

## Test domains and evidence kinds

The configured CTest registry classifies each maintained test by source owner
(`domain:engine`, `domain:editor`, `domain:consumer`) and evidence kind
(`kind:unit`, `kind:integration`, `kind:graphical`, `kind:contract`).
C++ does not imply editor ownership. Raw tests and script/variant registrations
use the same metadata authority in tests/cmake/JceTestMetadata.cmake. Missing
or conflicting metadata rejects configuration. Standalone SDK consumers keep
local declarations and do not require a repository sibling helper.

```bash
python scripts/jce.py test --suite all --arch x64 --jobs 2
python scripts/jce.py test --suite engine --arch x64 --jobs 2
python scripts/jce.py test --suite editor --arch x64 --jobs 2
python scripts/jce.py test --suite consumer --arch x64 --jobs 2
```

The default `all` selects the complete configured registry. Build inputs use the same call-site domain authority: `all` builds `jce_tests`, and the other suites build `jce_tests_engine`, `jce_tests_editor` or `jce_tests_consumer`. Shared/raw/script/tool inputs are wired through `jce_test_build_dependencies`; deferred checks reject missing or unclassified edges. Explicit `--target` overrides the build aggregate; `--filter` only selects registered CTest cases. Domain selection must retain the complete configured inventory and SKIP semantics.

 A configured count
is not a pass count; retain optional unavailable suites separately. Parser,
metadata, fake-port and Noop controls do not prove graphical execution.
Set CMAKE_BUILD_PARALLEL_LEVEL separately when limiting compiler/Conan work;
`--jobs` limits CTest concurrency. Run hardware/UI acceptance serially with
current artifact identities, isolated profiles and retained failing controls.

Only an unfiltered complete `all` selection runs the additional cook/colour
acceptance. Explicit targets, regex filters, single-domain suites and rerun-failed
must not expand the build scope after CTest. A repeated complete all selection
retains the acceptance. Preserve the overall command result separately from a
passing CTest subset if a later step fails or is interrupted.

Installed SDK target admission uses compiler-derived metadata beside the exact
configuration archive: lib/<CONFIG>/jce-sdk-target.json. The editor, build CLI
and CMake consume that same authority before cooking or cache/output mutation.
System, architecture, pointer width, configuration, variant and resolved CRT
and delivery must agree; renamed directories and VERSION.txt are not ABI authorities.
The exact nine-field record is bounded to 16 KiB, including before decoding.
The selected archive metadata is the sole SDK variant authority.
Schema 1 is generator-owned canonical ASCII scalar JSON; escaped keys/values,
embedded NUL, duplicate/unknown fields and partial reads fail admission. This
restriction does not apply to ordinary authored project JSON.
A missing/oversized/invalid metadata record requires an SDK refresh. Metadata
admission is distinct from inspecting and running the actual linked artifact.
The current native editor shipping policy is embedded for release/dist and
authoring for debug; it rejects a conflicting SDK profile explicitly.

Formal installed-SDK gates are `test_jce_sdk_consumer_c99` and
`test_jce_sdk_consumer_scripting`, classified consumer/integration. Select an
explicit prefix through the shared test driver, independently of Debug native
tests:

```bash
python scripts/jce.py test --configure --arch x64 --variant debug --suite consumer --sdk <installed-sdk> --sdk-variant dist --sdk-delivery-profile embedded --jobs 1 --filter "^test_jce_sdk_consumer_"
```

Missing selection reports SKIP (77), never PASS. An explicitly invalid SDK or
incomplete declared backend roster fails. These gates consume installed archives
and require actual native Noop execution; protocol ports and a cross build are
separate evidence. Runtime SDK toolchains remain a separate delivery requirement.

Scripting availability is generated from the installed roster and actual target
graph: JCE_SCRIPT_AVAILABILITY_SCHEMA=1, JCE_SCRIPT_CATALOG_LANGUAGES and
JCE_SCRIPT_COMPILED_LANGUAGES describe optional languages beyond built-in Lua.
An installed catalog does not prove a compiled backend. Strict consumers match
the selected Release dependency closure to the compiled set; a sibling Debug
archive cannot fill a missing Release payload. Uncompiled optional languages
report UNAVAILABLE; missing or malformed declared metadata requires SDK refresh.

Explicit optional test dispatch selection uses `test --configure --ai-dispatch on|off`. Omission preserves the cache selection. Supplying it without configure is rejected before cache or build mutation; registry availability and an actual passing runtime suite remain separate evidence.

Native `sdk --smoke` passes the SDK's explicit variant/delivery profile and
requires actual runs of both C99 and scripting consumers. Their DIST assets use
the same existing authenticated graph/PAK/key-share builder; production prebuilt
admission remains required. Historical raw-resource runs without the requested
variant/profile are compatibility evidence, not authenticated DIST proof.
Cross/Web execution remains pending until its target is actually exercised.

The bounded fixture stages C99 PNG/WAV or the exact `tests/sdk_smoke_scripting/assets/scripts/smoke_py.py` source
inside fresh consumer scratch. The script's public type is `script` and its
representation is `python.source`; path, bytes and fixture kind must agree.
Missing selected-SDK tools or source/type/graph/BOM mismatches refuse execution,
and the current proof is rechecked after linking. Release smoke retains its
existing cook/pack path; switching clears cached prebuilt selections.

`tests/sdk_smoke/test_stage_dist_fixture.py` checks real-file staging and typed
proof protocol. `tools/tests/test_sdk_consumer_gates.py` checks the actual driver
using counted compiler/proof/runtime ports, including failure before launch.
These CPU controls do not prove actual authentication, linkage or execution.
Native consumers retain cooked decompression, declared-backend lifecycle markers
and exit-success gates. Fixture success does not certify a shipping minimal
closure, editor GUI, project relocation or device presentation.

Declared Bundle projects use ordinary `app <project> --variant dist --arch x64
--delivery-profile embedded --sdk <matching-installed-sdk>`. No artificial loose
startup scene is required. The actual public inspector reads the internal typed
manifest and deep-verifies the archive; explicit startup takes priority, otherwise
runtime declaration order selects the first Bundle scene. Every declared asset
participates in language and native-registry admission. Unsupported scripts,
conflicting identities/payloads or incomplete proof refuse acceptance.

The final authenticated PAK includes the validated Bundle entries and real boot
scene; this build clears separate plaintext Bundle embedding. Optional sidecars
are not shipped requirements or typed authorities: absent is legal, present
invalid fails the public reader, and explicit legacy compatibility is UNVERIFIED
for archive hash binding. Source/sidecar/inspection snapshots remain stable
through packing and linking. Older SDK tools without typed inspection need
refresh. Counted CPU protocol controls are distinct from real Archive/Bundle
validation, native EXE relocation and actual scene/input/pixel acceptance.

Build Profiles defaults user projects to Dist and keeps explicit Release
selection. SDK projects cook assets and configure/build with CMake/Ninja; only
engine workspaces use their script/Conan authority. Build leaves the executable
in its build cache; Pack publishes the verified Dist single executable under
`dist/apps/<name>/<platform>-<arch>-dist/`. Explicit Release packaging may include
development companion files. Select an SDK whose variant/target contract matches;
never silently downgrade Dist to reuse Release. Idle rejected requests publish
FAILED and clear previous outputs; busy reentry preserves the current request.
Play requires dispatch acceptance before observing build success. The actual-body
CPU admission and old-wire controls do not certify GUI clicks or relocation.

Unknown annotation package identities include a bounded canonical JSON/SHA256
of the already-consumed configuration requests, including relevant overrides,
flags and user-toolchain request paths. Different unknown requests are isolated;
equivalent known defaults retain their historical package IDs. No toolchain
file content is read or authenticated, so an in-place external file change is
not certified by this request fingerprint.

The audio dependency selects original miniaudio0.11.25, including its released
WASAPI COM reference fixes. Refresh through normal Conan resolution and rebuild
all header consumers; never patch a cached header or alter MA_COINIT_VALUE to
hide an apartment mismatch. The Windows CPU regression consumes the complete
selected upstream function with typed external ports:
`python scripts/jce.py test --configure --suite engine --variant debug --arch x64
--jobs 2 --target test_jce_miniaudio_com --filter "^test_jce_miniaudio_com$"`.
S_OK/S_FALSE each own one reference; failed initialization owns none. Preserve
ordinary failures from the old implementation. Header/function hashes and CPU
controls do not replace native audio, Engine teardown and actual Editor exit.

### Build-only SDK staging

For a current native embedded SDK without publishing dist, use `sdk` or
`package sdk --staging-dir <absolute-repository-build/sdk/owned-prefix>` with
one release/dist variant. This explicit mode reuses the compatible canonical
cache and existing leases; it excludes isolated output pairs, clean, cross/Web,
authoring and shader OFF. Matching source/toolchain/variant/profile/shaders
remain required. Refuse alias/reparse paths and unowned or partial nonempty
prefixes. Installed target/archive and output-options metadata admit reuse;
retain a failed install and select a fresh owned prefix. Default and isolated
outputs remain unchanged. This does not supply an offline compiler or certify
consumer execution. Keep actual SDK build/product identity separate from tests.

### Generated output placement

Follow contracts/build-output-layout.md. Reuse the current absolute native,
Conan, host-tool and shader cache owners. New benchmark evidence defaults to
build/validation/benchmark; lint/audit gate logs default to build/reports.
Explicit output paths and historical outputs keep their existing semantics.
Do not move a CMake tree or copy PDB/SDK/dependency trees for each trial.
Keep dist/editor, dist/sdk and project dist/apps as final delivery owners.
Reference an immutable verified executable across tests; never hardlink a
historical artifact to mutable linker output. Deleted evidence is unavailable,
not permission to reconstruct a historical PASS from current source.

The native Editor/app shipment default is one embedded executable; standalone
is a compatibility alias, not a second product class. Authoring is explicit.
Editor executables go to `build/desktop/<target>/<variant>`. New native
Release caches use `obj/` and `conan/` under that target. Compatible historical
trees stay in place under verified leases. XP targets use `windows-x86-xp`
and `windows-x64-xp`. Configure pins release/dist. Skip-build must match the
actual variant and shipping closure. Windows Editor Dist requires GUI PE
subsystem 2. Release allows the console subsystem. Optional authoring outputs
use the target's `authoring/` directory.

Without an explicit shaderc override, native configure first validates the current admitted CMake cache's absolute JCE_SHADERC_EXECUTABLE with the existing execution check. An absent, ambiguous, relative or unusable cached value falls back to the existing host-tool resolver. Explicit JCE_SHADERC_EXECUTABLE/BGFX_SHADERC priority and empty-value behavior remain unchanged. Reusing the compiler does not guarantee shader-cache hits: compiler bytes, input bytes and declared argument/input addresses still participate in keys.

Existing cache cleanup protects declared projects before deletion. Editor and
SDK `--clean` routes share a bounded, no-follow ownership scan of all requested
roots before removing either root. A nested `jce_project.json`, ambiguous roots
or traversal, unreadable entries and exhausted scan budgets refuse the clean.
Do not remove the manifest or move source merely to make cleanup pass. Existing
projects inside historical caches remain protected; this guard is not a source
migration or an atomic concurrent-filesystem guarantee. See the generated
output-layout contract for the exact boundary.
