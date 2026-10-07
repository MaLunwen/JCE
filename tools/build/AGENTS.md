# tools/build — automated build implementation

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
CTest retains its separate explicit --jobs/CTEST_PARALLEL_LEVEL control.

jce.py owns platform selection, Conan/CMake ordering, SDK installation and
consumer builds. scripts/jce.py is a compatibility entry point. Shared command
helpers and non-desktop recipes are implemented here; they take explicit
consumer/toolchain input. Never supply a named game's icon, target or content
as a reusable builder's default. fetch_vendor_sources.py only creates new,
hash-verified cache trees; existing originals are verified and never changed.

Android host packaging is `android/`. Original SDL Java comes from the
verified external source cache; no Gradle wrapper or SDL source is tracked.
Use an installed Gradle 8.14.1 and supply SDK/NDK environment settings.

Native Windows single-EXE editor builds are owned by standalone.py and selected
with --standalone --variant dist. Use an isolated Conan static-CRT graph and
build directory; validate ordinary and delayed PE imports before delivery.
Validate embedded notice bytes against their source files before delivery.
SDKs and external managed runtimes are separate products. Do not replace native
single-file execution with a self-extracting wrapper or an embedded SDK bundle.
The explicit language set is Lua/C/C++/JavaScript; the full language build stays
available. Verify relocation with only the EXE and inspect actual captures.

SDK target selection follows the target toolchain: the native Windows STL
bridge is not requested by Linux, macOS or web builds. All platforms build the
shared fat library and install-owned scripting aggregate with the same job
budget; do not maintain separate hard-coded lists of scripting backends.

`test --filter REGEX --repeat-until-fail N` selects and repeats native checks
through CTest without a new build recipe. Empty selections are errors, never a
successful verification. Full-suite runs retain their independent acceptance.

Conan generation reuse requires a success fingerprint of the actual install
arguments, root recipe/lock, owned profiles (including shared base), local
recipes/hooks/policy and relevant toolchain environment. Missing or changed
inputs regenerate metadata once in the existing output directory. A failed
install removes only the first-party generation stamp; native build objects
and original third-party packages remain untouched. An existing toolchain
alone never proves generated dependency interfaces are current.

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
targets; default remains jce_tests. It requires --filter for CTest selection.
Never infer a build target from that regex: build selection and test selection
are independent. Valid target names contain only standard identifier, dot,
plus or hyphen characters and cannot start with an option marker. Existing
resource/cache ownership and colour-space chain behavior remain unchanged.

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
