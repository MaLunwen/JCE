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
