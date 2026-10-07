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
