/* jce_engine_shaders_pak_stub.c
 *
 * Fallback weak/stub definition of the embedded engine-shader PAK.
 *
 * When the build is configured WITHOUT `JCE_EMBED_ENGINE_SHADERS=ON`
 * (or before the generated `jce_engine_shaders_pak.c` is available)
 * this 1-byte placeholder satisfies the externs declared in
 * `jce_shaders.c`, and `embedded_pak()` becomes a no-op
 * (size <= 1 → never tries to open).
 *
 * When the option is ON, the generated `jce_engine_shaders_pak.c`
 * is added to the same target *before* this file in the source list,
 * so the linker picks the real symbols and ignores this TU's
 * (weakly-bound on ELF / Mach-O; on COFF the stub TU is simply not
 * pulled in because the strong definition arrived first inside the
 * same static archive).
 *
 * The CMake side guards this by only including the stub when the
 * embed option is OFF — see engine/CMakeLists.txt.
 */

#include <stddef.h>

#ifndef JCE_ENGINE_SHADERS_EMBED
#  define JCE_ENGINE_SHADERS_EMBED 0
#endif

#if !JCE_ENGINE_SHADERS_EMBED
const unsigned char jce_engine_shaders_pak[1]      = { 0 };
const size_t        jce_engine_shaders_pak_size    = 0;
#endif
