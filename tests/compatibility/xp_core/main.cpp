/* A1 COMPATIBILITY PROBE, NOT THE FULL JCE RUNTIME OR SDK.
 * Uses a first-party JCE OS adapter and the XP-targeted C++20 runtime.
 */
#include <jce/jce_compat.h>
#include <jce/os/platform/jce_entropy.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

#if !defined(JCE_WINXP_LEGACY) || !JCE_WINXP_LEGACY
# error "The A1 XP probe must opt into the XP compatibility profile"
#endif
#if JCE_ARCH_X64
static_assert(JCE_MIN_WIN32_WINNT == 0x0502, "XP x64 must target NT 5.2");
#elif JCE_ARCH_X86
static_assert(JCE_MIN_WIN32_WINNT == 0x0501, "XP x86 must target NT 5.1");
#else
# error "XP probe requires x86 or x64"
#endif

static std::mutex gate;
static std::atomic<int> counter{0};
int main()
{
    unsigned char bytes[32] = {0};
    /* true means OS API success; bytes inequality would NOT prove security. */
    if (!jce_host_random_bytes(bytes, sizeof(bytes))) {
        std::puts("JCE_XP_ENTROPY_OS_FAIL");
        return 4;
    }
    std::puts("JCE_XP_ENTROPY_OS_OK");
    std::thread worker([] {
        std::lock_guard<std::mutex> lock(gate);
        counter.fetch_add(1);
    });
    worker.join();
    std::printf("JCE_XP_THREAD=%d\n", counter.load());
    if (counter.load() != 1)
        return 5;
    std::puts("JCE_XP_A1_PROBE_OK");
    return 0;
}
