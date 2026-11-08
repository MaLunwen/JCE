# cmake/ — shared producer and installed-SDK contracts

The root private CSM-PCF option remains default OFF. Its existing shared PAK
exclude argv adds four bounded owned diagnostic name tails only while OFF,
covering game/editor/embedded-engine PAK recipes. ON-to-OFF reconfiguration
changes these pack commands and repacks outputs while retaining cached bins.
Native host-tool controls execute that actual CMake policy and existing packer
against one reused output/archive, inspect verified BOM contents, and require
the omitted-policy control to fail ordinary archive assertions. No new packer,
cache deletion or suffix-limit extension is involved.

JCESDKTarget.cmake owns compiler-target identity and the schema-1 archive
contract. Its existing MSVC COFF resolver is also consumed by JCESDKHelpers;
keep one mapping. Other targets use compiler macro probes with STATIC_LIBRARY,
never try_run. Cache probes by compiler/target/flag identity and reject C/C++
architecture disagreement. Directory labels and CMAKE_SYSTEM_PROCESSOR are
not evidence of the compiled architecture.

engine/cmake/JCESDKInstall.cmake installs jce-sdk-target.json beside each
lib/<CONFIG>/jce_engine_core archive. The generated file describes that archive
owner's system, architecture, pointer width, configuration, variant, delivery,
resolved CRT and fixed archive name. Check mixed owned CRTs before installation.
A Release file cannot admit a missing or stale Debug configuration.

JCEConfig reads the same file before creating imports or enabling asset/ASM
helpers. Explicit consumer variant, delivery and CRT must match; missing,
malformed or incompatible metadata requires SDK refresh. Reads are bounded to
16 KiB. Schema 1 is generator-owned: all keys and permitted strings are fixed
ASCII scalars and JSON escapes are forbidden by every reader. This closes
decoded duplicate-key aliases and embedded NUL; it is not a restriction on
ordinary project JSON. Relocated or renamed SDK prefixes retain their identity.

Schema/dispatch fixtures in tools/tests/test_sdk_target_admission.py model
CMake identity with project(NONE) and script mode; they invoke no compiler.
These tests do not certify archive ABI, linked products or target execution.

Windows schema 1 describes an MSVC-ABI archive and CRT. The shared private
compiler-ABI guard is consumed before target probing by both metadata production
and consumer admission. CMake MSVC-compatible frontend mode permits cl/clang-cl;
GNU/LLVM-MinGW mode fails with SDK_ABI_MISMATCH even at matching architecture.
Compiler brand is not an ABI contract. Non-Windows policy is unchanged. A future
GNU Windows SDK requires a separately produced compatible archive/runtime policy;
this guard does not claim universal clang ABI or bundled compiler availability.
Frontend controls model CMake identity in script mode and project(NONE), invoke
the actual producer/consumer, and require ordinary configure failure for rejected
modes. They do not prove that either compiler links or runs an application.

The SDK's deferred required-language hook reuses successfully validated
target-owned scripting declarations, including native factory symbols. Required
manifest metadata checks those symbols; it cannot invent a missing declaration.
The hook merges required languages with the target's enabled set and delegates
again to JCEScriptEnable, preserving factory/path/closure mismatch failures.
tools/tests/test_static_native_registry.py consumes the actual SDK hook and
scripting owner, including an ordinary configure failure for the old wire.
These controls do not certify an installed SDK shipment or project gameplay.

The root Engine stock PAK recipe uses the existing --strip-debug-paths option
in DIST only. Public stock payloads remain distinct from the authenticated,
encrypted project archive; removing the debug name table does not encrypt them.
Keep hashed runtime lookup and the separate producer BOM inventory intact.
Verify the actual built archive and a graphical SDK consumer after changes.

Web application configuration owns the private Emscripten internalformat wrap
and JS-library linkage in the existing idempotent _jce_configure_web_target.
Only EMSCRIPTEN executable targets consume it. Repo builds resolve the same
platform JS owner that SDK installation ships beside the helper; missing bridge
content fails clearly. Keep shell quoting valid for relocated paths with spaces.
Actual resolver positive/omitted-wrap controls prove pointer interception; CPU
query and JS owner controls remain separate from SDK/browser execution. Do not
change upstream packages or globally intercept browser prototypes to fix queries.

The installed JCESDKHelpers owns a success-only IPO probe cache. The existing
CheckIPOSupported invocation and application properties/warnings remain the
native authority. A cache identity binds enabled languages, compiler/tool file
content, effective flags/configurations/CRT/architecture, generator/policies,
critical environment (including MSVC LINK/_LINK_) and CMake/owner probe
sources. CMP0137 platform propagation and CMP0138 flag propagation both enter
the policy identity. Each configure content-hashes
the real files; matching requests within that configure memoize the digest only
while tools/modules remain stable. The file-hash count is logged separately
from probe reuse. This does not certify target-specific flags or runtime speed.

Unknown compiler/language identity, custom toolchains/rules/module paths and
launchers retain real probing rather than accepting presence-only stamps.
Failures and diagnostics are returned unchanged and are not persistently cached.
Do not delete the upstream probe tree or exclude its products from no-op scope.
tools/tests/test_ipo_probe_cache.py runs tiny compiler-free CMake projects with
counted language/probe ports, changed-input controls and an old-repeat ordinary
failure. Actual native warm verification remains a separate consumer witness.

The private IPO fingerprint has an optional reason output. Unknown identity keeps
all original probe behavior; the wrapper reports a stable reason/key once per
configure for each fallback. It prints no environment contents and does not
silently admit relative tools, missing files, extra languages or opaque inputs.
The NONE controls cover all five fallback classes and preserve one-output calls.
An isolated metadata replay is diagnostic, not proof of the real caller's
variable state; use the supported consumer configure log to identify that state.

IPO identity retains the complete enabled-language set. Only C/CXX are qualified
native IPO probe languages. RC and ordinary ASM are auxiliary languages ignored
by CheckIPOSupported's default selection; their loaded/compiler identity, flags,
rule inputs and real tool content still enter the fingerprint. MSVC enables RC
inside its platform initialization, before an application explicitly requests it,
and RC participates in executable manifest linkage. IPO templates are checked
only for the actual C/CXX probe set. CUDA, Fortran, other languages, incomplete
auxiliary tools or an auxiliary-only set continue through the original default
probe. Never add a LANGUAGES argument that narrows its native scope.

## Explicit Windows XP target lane

`cmake/toolchains/windows-{x64,x86}-xp-llvm.cmake` and the shared owner
`windows-xp-llvm-common.cmake` select the GNU/MSVCRT ABI with exact NT 5.2
(x64) or NT 5.1 (x86) floors. They take `JCE_XP_TOOLCHAIN_ROOT` from the caller
or its explicitly isolated build environment. Target libraries/includes stay
rooted; Conan configuration packages are admitted separately by the producer.
Never substitute them for a modern MSVC SDK archive. `JCEWindowsTarget.cmake`
owns the explicit legacy root build opt-in and preserves the default Win7 lane.
`xp_core` proves a limited OS/C++ runtime subset; it never certifies the Editor.
GNU Windows Editor assets reuse `_jce_target_embed_binary`, whose generated
assembly owns COFF symbol decoration and pointer width. Keep the MSVC and
Apple/ELF paths intact; the old ELF-only template is not a MinGW wrapper.

The root no-cook Editor resource branch stages root LICENSE and
THIRD_PARTY_LICENSES.md once with configure_file(COPYONLY) in its build-local
_raw_editor_notices owner, includes that directory in the Editor PAK inputs and
keeps both source files in the actual pack command dependencies. Notice edits
must invalidate packing; missing inputs fail configuration before packing. About
reads the PAK address, so the separate Windows RCDATA check cannot substitute
for this resource input. The cooking branch retains its existing notice copy.
The existing test_standalone.py controls execute the actual no-cook CMake body
with real temporary notice bytes and a counted pack-command destination. An
omitted resource-root wire fails ordinary assertions; these compiler-free
controls do not certify a produced PAK, native resource linker or About GUI.
