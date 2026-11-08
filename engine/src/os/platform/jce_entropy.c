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
#  if defined(JCE_WINXP_LEGACY) && JCE_WINXP_LEGACY
#    include <wincrypt.h>
#    if defined(_MSC_VER)
#      error "The XP legacy profile requires the verified LLVM-MinGW-XP toolchain"
#    endif
#  else
#    include <bcrypt.h>
#    if defined(_MSC_VER)
#      pragma comment(lib, "bcrypt.lib")
#    endif
#  endif
#else
#  include <stdio.h>
#endif

bool jce_host_random_bytes(void *out, size_t n)
{
    uint8_t *dst = (uint8_t *)out;
    if (!dst || n == 0) return false;

#if JCE_PLATFORM_WINDOWS
#  if defined(JCE_WINXP_LEGACY) && JCE_WINXP_LEGACY
    {
        /* XP x86/x64 has CryptoAPI, not Bcrypt.dll.  This branch is compiled
         * only for the independent LLVM-MinGW-XP legacy target.  It has no
         * static import of Vista's BCryptGenRandom.  CryptGenRandom accepts
         * DWORD lengths, so split larger buffers without narrowing size_t. */
        HCRYPTPROV provider = 0;
        if (CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_FULL,
                                 CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
            size_t offset = 0;
            bool ok = true;
            while (offset < n) {
                size_t remaining = n - offset;
                DWORD chunk = (DWORD)(remaining > 1048576u ? 1048576u : remaining);
                if (!CryptGenRandom(provider, chunk, dst + offset)) {
                    ok = false;
                    break;
                }
                offset += chunk;
            }
            CryptReleaseContext(provider, 0);
            if (ok) return true;
        }
    }
#  else
    /* Do not truncate size_t to BCrypt's ULONG length. */
    if (n <= 0xFFFFFFFFu &&
        BCRYPT_SUCCESS(BCryptGenRandom(NULL, dst, (ULONG)n,
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return true;
#  endif
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
