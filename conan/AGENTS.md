# Conan dependency configuration

Versions are pinned in conanfile.py; profiles own target/compiler configuration.
Original dependency sources and installed packages must never be modified by
JCE tooling. Hooks only set generated toolchain configuration or package metadata.
CMake 4 compatibility uses CMAKE_POLICY_VERSION_MINIMUM, not edited upstream
CMakeLists.txt. The bgfx tier and pristine policy enter its package identity.

The appended graphics tier `compatibility` has value 3 and GL21/GLES20 floors;
stable/modern/current rows and ordinary stable default keep their identities.
The XP producer selects compatibility explicitly. Root recipe generation forces
the producer shader-policy BOOL in both directions using the existing owned
extra-variable channel; ordinary CACHE defaults cannot reset a reused tree.
The installed SDK carries that same selection. API floors and static package
identity never substitute for target shader/runtime/GUI qualification.

Compatibility bgfx packages additionally enable the pinned upstream's
BGFX_GL_CONFIG_BLIT_EMULATION=1 through compiler flags and directory defines.
The upstream native copy path still wins when available; its fallback uses
2D color FBO attachment and glCopyTexSubImage2D. This is not a cube, array,
3D, depth or multisample copy guarantee. The upstream-2d-fbo-v1 policy enters
the compatibility package identity, so old packages without emulation cannot
be reused. Stable/modern/current defaults and identities remain unchanged.
Conflicting exact reserved define/undef/CMake channels refuse before binary
reuse. Sources and existing installed packages remain immutable. Capability
admission and actual rendered/read-back pixels remain separate acceptance.

The opt-in `user.jce:bgfx_bx_debug=auto|off|on` selector controls the pinned
upstream BX_CONFIG_DEBUG CMake option independently of CPU profiling and debug
annotations. Auto preserves caller options and adds no override. Explicit on/off
adds a forced BOOL cache option only when no caller option exists; matching caller
booleans and typed records are preserved. The upstream bx/bgfx target expressions
compile BX debug on for Debug even when the option/request is off. This also
turns on upstream assertions, not only debug traces; Release on is a diagnostic
cohort, not ordinary shipping policy.

JCE_BGFX_BX_DEBUG_REQUEST reports -1/0/1 for auto/off/on; OPTION and COMPILED use
-1 for unknown and 0/1 for known values. Custom toolchains, unresolved values in
tools.build:cflags/cxxflags/defines or active CMake compiler flags, unknown
configuration/system inputs and multiconfig overrides
retain unknown compiled state. Their bounded canonical request fingerprint is
computed without reading a toolchain or package. Nondefault/unknown binaries carry
a separate upstream-cmake-v1 BX-debug package revision and option/compiled input
identity; effective upstream defaults retain historical identities. Exact reserved
compiler define/undef operands, including -Wp forwarding and C/C++ CMake flag
channels, must agree with the actual compiled state. CMake option ON/OFF are
interpreted as booleans rather than compared with literal compiler operands.
The original bx component consumer macro is normalized only from its canonical
recipe baseline to a known compiled value; defaults and order are preserved,
and duplicate/noncanonical/contradictory exports refuse. Unknown auto preserves
the historical consumer baseline while reporting provider COMPILED=-1. Unknown
explicit on/off requires `user.jce:bgfx_bx_debug_consumer=off|on` during
package_info; auto refuses early instead of omitting bx's mandatory consumer macro.
This separate selector asserts the consumer ABI macro only and never changes the
provider binary identity, upstream build option or COMPILED=-1. It emits the exact
BX_CONFIG_DEBUG=0/1 bx component definition plus distinct
JCE_BGFX_BX_DEBUG_CONSUMER_ASSERTED=1 and CONSUMER_VALUE=0/1 metadata. Auto emits
neither new consumer marker and preserves historical default metadata. Assertions
must match a known provider state or the deterministic requested upstream Debug
OR option expression when available; off under Debug therefore requires consumer
on. An unknown expression remains an assertion, not a provider observation.
Verify actual native provider operands and artifacts before compiling its consumer;
no custom toolchain is implicitly trusted. Unknown compiled state cannot be
certified by an asserted consumer or a manual provider macro definition. Pure
controls live in tools/tests/test_bgfx_bx_debug.py; actual native flags, callbacks,
assertions and target behavior remain separate evidence. Original sources and
installed packages are never modified.
The optional bgfx CPU profiler is off by default. The shared build driver maps
JCE_BGFX_CPU_PROFILER=off|on to user.jce:bgfx_cpu_profiler, which the configuration
hook adds to bgfx's package identity. The hook merges the real
tools.build:cxxflags BGFX_CONFIG_PROFILER=0|1 definition; a CMake cache variable
alone does not enable upstream scopes. Invalid selections, conflicting reserved
defines/undefines and conflicting CMake variables refuse before package use.
The same definition also enters tools.build:defines, which Conan emits as
directory add_compile_definitions and which survives upstream Apple/Ninja's
CMAKE_CXX_FLAGS reset. Enabled packages add the directory-defines-v1 config
revision to their identity; disabled packages add no CPU-specific revision.
The annotation policy below independently partitions new package identities.
Generated aggregate/component metadata exports JCE_BGFX_CPU_PROFILER_COMPILED
as 0 or 1 from that package selection. A missing macro is unknown; missing
runtime callbacks never prove a compiled-off package. Existing dependency
sources/packages remain untouched, and actual isolated compilation plus callback
observations are separate acceptance from Python configuration controls.

Local recipes are first-party packaging for upstreams absent from Conan Center.
They fetch verified source archives and build unchanged upstream source. Any
required upstream code fix must be addressed by a verified upstream release or
an owned adapter outside the upstream tree. Do not revive retired source patches.

CONAN_HOME selects the cache; a fresh cache is the way to obtain pristine sources.
Never repair an existing source checkout or replace its bytes. Existing cached
packages may predate this policy; a successful cached build is not source-purity
verification. Cross-platform recipes that needed retired patches require fresh
profile validation before claiming support. See contracts/conan-source-policy.json.

CMakeDeps names the bgfx whole-package aggregate bgfx::all. The upstream
explicit bgfx::bgfx component remains the renderer dependency; its closure
must not inherit texturec or bimg_decode through a name collision. Texture
encoding consumers still link bimg_encode, bimg and bx explicitly. Adjust
first-party generated metadata, never remove upstream symbols or libraries
to hide a duplicate-provider conflict.

The harfbuzz package aggregate is harfbuzz::all; core shaping consumers keep
the explicit harfbuzz::harfbuzz component. Runtime schema messages link
protobuf::libprotobuf; protoc remains the matched build-context executable,
not a runtime aggregate that also imports alternate UTF-8 providers. This is
generated metadata/consumer closure configuration, never upstream archive edits.

Web dependency recipes that reject Conan's emcc compiler model use the actual
underlying Clang major, never a fabricated supported version. The driver must
fail honestly if its Conan settings schema cannot represent the installed LLVM;
it must not rewrite user/global settings or claim Clang 19 for a Clang 23 SDK.
`tools/build/web_toolchain.py` verifies actual emcc/Clang banners and version-file
agreement, retaining the full Emscripten version (including patches), selected
public ABI headers and CMake toolchain hash in an explicit identity digest.
The wasm profile includes `user.jce:web_toolchain_abi` through the documented
`tools.info.package_id:confs` mechanism; ordinary compiled host dependencies,
not only JCE/bgfx, receive this binary identity. Header-only recipe identity
clearing remains intentional. The digest must also enter generation reuse so
an in-place SDK/header upgrade cannot reuse an old generator stamp. This is
selected ABI identity, not an all-sysroot purity certificate. Original SDK and
package/source bytes remain untouched. Conan's official emcc/full-version model
was preferred, but actual libvpx recipe rejection requires this verified Clang
fallback while preserving the Emscripten ABI partition in package identities.

When a Conan settings schema lacks the real LLVM major, `conan_config.py` prepares
an isolated configuration home under build output, using settings_user's normal
list merge. It shares the original effective core.cache:storage_path (including
its database), so packages are not copied. Only bounded small settings/config/
profile inputs are read; private authentication databases are not copied and
public Conan Center is the overlay's default remote. Credential-bearing profiles
are rejected, sensitive global configuration values are not replicated, and
original settings/home/source bytes remain unchanged. The driver supplies this
CONAN_HOME only to the relevant Web Conan calls; ordinary host configuration
must remain independent. Merely preparing the overlay is not a successful Web
package graph or SDK build.

Native first-party hook selection uses the same conan_config.py bounded
configuration owner through prepare_configuration_home. It copies the actual
settings/settings_user bytes without adding any compiler/version, shares the
original effective package store, and retains the existing safe-profile,
credential-filter, public-remote and owned-directory checks. Only the explicit
returned CONAN_HOME receives new first-party hooks. Original installed hooks,
authentication, packages, sources and cache database are not copied or edited.
The Web prepare_toolchain_home entry retains its actual-Clang extension and
validation. CPU directory controls use real Conan settings/conf readers and
the actual hook synchronizer; they are not a dependency install or compile.
The same configuration owner supplies the build driver's automatic selected-
home digest. Only named settings/conf and bounded profiles are read; missing
values are explicit, access errors propagate and directory redirects refuse.
Packages, authentication and database files never enter generation reuse input
scanning. The existing Native fixture checks same-home changes and ordinary
omitted-digest failure through actual command dispatch with counted install ports.

GPU annotations use the shared JCE_BGFX_DEBUG_ANNOTATION=auto|on|off selector.
Auto preserves the upstream option/caller value; explicit on/off refuse option
conflicts and never add a second BGFX_CONFIG_DEBUG_ANNOTATION compiler define.
The upstream actual expression is !WindowsStore && (Debug || option), so off is
not a forced-disable promise in Debug. Normalized option, changed/unknown
compiled state and upstream-cmake-v1 revision partition changed bgfx binaries;
the request remains consumer metadata and generation input.
Historical package IDs remain exact when effective compilation is unchanged:
auto/off, Debug's forced ON and WindowsStore's exclusion do not add annotation
identity keys or rebuild bgfx just to publish new consumer metadata. Only a
changed compiled annotation bit or unresolved inputs add annotation identity
and revision. Equivalent enabled auto/caller and explicit on share one ID.
Aggregate/component metadata exports JCE_BGFX_DEBUG_ANNOTATION_REQUEST/OPTION/
COMPILED, with -1 unknown. Custom toolchains, unresolved values and mismatched
configuration overrides cannot become a compiled-off claim. Preserve unrelated
caller flags and typed CMake values, reject exact reserved define/undef conflicts,
and keep every source/package byte immutable. Actual compiled definitions and
native markers still require isolated target validation.

Unknown annotation configurations additionally hash only already-consumed
configuration requests: relevant CMake variables, selected system/build type,
caller flags/defines and user_toolchain request paths. Canonical JSON uses
sorted keys, ASCII escaping, no NaN and compact separators; validation caps
256 nodes, depth 8, 8192 text characters and 16384 serialized characters before
SHA256. Different unknown requests cannot share the former generic unknown ID;
same requests and dictionary reordering remain stable. No external toolchain
contents are read or authenticated: in-place file changes remain unverified.
The source guard allows only direct canonical json.dumps and pure sha256 calls,
retaining filesystem/loader/process/reflection prohibitions.

Desktop Windows PhysFS packages use the shared CMake configuration hook to set
_WIN32_WINNT and WINVER to 0x0601 through C/C++ flags and directory definitions.
The original PhysFS source then selects its existing CreateFileW path instead
of a hard CreateFile2 import. Conflicting reserved definitions or undefines
refuse generation and package resolution; unrelated inputs remain unchanged.
The PhysFS-only Windows floor and directory-flags-v1 revision enter its package
identity before binary reuse. Other packages, non-Windows and WindowsStore keep
their existing identity and compile flags. This is generated configuration,
not a patch or relabeling of old dependency source, packages or installed hooks.
Actual compiler inputs, linked PE imports and Win7 execution require separate
validation; passing Python hook controls does not certify those observations.

## Explicit XP target profiles

`profiles/windows-{x64,x86}-xp-llvm` are target profile bases. The XP producer
injects verified LLVM-MinGW-XP compiler/runtime inputs, libc++, system MSVCRT,
exact NT floor and an ABI content digest into isolated package identities.
Keep build-host MSVC tools separate from GNU target archives. A successful
resolve is not proof of target execution. The optional private AI dispatch
graph stays disabled on XP; its unused Vista-floor CURL dependency is omitted.
PhysFS keeps its upstream XP branches through generated floor configuration;
C-only recipes may remove C++ settings before package-ID hooks. BGFX receives
a complete explicit OpenGL, D3D11, D3D12 and Vulkan renderer set under a distinct
XP identity. The compatibility tier retains GL 2.1. The desktop Direct3D and
Vulkan loaders are resolved at runtime. The application verifies the newer DXGI
swap-chain interface before entering Direct3D. The first-party Vulkan probe
checks Windows surface support, required device extensions and a hardware
device with a presentation-capable graphics and compute queue before calling
the provider. It creates no window or device and releases its temporary instance
and loader. Its source is part of the verified BGFX cohort. The provider's
hardware video option stays off in this package. JCE media decoding remains
independent of that option.
Default modern flags and package identities remain unchanged. No original
source or cached package is patched. Suspect historical sources require a
separate verified source scope; a package-ID label does not prove pristine bytes.

The explicit XP graph selects quickjs-ng/0.4.1 from its immutable official
commit archive; modern targets retain quickjs-ng/0.16.2 as the first/default
recipe source. The legacy provider requires boolean user.jce:windows_xp=True
and a Windows GNU Clang/libc++ x86 or x86_64 profile without MSVC runtime
settings. It exports JCE_QUICKJS_LEGACY_EXCEPTION_API=1 for the older exception
API. This library-only XP configuration has no JavaScript Atomics or Worker
facility. Its sole added C warning adjustment keeps Clang's unterminated-string
warning without promoting it to an error; no upstream bytes, Windows version
macros or threading macros are changed. Modern generation and metadata remain
unchanged. Injected recipe controls verify packaging policy, not a real runtime
or complete Editor qualification.

`hook_windows_xp_compat.py` configures only explicit XP mimalloc/3.3.2 builds.
The producer verifies an immutable two-header cohort and supplies its consumer
path and combined SHA256; the hook reads no files. Both private XP opt-in guards
reach C/C++ flags and directory definitions; the quoted forced include reaches
the C/C++ compiler flags, including caller base-flag presets.
The official MI_EXTRA_CPPDEFS selects MI_USE_RTLGENRANDOM=1, while conflicting
FLS/custom initialization/direct-TLS selections refuse. The default CRT/TLS
configuration remains selected. Policy, effective configuration and header
content partition XP package IDs; only the verified include operand is replaced
by its content identity so relocating an identical cohort preserves reuse.
Modern packages keep their existing flags and identities. These configuration
controls do not qualify a complete allocator build, final PE imports or Editor.

`hook_windows_xp_consumers.py` configures only exact reviewed XP codec and
standard-stdio consumers. CMake uses escaped forced-include operands; Meson uses
separate compiler argv elements. The reviewed x86 dav1d consumer additionally
binds Meson's WINDRES to the same toolchain's existing target-specific rc
request. Conan's MesonToolchain reads package buildenv rather than the CMake
rc compiler key. Explicit conflicting environment operations refuse; only
pre_generate changes this package's environment. The selection enters this
consumer's identity without changing the complete profile or global ABI.
The producer declares the verified singleton CMake user-toolchain path; other
replacements and conflicting reserved flags
refuse. The real libc++ selection is retained, not classified as opaque input.
Canonical configuration, policy and cohort content participate in package IDs;
only the verified header operand is normalized for relocation reuse.
BGFX C translation units additionally consume the codec cohort for bundled
dav1d. Its C++ channel uses documented MINIZ_NO_STDIO plus a separate BX optional
large-page-query header, without codec aliases. Both distinct content digests
participate in BGFX identity. Directory guards alone do not include a header.
This retains memory compression while removing unused miniz file helpers.
External and bundled dav1d providers must not be assumed interchangeable or
safe to co-link.
Both reviewed XP architectures compile BX, BGFX and bimg for SSE2. The verified
BGFX cohort includes a project-specific CMake callback and a C++ selector. The
callback removes the pinned BX SSE4.2 option from its target and public usage
requirements, preserving all other options. The selector uses the upstream BX
reference SIMD branch and leaves its debug include order intact. ASTC retains
its separate SSE2 implementation through its existing build definition.
The policy and cohort digest partition XP BGFX package IDs. Modern builds and
independently runtime-dispatched codecs retain their existing configuration.
Disassembly and execution on a CPU without SSE4 are separate acceptance checks.
Only the reviewed XP x86 GNU Clang 22 BGFX target selects upstream
BX_AMALGAMATED=OFF. A typed BOOL cache variable with force=True overrides the
recipe's ON request during toolchain loading and enters the canonical package
identity. Conflicting caller selections refuse. This keeps O3, target ISA,
features and original sources while separating the Settings translation unit
from FilePath/string definitions; standalone compilation avoids the observed
Clang 22 i686 frame-index overflow. Link/provider and guest checks remain
separate from that compiler observation. Other target identities stay intact.
The codec consumer admits true XP WINVER plus either its true declaration floor
or the reviewed upstream 0601 selection; it restores that selection after
parsing shared helpers. Final PE import, actual provider and guest checks remain
mandatory. No callback reads or edits upstream source/package files.
