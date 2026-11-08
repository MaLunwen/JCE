# tools/build — automated build implementation

`stock_app.py` owns explicit Lua/JS runtime publication and single-file sealing;
`stock_runtime.py` owns the separate unsealed component producer through this
driver's public SDK tools, compiler environment, job budget and cache leases.
The fixed stock consumer cache is build/consumers/stock-runtime/windows-x64-dist.
Do not route an unsealed component through normal application's startup-scene
admission or supply a fake scene. Bind verified SDK product identity to actual
compile commands and publication provenance. Matching resource outputs may be
reused only after identity and deep public inspection, never by presence alone.
Final application directories contain the executable only; inspection work and
receipts take explicit outside paths. None of this certifies a bundled C/C++
compiler, another target architecture or actual relocated application execution.
Stock seal requires an explicit external key with strict decoded length/format
and a hash matching the compiled producer/protection identity, rechecked after
the bound public archive tools return. The manifest's original absolute key
path is producer provenance, not a location requirement: a byte-identical key
may be copied to a new caller-selected directory. Never publish the key bytes
or weaken authenticated archive/source verification for relocation. Python
controls use actual temporary copies with counted native-tool outputs; they do
not establish a native key, encryption or compilerless product execution.

AgentBridgeDriverControls in tools/tests/test_build_driver.py exercises the real
agent-bridge parser and fixed SDK-consumer command owner with counted command and
compiler-environment ports. It has no project manifest/target/language request;
application-only flags must not enter this route. Its old-wire ordinary failure
is CPU command-plan evidence, not native compilation or agent protocol execution.

`test --configure --variant debug|release` prepares a fresh native test tree
through the same Conan/toolchain selection as SDK/editor builds. CTest and the
colour-chain cooker use native executable names. Debug CRT settings are Windows
only. JCE_SYSTEM_PACKAGE_MODE defaults to check; CI explicitly chooses install
and JCE_SYSTEM_PACKAGE_SUDO=1. Preserve a caller's CMake policy override; the
default CMAKE_POLICY_VERSION_MINIMUM=3.5 reaches dependency CMake before its
minimum-version command, without editing upstream sources.
The shared compiler job budget also reaches Conan host and build contexts via
tools.build:jobs. It reads the passed environment's CMAKE_BUILD_PARALLEL_LEVEL;
dependency recipes must not silently replace a two-job budget with all CPUs.
Native host-tool refresh uses the same Conan job arguments. The clean MSVC
environment retains this selector before compiler activation, so the host CMake
build consumes the same budget. Actual owner/command-port controls include a
fresh missing-toolchain branch and ordinary omissions of each budget boundary.
CTest retains its separate explicit --jobs/CTEST_PARALLEL_LEVEL control.

Clean MSVC activation retains the existing JCE_SHADERC_EXECUTABLE and
BGFX_SHADERC host-tool overrides, including empty values, independently of the
runtime build type. Do not inherit unrelated compiler PATH/INCLUDE/LIB state.
Tool execution/admission remains with the shaderc resolver and CMake owner;
retaining an override does not certify a compiler, SDK or cache hit. The actual
MSVC environment fixtures cover both pristine activation and an already-ready
shell without launching native tools. Without an explicit nonempty shaderc
override, standalone configuration first execution-probes the absolute tool
recorded in its admitted active native CMake cache, then uses the existing
automatic resolver if that tool is absent or cannot run. Empty primary values
retain the existing BGFX_SHADERC alias behavior. Never consult another native
tree's remembered compiler or bypass explicit override failures. Counted real
configure/cache controls and an omitted-preservation negative exercise this
policy without native tools; stable selection does not certify shader cache
hits or recover older compiler provenance.

jce.py owns platform selection, Conan/CMake ordering, SDK installation and
consumer builds. scripts/jce.py is a compatibility entry point. Shared command
helpers and non-desktop recipes are implemented here; they take explicit
consumer/toolchain input. Never supply a named game's icon, target or content
as a reusable builder's default. fetch_vendor_sources.py only creates new,
hash-verified cache trees; existing originals are verified and never changed.

Android host packaging is `android/`. Original SDL Java comes from the
verified external source cache; no Gradle wrapper or SDL source is tracked.
Use an installed Gradle 8.14.1 and supply SDK/NDK environment settings.

Native single-executable editor builds are the default and owned by standalone.py;
--standalone remains a compatibility alias. Default executables go under
build/desktop/<target>/<variant>. New native caches use obj/ and conan/ in
the same target directory. Reuse a historical <target>-release or
<target>-standalone cache in place only after bounded
source/toolchain/compiler/architecture/static-CRT/embedded-policy admission,
repeated under the same cache lease. Never rename or copy a CMake cache.
Release/dist configure may reuse the same Release closure while always pinning
the requested variant; --skip-build requires that exact cached variant and
shipping flags. A conflicting canonical owner refuses reuse. Explicit authoring
development uses the target's authoring/ directory. Editor packaging holds the same
cache lease, configured artifact directory and final output path; revalidate
the selected artifact path after waiting. Both variants validate ordinary and
delayed PE imports. Dist requires GUI PE subsystem 2. Release allows the
console subsystem. Compiler identity uses the cache's active CMake
version; retained older version directories do not invalidate compatible objects.
The shared PE reader validates file-backed ordinary/delay descriptor, thunk and
hint/name ranges for the existing PE32/PE32+ architectures. Delivery rejects the
known post-Windows-7 imports GetThreadDescription, SetThreadDescription,
CreateFile2 and CopyFile2, even when their DLL is system-owned. Runtime symbol
lookup strings are not imports. Preserve the DLL-list interface and report the
bounded named-symbol policy plus ordinal imports with an unknown API floor;
this is not a complete Windows API catalog or Windows 7 execution certificate.
Validate embedded notice bytes against their source files before delivery.
SDKs and external managed runtimes are separate products. Do not replace native
single-file execution with a self-extracting wrapper or an embedded SDK bundle.
The explicit language set is Lua/C/C++/JavaScript; the full language build stays
available. Verify relocation with only the EXE and inspect actual captures.

Editor and SDK --clean share standalone.py's read-only source-ownership guard.
Under the existing cache lease, inspect both selected object/Conan roots before
deleting either. Roots remain resolved strict children of the allowed build
directory and their original spelling must not traverse aliases. The iterative
scan is capped at 100000 entries and 64 directory levels; unreadable paths,
observed directory changes, traversal ambiguity or exhausted budgets refuse the
entire clean. Any jce_project.json entry marks owned project source regardless
of contents or readability. Generated CMakeLists alone is not a project marker.
Never follow descendant symlinks: ordinary file symlinks and POSIX non-directory
entries (including FIFOs) remain cleanable, while Windows directory/unknown
reparse cases refuse. No source is moved, copied or automatically migrated.
This protects declared JCE projects, not arbitrary files without a manifest.
CacheCleanSourceProtectionTests uses disposable caches and real project/source
sentinels, a second-root refusal, generated-only deletion, preserved external
link targets, unavailable traversal/budget controls and an omitted-wire mutant.
Its Python command ports do not execute native compilers or clean real caches.

`sdk` and `package sdk` accept paired --build-dir / --install-dir plus
--embed-engine-shaders auto|on|off for native embedded SDK production. Custom
caches are strict children of build/, outside shared desktop/web/host/lock
ownership; custom prefixes are strict children of dist/sdk/, independent of
all default SDK prefixes. Both paths are required and --clean is refused for
custom outputs. OFF is an explicit missing-stock capability and requires a
custom pair; consumers must supply required shader assets. Native auto/on
explicitly writes JCE_EMBED_ENGINE_SHADERS=ON, restoring shipping policy after
a cached OFF. Authoring/Web/cross targets reject custom or non-auto selection.
Isolated SDK requests select the shared Conan generator path without admitting
unrelated default editor objects; only their own object cache enters admission.
sdk_outputs.py owns read-only path/request/cache admission, existing lease
resource selection and bounded atomic producer state. A matching request is
recorded inside ownership before configure, including ordinary failed states;
a failed first configure is retryable only with the same owner request and,
when present, a complete matching CMake cache. Custom installed SDK reuse and
post-install validation use sdk_target.py's compiler-generated metadata and
archive presence. sdk-build-options.json records output/shader request identity;
it neither extends SDK target schema nor certifies compiled ABI or rendering.
The producer still uses standalone.py's static Conan/CMake/install owner and
existing manifest pruning. No extra recursive-delete authority is introduced.
test_sdk_outputs.py uses actual dispatch with counted command ports and real
temporary metadata files, plus ordinary output/shader/preflight wire failures.
These controls never execute a compiler, native SDK, renderer or GPU.

SDK target selection follows the target toolchain: the native Windows STL
bridge is not requested by Linux, macOS or web builds. All platforms build the
shared fat library and install-owned scripting aggregate with the same job
budget; do not maintain separate hard-coded lists of scripting backends.

`test --filter REGEX --repeat-until-fail N` selects and repeats native checks
through CTest without a new build recipe. Empty selections are errors, never a
successful verification. Full-suite runs retain their independent acceptance.

Project acceptance consumes the shared periodic-perf parser. An unavailable or
ambiguous legacy-zero RSS cannot satisfy a memory ceiling: retain the timing
summary and fail that budget with an explicit unavailable reason. Modern
MiB-rounded known zero remains valid; this is not process-memory ownership proof.

Conan generation reuse requires a success fingerprint of the actual install
arguments, root recipe/lock, owned profiles (including shared base), local
recipes/hooks/policy and relevant toolchain environment. Missing or changed
inputs regenerate metadata once in the existing output directory. A failed
install removes only the first-party generation stamp; native build objects
and original third-party packages remain untouched. An existing toolchain
alone never proves generated dependency interfaces are current.
The fingerprint automatically reads the effective CONAN_HOME, including the
default home, through conan_config.configuration_digest. Only settings.yml,
settings_user.yml, global.conf and profiles enter this digest. Missing inputs
have deterministic null entries; unreadable, redirected, nonregular or oversized
inputs refuse reuse. Reads are capped at 1 MiB per file, profiles at 4 MiB and
4096 directory/file entries. Strict profile enumeration propagates access
failures and never scans packages, authentication or the shared database. Native
same-home configuration refreshes therefore invalidate metadata without a
caller-supplied digest; the existing Web identity environment remains retained.

JCE_BGFX_CPU_PROFILER is an explicit off|on build input, default off. The same
bgfx_graphics_conan_args owner passes its normalized package configuration for
native, Web and standalone graphs; those actual install arguments enter the
existing generation fingerprint. Switching off/on regenerates metadata while
preserving compiled objects and dependency originals. Windows clean MSVC
activation retains this input and the graphics tier; standalone synchronizes
hooks with the same explicit environment used for export/install. The bgfx
hook owns real compiler flags, package identity and compiled-selection metadata,
not callback inference or a second profiling framework. Existing hook and build
driver fixtures cover defaults, opt-in, conflicts, reuse and environment wiring;
counted ports do not prove an actual dependency compile or GPU execution.
The actual Apple reset control is a separate explicit-input class in
test_conan_configuration.py. JCE_TEST_BGFX_CMAKE_SOURCE selects the real upstream
CMakeLists; absent input reports SKIP. It renders installed Conan's actual flag
blocks, executes the complete Apple if-body with CMake LANGUAGES NONE, and checks
root/child directory definitions plus an ordinary omitted-directory-wire mutant.
The regular pure-Python controls still require the directory channel and on-only
configuration identity revision. No compiler, Apple SDK or GPU is exercised.

Native `ensure_host_tools` configures only its existing host asset-tool workspace:
explicit `JCE_BUILD_EDITOR=OFF` and `JCE_BUILD_TESTS=OFF` override stale cached ON
values on every refresh. Keep SDK install support, native host compiler environment,
shared job budgets and both host cache resources unchanged. A Web/cross caller's
target environment must not configure or build the native tools. Existing tool
files still enter Ninja's freshness check. `HostToolConfigurationControls` in
`tools/tests/test_build_driver.py` uses actual CLI/owner dispatch with counted
command/environment/cache-lock ports and real temporary markers; same-owner AST
omissions of either OFF flag must fail the unchanged ordinary admission assertion.
These are CPU command-plan controls, not compiler, native-tool or Web acceptance.

Build writers coordinate through `cache_lock.py` kernel advisory ownership.
Hold actual native trees, shared Conan generator output and SDK prefixes across
configure/build/install or CTest. Host-tool nested scopes must be declared in
outer ownership; nesting may reuse resources but cannot introduce new ones.
Persistent `build/.locks` files must never be unlinked as stale ownership;
kernel locks expire on process exit. Dry runs do not create ownership files.
Locks coordinate cooperating driver processes only; external direct Ninja and
processes started before this integration remain outside the protocol.
CPU tests: `tools/tests/test_cache_lock.py`, `test_build_cache_ownership.py` and
`test_build_driver.py`. Windows kernel tests do not certify POSIX execution.

Normal editor --artifact-dir relocates only its link output, retaining the native
build cache. Omitting it explicitly clears the CMake override. Existing target
file-directory dependency staging follows the output; no running editor is killed.

`test --target NAME` is repeatable and selects only explicit native build
targets; without an explicit target, `--suite all` builds `jce_tests`, while
engine/editor/consumer build `jce_tests_<domain>`. It requires --filter for CTest
selection when explicit targets are supplied. Never infer build targets from the
regex: filters only narrow CTest inside the selected domain. The metadata owner
routes native and script-provider inputs from the registering call site's domain,
including tools outside the test tree and providers shared by multiple domains.
Default all retains the complete configured registry; unavailable suites remain
explicit. Valid target names contain only standard identifier, dot, plus or
hyphen characters and cannot start with an option marker. Existing resource/cache
ownership and colour-space chain behavior remain unchanged.

`bundle` invokes the public-API host tool through the existing native cache, or
an explicitly selected installed SDK. Missing SDK tools fail without rebuilding
a substitute. This entry does not build an application or create a second packer.

`cook --rgba8` forwards the existing host-cooker uncompressed texture option. Compression stays platform-selected by default; source art fidelity projects explicitly opt in. This changes neither source asset bytes nor model import.

`cook --mipmaps` forwards complete mip-chain generation separately from `--rgba8`; both stay opt-in and use the host-cooker incremental salt to invalidate older outputs.

`delivery.py` consumes contracts/delivery-profile.json and the public bundle
content graph for selected startup-scene language admission. Never scan unused
authoring/templates as shipped scripts or equate a native app source with a
statically registered script module. Embedded editor/SDK configuration reuses
standalone.py's existing static dependency cache; authoring is explicit and
optional external VMs never enter default shipments. Actual SDK roster and
profile stamps must accompany installation. Non-Windows relocation/import
certification remains unavailable until native target evidence exists.


Project `bundles` inputs are forwarded by delivery.py to installed SDK helpers,
with ordered canonical paths, duplicate/missing-input rejection and explicit
clearing of stale bundle cache arguments. The app artifact resolver accepts the
stock template's extensionless executable name and adds a Windows suffix once.
Bundle files themselves remain single binary content archives, not SDK copies;
validate their editor reopen and embedded EXE-only relocation separately.

Cross SDK/editor builds refresh both native jce_pak and jce_bin2obj through one
host-tool resolver and forward their explicit paths to CMake. Target executables
cannot perform build-host embedding. Shader inspection uses the same execution-
probed shaderc resolver as SDK builds; mere file existence is insufficient.

Web dependency configuration records the actual Clang frontend and full emcc
ABI identity. An owned build-directory Conan configuration overlay extends the
settings schema while sharing the original package store/database; never edit
user settings, copy authentication or duplicate the dependency cache. Pass its
environment explicitly to hooks, recipe export and install. Derive EMSCRIPTEN
from the selected compiler wrapper for dependency recipes, not a machine path.

The same conan_config.py owner exposes prepare_configuration_home(base, overlay)
for an explicitly selected Native configuration home, including opt-in bgfx
CPU-profiler packages when an original installed hook must remain untouched.
Native preserves settings/settings_user bytes and reports clang_major=null;
the Web entry still adds and validates only the observed actual Clang major.
Callers pass the returned home explicitly to existing hook/export/install
owners. The original effective package store/database is shared, with no
package/source/auth copy and no change to the caller's original home. Existing
small-file/profile budgets, credential filtering, public remotes and overlap/
unowned-directory refusal remain common. test_conan_config.py exercises actual
temporary Native settings, store, original-hook immutability, the shared hook
synchronizer and an ordinary missing Native user-settings preservation wire.

conan_config.sync_hooks also owns default-home hook conflict admission. Native
commands without an explicit CONAN_HOME select a deterministic small owned
build/conan-config/native-<identity> overlay when an installed first-party hook
differs. Base-home identity and current hook/policy bytes partition that tuple;
unchanged inputs reuse it, including the original effective package store and
generation fingerprint. Explicit homes still refuse unknown installed bytes.
The original home/hooks/auth/database are never copied or repaired. Preflight
configuration, profile, directory and temporary-file redirection before writes;
host tools forward the same selected host_env through hooks/export/install.
The driver wrapper never mutates os.environ. Real temporary filesystem controls
and a missing environment-forwarder assertion cover selection and reuse; mocked
Conan commands do not certify package resolution, compilation or source purity.
`select_hook_home` owns the read-only selection policy used by both synchronization
and installed-hook lint. It never creates an overlay, changes the environment or
repairs installed bytes. Current owned inputs determine the single tuple; stale
selected overlay hooks remain failures, not another selection opportunity.
A missing planned home means not installed, never an executed-build certificate.

Embedded project admission passes the actual bundle platform and only a verified
SDK stock-shader directory. SDK stock files must match the current embedded PAK
BOM before installation, with an exact hash manifest. Project assets take priority;
unknown custom shaders still fail. Single EXEs do not depend on this SDK directory.

SDK consumers prefer execution-probed tools from the explicitly selected SDK.
Only a complete cook/pack/wrapper set avoids host-cache ownership; recheck after
acquiring SDK ownership and reject changed availability before any unowned fallback.

Selected native ScriptAssets require the SDK-generated NATIVE_FACTORIES registry.
Admission plans bind canonical assets to authored class references without losing
case. A plan permits compilation, never shipment: the relocated executable must
produce a nonce-bound receipt for its compiled closure through public registry
APIs. Web artifacts remain pending until an explicit actual target receipt matches
the recorded artifact and request. No manifest boolean substitutes for execution.

SDK-only and test configurations select JCE_BUILD_EDITOR=OFF; editor builds
explicitly restore ON when reusing an authoring cache. Embedded ScriptApi uses
the same C99 forwarders as a static archive; external-VM backends still require
a loadable transport. SDK installation prunes only stale files owned by its
install manifest, then rewrites profile stamps, preserving registered shader stock.

Web single-HTML application target policy is applied before every prebuilt
asset return, independent of release/dist. Archive MEMFS embedding and linked
prebuilt sources share that policy. Web authenticated PAKs use
JCE_PROJECT_PREBUILT_ASSETS_PAK and the same /game_assets.pak MEMFS
archive owner as raw builds; linking a binary blob alone is not Web boot.
Native prebuilt ASM enables its language before target_sources. Clear stale
PAK/OBJ/ASM/C/key selections on every application configure. Actual HTML admission must reject
external JS/wasm resources; a successful compiler exit is not delivery proof.

`app --registry-proof` audits the previously built embedded Web artifact,
without configure, repacking or relinking. DIST key shares use fresh masks, so
rebuilding would invalidate a real browser receipt. Preserve current SDK/profile
and closure admission plus nonce/request/artifact checks; missing products or
clean/configure-only/native conflicts are errors. This proves execution binding,
not visual/performance acceptance.

Standalone packaging resolves cached JCE_EDITOR_ARTIFACT_DIR from the selected
build tree for skip-build; missing configured artifacts never fall back to old
default outputs. Asset verification always reads the authoritative build tree,
independent of the executable output directory.

Only a complete `test --suite all` selection with no explicit target, filter or
rerun-failed executes the additional cooker/colour-chain acceptance. A repeated
complete selection retains it. Domain and subset commands end after their
selected CTest run; they must not build jce_cook or probe its extra dependencies.
`test_test_domain_registration.py` exercises actual parser/command dispatch,
including selected CTest failure, rather than matching implementation strings.

SDK target admission: sdk_target.py reads only the selected archive's
lib/<CONFIG>/jce-sdk-target.json, limited to 16 KiB, before app/build-project
cleaning or package-game cooking. All nine schema fields are mandatory and
unknown/duplicate keys and JSON escapes are rejected. The fixed ASCII scalar
contract applies only to generated SDK metadata. Compiler-derived system/arch/bits,
configuration/variant, delivery and resolved CRT must match; the fixed archive
must exist beside the metadata. Missing/old SDKs require refresh. VERSION.txt
and renamed SDK directory labels cannot override this contract. Production
metadata is owned by cmake/JCESDKTarget.cmake and installed by JCESDKInstall;
do not stamp a requested CLI architecture over a compiled archive.
CPU reader and compiler-free CMake/dispatch controls are test_sdk_target.py
and test_sdk_target_admission.py, consumed by the existing lint runner.
Compatibility admission is separate from real archive ABI, installed-SDK
smoke, GUI builds, final import/architecture audits and target relocation.

Formal installed-SDK consumer tests select an explicit prefix through
`test --configure --sdk DIR --sdk-variant release|dist
--sdk-delivery-profile authoring|embedded`. These inputs configure the existing
CTest consumers; their archive metadata and actual Noop execution remain the
admission authorities. SDK profile is independent of the native test variant.
Missing SDK selection is SKIP, and SDK flags without configure/selection fail.


`package game` is a compatibility entry to `build-project`, using the existing
native headless request and `jce_build_package_admit/publish_dist` owners. It has
no independent cooker, packer, copier or recursive output deletion. Both entry
points default to `<project>/dist/apps/<manifest-name-or-target>/<platform>-<arch>-<variant>`;
old `dist/games` artifacts are kept and never migrated automatically. CLI
preflight resolves paths before child dispatch, refuses root/project/source,
build and SDK overlap, and admits only an empty destination or the same regular
EXE for Dist replacement. Explicit external directories follow these same
checks; checkout outputs remain in dist. Native admission rechecks contents
before its audited publication, preserving old output on rename failure.
`--name`/`--version` are compatibility checks, not metadata overrides. Missing
headless ports for `--with-loose` or manifest `stage_loose` fail explicitly;
choose the editor's authoring workflow instead. A private result inside the
package is refused before stale-result cleanup; dry runs do not create result
parents or delete results. `test_build_driver.ProjectPackageSafetyControls`
uses real temporary files/SDK metadata and a counted headless child, including
an admission-wire removal negative control. It does not certify final native
publication, architecture/imports, relocation, or graphical execution.


`test --configure --ai-dispatch on|off` explicitly forwards
JCE_ENABLE_AI_DISPATCH=ON/OFF through the existing configure command. Without
this option no AI-dispatch cache value is overridden. An explicit selection
requires configure and is refused before cache ownership, dependency or output
mutation otherwise. SDK consumer selection may coexist in the same configure.
Public clones with missing optional sources retain the existing CMake
UNAVAILABLE registration; requesting ON is not proof of available targets or
executed controls. No private implementation or policy is copied into the
public CLI. Protocol controls live in the existing TestSuiteCLI gate; actual
native target discovery/execution and optional runtime availability remain
separate evidence.

Native `sdk --smoke` forwards the SDK's explicit variant/delivery profile and
requires actual execution for both C99 and scripting consumers. DIST consumers
share the existing authenticated content-graph/PAK/key-share builder through
`sdk_smoke_assets.py`; production prebuilt admission remains intact. Historical
raw-resource smoke without explicit variant/profile is compatibility evidence,
not authenticated DIST delivery proof. Cross/Web execution stays separate.

`tests/sdk_smoke/stage_dist_fixture.py` stages only declared regression inputs
inside fresh scratch projects and records the fixture kind in a bounded pointer.
C99 requires cooked `smoke.png` and `smoke.wav`; scripting preserves the exact
`scripts/smoke_py.py` bytes and public `script` / `python.source` representation.
Selected-SDK tool gaps, source/type/graph/BOM mismatches or changed proof after
linking prevent launch. Old scratch inputs/keys remain; switching build paths
clears cached prebuilt selections. These fixtures are not shipping closure claims.

`test_stage_dist_fixture.py` owns real-file staging, source identity, fixture
isolation and typed/proof protocol controls. `tools/tests/test_sdk_consumer_gates.py`
consumes the actual driver with counted compiler/proof/runtime ports to check
admission, build, verification and launch ordering and failure refusal. Those CPU
controls do not replace native decompression, declared-backend lifecycle markers
and exit success, or separate GUI, relocation and device presentation acceptance.

Declared Bundle DIST admission uses the same installed jce_pak public reader
and actual Archive verifier. Internal typed manifests select the explicit scene
or, when absent, the first declared Bundle scene in runtime order; all declared
assets participate in language/static-registry admission. A matching present
sidecar is audited, absence remains legal, and explicitly reader-compatible
legacy 1.0 sidecars are recorded as UNVERIFIED for final-byte hash binding.
Sidecar JSON never supplies the selected scene or typed truth.

DIST imports each validated archive through repeated --selected-archive into
one authenticated project PAK. Actual payload conflicts fail in the existing
packer; typed/provenance conflicts, changed inputs/inspection, incomplete deep
verification or unsupported scripts refuse acceptance. Runtime boot records the
real selected scene and the CMake invocation clears separate raw Bundle embedding.
Project/build/delivery-admission belongs to the existing app kernel lease before
inspection; it shares that lease with the target build tree and selected SDK.
Dry-run cannot certify uninspected Bundle closure. Older SDK inspectors lacking
typed evidence require refresh. tests/test_project_bundle_inputs.py and
test_build_cache_ownership.py exercise counted CLI/compiler/proof boundaries
including actual source wire removal; they do not prove native decoding,
authentication, linking, relocation or rendering.

Installed SDK file identity is owned by `sdk_product.py`. Native embedded and
standard/native-Web completion hooks publish `jce-sdk-product.json` only after
install, prune, required host tools and metadata complete, inside the existing
SDK lease. Bind generated UTF-8 version/API headers, the actual schema-1 target
and language profile, and canonical SDK-relative entries from the current
CMake install roster; missing/pruned roster entries require the existing prune
owner to synchronize the roster, not an identity bypass. Hash all observed
regular installed files with streaming SHA-256 and size limits; reject links,
escapes, duplicates, source changes and malformed identity. Verification is
read-only; publication atomically replaces one manifest and excludes itself.
The final directory snapshot guard still rejects any of its seven stat-field
changes. Its ProductError retains the literal prefix and reports only the first
changed SDK-relative directory (`.` for the root) plus changed before/after
fields as exact decimal strings, including integers beyond 2^53. Diagnostics
are failure-only and do not alter file identity, acceptance or retry policy.
Real temporary-tree controls in test_sdk_product.py mutate directory mtime
while preserving file inventory/bytes; controlled stat ports additionally verify
all seven field names and large-value precision. Earlier observation failures
need their own witness and are not explained by these fixture controls.
A retained manifest after failed installation must still verify current bytes.
The unsigned manifest describes installed files, not compiled ABI, execution
or a complete compiler/sysroot/CRT payload: all three capability flags remain
false, and its presence must not enable zero-environment GUI builds. Actual
CPU contracts live in `test_sdk_product.py` and the existing SDK output fixture;
counted compiler/tool ports do not certify linking or tool execution.


`stock_app.py` publishes an already-built Windows x64 embedded Lua/JS stock
runtime and seals one existing contract-1.1 Windows Bundle through `stock-app`.
It never configures/compiles, recooks assets, extracts an archive, or substitutes
a packer. Producer publication binds the explicit SDK product and verifies
ordinary/delayed PE imports plus actual embedded notices with the standalone
owner. A relocated runtime package carries only its bounded EXE, public PAK
inspector and exact notice/file identity manifest; sealing needs no installed
SDK, compiler or Conan. The canonical delivery reader owns fresh deep Bundle
inspection and typed dependency closure; stock admission additionally matches
the actual C reader's Lua/JS-only and typed-scene boundary.

The version-1 64-byte tail records little-endian exact sizes and FNV-1a64
corruption checks. It is not signing/authentication. Preserve exact host and
Bundle bytes, refuse existing outputs and retain separate inspection evidence.
A seal receipt says SEALED_NOT_EXECUTED with runtime/relocation verification
false; metadata cannot certify scripting, pixels, GPU completion, memory floors
or Editor GUI integration. Native production reader and source-body controls
belong to tools/runtime. `test_stock_app.py` exercises this actual Python owner,
public delivery admission and arbitrary-CWD real shared CLI failure/help paths;
its inspector/import/notice ports never stand in for a native stock build.

Stock seal version 2 wraps one exact admitted Bundle in a secure PAK using the
public packer, then requires fresh authenticated deep inspection. Its external
--pak-key-file must match the successful producer's owned path and key SHA;
publication distributes no raw key. The runtime manifest records schema 2 and
this protection identity. FNV remains only envelope corruption detection.
Version 1 and unprotected outer PAKs cannot qualify as Dist. The final directory
still contains only the EXE; tools, key input, work and receipts stay outside.
This restricted producer-cache key input does not deliver a zero-configuration
end-user builder; CPU fixture success also does not certify graphical execution.

The optional GPU annotation selector lives only in jce.py's shared bgfx Conan
arguments and clean-MSVC environment keep-list. Auto is the unchanged upstream
default; explicit on/off select the upstream CMake option, not Debug's effective
macro. Standalone and Web use the same helper and exact command fingerprint.
Do not duplicate its policy in SDK/editor/consumer builders or erase shared
cache entries. Dependency package identity and truthful compiled metadata are
owned by the existing configuration-only Conan hook.

## Explicit Windows build Job experiment

`windows_build_job.py` owns one opt-in Windows BUILD process tree. It is not
called by jce.py, the default command runner, benchmark sampler or runtime.
Trusted callers provide exact argv/cwd, a fresh output directory and optional
absolute helper-image paths/SHA256 values; project/environment configuration
never supplies helper policy. Default helper policy is empty and strict.

The owner creates an unnamed Job with KILL_ON_JOB_CLOSE and no breakaway flags
before CreateProcessW. The initial thread stays suspended until Job assignment
and held-handle PID/native integer creation-FILETIME/membership observation.
Only the three new inheritable NUL/stdout/stderr handles enter HANDLE_LIST;
attribute value storage lives until DeleteProcThreadAttributeList. Root launch
requests CREATE_NO_WINDOW; descendant window behavior is not certified.
Kernel Job containment is resource management, not a security sandbox or full
ETW ancestry observation. Short-lived metadata may be unavailable and is reported.

After command0, a bounded250ms natural drain and unchanged Job process-count
fence precede exact survivor classification. All surviving helpers must match
held process handles, membership/birth/path and separately held read-only image
path/hash pins before any planned termination. The cleanup deadline includes
the natural drain and150ms empty-Job quiescence check. New members after parent
return, unknown survivors, nonzero/timeout/cancellation, missing observations or
cleanup/close failures deny PASS and request whole-Job failure containment.
Receipts separate planned forced helper actions from emergency termination,
command return/reap, kernel empty verification, metadata gaps and each handle's
one close attempt. Atomic final receipt publication occurs after cleanup; a
missing receipt is not graceful success. Poll/time/output limits are cooperative
OS-call bounds; an output poll can overshoot its byte budget.

Caller-exclusive process-tree AND IPC-service ownership is a prerequisite.
Job membership and exact image identity do not prove exclusive broker clients.
Shared brokers are unsupported: this version rejects canonical MSPDBSRV.EXE
and VCTIP.EXE helper policies without any caller-boolean override. This deny
guard is not general broker detection. Real MSVC build integration requires a
separate supported instance-isolation/exclusive-lease proof; none is implemented
or tested here. Do not use global shutdown/kill or relabel prior strict failures.

`test_windows_build_job.py` uses normal-token hidden CPU fixtures, documented
Win32 APIs and failure ports. Explicit positive fixture policies may include
Python plus the exact SystemDirectory-derived console host; neither is silently
added to production policies. No compiler, GUI/GPU, SDK or real shared broker is
launched by these controls. Full source/architecture gates and real compiler
qualification remain separate Root-owned work.

## Build-only SDK staging

sdk_outputs.py also owns explicit --staging-dir for native embedded release/dist
SDKs. The prefix must be a strict ordinary-directory child of build/sdk. Reuse
the canonical compatible target cache, Conan graph and existing producer leases;
only the prior install prefix and SDK-enable bit may differ. This mode excludes
isolated output pairs, clean, cross/Web/authoring and shader OFF. Default dist
outputs are unchanged. No mutable request stamp belongs in the shared cache.
Reject aliases/reparse paths and incomplete or unowned nonempty prefixes. Reuse
requires actual target/archive admission and matching output-options metadata.
Partial installation remains unqualified and requires a fresh owned prefix;
never delete it or certify it from presence alone. Tests must let the real
verification owner publish options, rather than prepopulate its output in an
install port. Staging is not a bundled compiler, ABI or runtime certificate.

## Windows compatibility lanes

The XP lane uses verified LLVM-MinGW-XP with static C++/support libraries and
system MSVCRT; the modern Win7+ lane retains MSVC. Both use C99 engine/runtime,
C++20 Editor and native single-EXE delivery. OS/API, compiler/CRT, instruction
and graphics floors require independent qualification.

The explicit XP producer exposes --bgfx-bx-debug auto|off|on for supported
upstream BX diagnostics and assertions. Auto preserves historical commands;
explicit values reach graph and install through the same argument owner and
therefore invalidate the dependency-generation identity. Receipts record the
request, not proof of compiled diagnostics. Non-auto also asserts the matching
mandatory BX C++ consumer macro through a separate Conan selector; this is a
consumer ABI configuration, not observation of the provider. Actual provider
compile evidence remains required for a diagnostic acceptance claim. The Conan
hook owns option, compiled-expression metadata and package identity. Release
diagnostics retain optimization; upstream Debug enables BX checks even with
an off option. Explicit diagnostic audits retain the cache EXE and never
publish to the normal Editor release path. These controls do not certify
program linkage or rendered pixels.

`xp_editor.py` owns the explicit XP target graph, host-tool separation, isolated
ABI/cache admission and staged build/audit receipts. A complete target-guest
export inventory and stock payload/notices are required for qualified delivery;
prepared state or a PE header alone never publishes a qualified Editor.
Its immutable platform-header snapshots keep mimalloc, codec, stdio and BX query
cohorts separate; only named first-party headers enter them. Both consumer cohort
digests enter dependency arguments and built-EXE identity, so changing an adapter
cannot bless an earlier binary at audit. Keep the original mimalloc cohort stable
when an unrelated consumer changes. Known system DLL names such as MSWSOCK are
only candidates: exact captured exports and complete forwarders still govern
admission, never a filename or broad allowlist alone.
`conan_config.prepare_compiler_home` adds the actual observed Clang major in
small owned configuration. Its optional independent store selects fresh source
provenance without copying or repairing the original cache; omission preserves
the existing shared-store behavior.

`msvc_build_profile.py` selects a complete installed native MSVC toolset within
its plain base profile's compiler family and pins the corresponding Conan update
and VS installation. Its bounded critical-file hashes record selection provenance,
not executed frontend or complete package ABI verification. Exact two-file cohorts
are content addressed; changed, interrupted or foreign cohorts are refused without
repair. Audit mode cannot create a profile. The XP producer uses this owner for
its default native build profile and preserves explicit `--build-profile` inputs.

`xp_editor._default_conan_home` selects a content-addressed small configuration
home from bounded first-party hook names and bytes, then rechecks that selection
after hook synchronization. Hook revisions select new configuration homes while
retaining the same independently owned XP package store. Do not overwrite or
repair original installed hooks, settings, package or source bytes; an unknown
installed hook must retain its refusal rather than be relabelled or replaced.

Built-in stock compilation opts into `jce_compile_shaders(STOCK ...)` and reads
`engine/shaders/stock_profiles.json` through `tools/shader_stock_policy.cmake`.
The full profile is unrestricted; compatibility restricts GL/ESSL stock only.
Generic SDK shader calls keep their own names and existing commands. Root CMake
places compatibility assets beside, never under, the full root using the exact
catalogue digest. Cache metadata publishes policy name/digest and shader path;
XP configure/audit require that current identity and canonical output directory.
SDK installation carries the catalogue and helper with relocatable prefix paths.
The XP producer cooks GLSL, DX11 and SPIR-V, and audits the embedded ImGui
and PBR shader pairs for each profile. D3D11 and D3D12 share DX11 assets.
Optional runtime-loaded graphics libraries must not appear in its strict PE
import inventory. These controls do not establish
device admission or pixels.

The explicit XP producer requests the appended compatibility graphics tier;
ordinary builds still default to stable. Root Conan generation owns the forced
shader-policy cache value, and SDK configuration publishes the producer choice.
`tools/compile_shaders.cmake` adds the compatibility macro inside one quoted
semicolon-delimited shaderc define argument for GL/ESSL graphics only. Default,
other backend and compute command identities remain unchanged. The configure
controls in `tools/tests/test_shader_compatibility.py` execute no shader compiler
or GPU; a selected API floor does not certify GLES2 profiles or Editor behavior.

`windows_owned_launcher.py` builds the tooling-only C99 helper in `native/`
through the explicit XP toolchain and bounded build runner. It never runs the
helper or adds a runtime companion to the single-file Editor. `xp_vm_owned.py`
admits nonce-bound child PID/FILETIME, raw stream bytes and held-handle retirement
from the explicit `xp-vm --owned-launcher` transport. Child nonzero exit and timeout
remain failures even when the helper reports successfully. `--require-terminal-ack`
uses independently observed child identity with the unchanged canonical ACK reader.
Requested desktop and nullable token observations do not certify active-input
ownership, Explorer-token equivalence, process-tree cleanup or graphical usability.
Generic secret-bearing environment overrides remain refused; ACK nonce injection
is a typed private transport input. Pure command/parser controls are distinct from
native helper compilation, actual guest execution and Editor rendering acceptance.

`xp_vm_inputs.py` admits only explicit bounded flat input files through the same
byte-verified uploader and exact launch inventory. Inputs are caller data, never
additional executable launch targets. Default transport has no inputs. The
`--editor-window visible` opt-in keeps automation non-focusable while displaying
the Editor; default tests remain hidden. Input limits, filename collisions,
source mutation and launch-window policy have focused CPU controls in
`tools/tests/test_xp_vm_inputs.py`; none of these controls certify scene loading
or visible Editor pixels.

`xp_vm.py` uses explicit host-key pins, bounded SSH/SFTP and byte-verified Desktop
mapping/delivery. Credentials remain runtime inputs and never enter receipts.
Core probes and Editor smoke are separate; successful process exit does not
certify pixels, GUI usability, render capabilities or application lifecycle.

The explicit `native-probe` kind requires an exact marker and a successful
`--export-receipt` from the guest export collector. `xp_native_probe.py` owns
bounded receipt/inventory loading, adjacent `inventory.json` hash binding and
exact host/user/pin/architecture admission. It calls the existing complete
normal/delayed-name, ordinal and forwarder audit with explicit console support;
the Editor audit still requires GUI by default. Static admission and process
smoke leave Editor, runtime context, interactive and physical-driver qualification
false. Core probes retain their narrow DLL/CryptoAPI policy. No component or WGL
probe becomes an Editor proof, and no per-component import exception is needed.

`xp_os.py` compiles isolated C99 controls for the actual executable-path and
legacy display-observation owners. Its validation-omission control must fail at
the expected runtime assertion; a compile failure or unrelated crash is not a
passing negative control. `jce.py xp-os-probe` delegates to that one owner.

`xp_guest_exports.py` collects explicit allowed system DLLs and their forwarder
closure through the shared pinned, bounded, read-only SSH/SFTP primitives. It
checks actual guest/registry/PE identities, canonical mapping, held-file metadata
and two complete content hashes. These are stable observations, not an atomic
filesystem snapshot. Default inventory-only output avoids DLL copies; explicit
raw retention must preserve byte identity or refuse. Credentials never enter
source or evidence. `jce.py xp-guest-exports` forwards only the credential env
key. The inventory certifies the captured guest exports, not Editor execution.
See [Windows target policy](../../contracts/windows-targets.md).

`xp_flecs.py` owns the standalone C99 callback/atomic control producer, using an
explicit verified toolchain and unchanged Flecs header directory. It inventories
sources/headers, guards its own cache, and records command/static-PE evidence.
Explicit `--run-local` checks unchanged executable hashes and exact scenario
markers; `--negative-controls` additionally requires the exact ordinary assertion
exit and marker for each generated first-party mutation. Host execution remains
separate from matching XP guest execution. The 17 positive and 6 negative
controls exercise typed callback ownership and integer helpers, not a real
Flecs world, SDL thread, complete SDK or Editor.

## A1 XP compilation audit (opt-in)

`editor --xp` delegates to the existing XP Editor build phase before taking
modern desktop cache ownership. It uses the XP toolchain resolver shared with
the core probe and holds native host-tool ownership through the delegated
build. The XP producer still owns its target caches, tool probes and audit.
A successful shortcut builds the candidate only. Publication remains the
explicit XP audit with the matching guest export inventory.


`jce.py xp-probe` delegates exclusively to `xp_compat.py`, compiling the
standalone `tests/compatibility/xp_core` CMake project with the user-provided
LLVM-MinGW-XP MSVCRT distribution.  It compiles *real* jce_entropy.c but
intentionally does not select a regular SDK target, Conan package graph,
Editor or graphics pipeline.  Outputs and receipts are restricted to
`build/compat/windows-{arch}-xp-llvm-msvcrt`; the default Windows MSVC
route and its caches are untouched.  A static PE audit is not VM execution.
`tools/tests/test_xp_compat.py` owns synthetic positive/negative PE controls.

`xp-vm --editor-run-in-background` is an explicit Editor-only project-policy
request. It stages only {"player":{"run_in_background":true}} at the fresh
owned working directory's .jce/project-settings.json, using the existing bounded
byte-verified uploader. An existing directory, alias, nonregular file, changed
readback or unexpected subtree refuses launch. The exact prelaunch inventory
includes .jce and rechecks its sole regular file, canonical paths and bytes;
the existing eight-entry cap still permits four flat inputs plus target/helper,
fresh profile and this directory. Omission stages no policy or extra metadata.
Generic environment/credential filters and flat-input limits remain unchanged.
The public project-settings reader consumes this CWD location when projectless,
or when its selected project root is this same staged directory. An unrelated
open/restored project's own .jce settings take precedence; staging is not proof
of the effective setting or runtime stepping. Receipts retain this scope and
UNOBSERVED effective state. SFTP observations are bounded before/after checks,
not an atomic exclusion of concurrent path replacement. This does not alter the
Windows account, system settings or user profile. Focused fake-transport controls
are tools/tests/test_xp_vm_background.py; they execute no guest application.
