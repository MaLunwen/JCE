# Conan dependency configuration

Versions are pinned in conanfile.py; profiles own target/compiler configuration.
Original dependency sources and installed packages must never be modified by
JCE tooling. Hooks only set generated toolchain configuration or package metadata.
CMake 4 compatibility uses CMAKE_POLICY_VERSION_MINIMUM, not edited upstream
CMakeLists.txt. The bgfx tier and pristine policy enter its package identity.

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
