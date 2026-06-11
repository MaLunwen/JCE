/*
 * jce_entropy.c  Host OS CSPRNG wrapper.
 *
 * Platform shim (engine/src/os/platform/): native #includes are sanctioned
 * here — the entire purpose of this directory is to absorb host-OS quirks
 * behind one uniform API.
 */

#include <jce/os/platform/jce_entropy.h>

#include <stdint.h>
#include <time.h>

#if JCE_PLATFORM_WINDOWS
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <bcrypt.h>
#  if defined(_MSC_VER)
#    pragma comment(lib, "bcrypt.lib")
#  endif
#else
#  include <stdio.h>
#endif

bool jce_host_random_bytes(void *out, size_t n)
{
    uint8_t *dst = (uint8_t *)out;
    if (!dst || n == 0) return false;

#if JCE_PLATFORM_WINDOWS
    if (BCRYPT_SUCCESS(BCryptGenRandom(NULL, dst, (ULONG)n,
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return true;
#else
    {
        /* Platform shim: raw stdio on the urandom device is the sanctioned
         * exception here (no SDL dependency in this layer's contract). */
        FILE *f = fopen("/dev/urandom", "rb");
        if (f) {
            size_t got = fread(dst, 1, n, f);
            fclose(f);
            if (got == n) return true;
        }
    }
#endif

    /* DEV-ONLY fallback: time + ASLR'd pointer mix through xorshift64*.
     * NOT cryptographically secure — callers must warn and never ship
     * keys produced this way. */
    {
        uint64_t s = (uint64_t)time(NULL);
        s ^= (uint64_t)(uintptr_t)&s * 0x9E3779B97F4A7C15ull;
        s ^= (uint64_t)(uintptr_t)dst << 17;
        if (s == 0) s = 0xDEADBEEFCAFEF00Dull;
        for (size_t i = 0; i < n; ++i) {
            s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
            dst[i] = (uint8_t)((s * 0x2545F4914F6CDD1Dull) >> 56);
        }
    }
    return false;
}
