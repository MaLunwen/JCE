/* Windows compatibility fixture; no Editor or renderer is configured. */
#include <jce/jce_compat.h>
#include <windows.h>
#include <cstddef>
#include <cstdio>
#include <cstring>

extern int jce_xp_core_main();
extern "C" {
extern const unsigned char assets_pak_data[];
extern const std::size_t assets_pak_data_size;
}
static_assert(sizeof(assets_pak_data_size) == sizeof(void *), "native size_t width required");

static int run_probe()
{
    const unsigned char expected[] = {0x31, 0x7c, 0xa5, 0xe2, 0x19, 0x63, 0xbd, 0x4f};
    const int core = jce_xp_core_main();
    if (core != 0)
        return core;
    if (assets_pak_data_size != sizeof(expected) ||
        std::memcmp(assets_pak_data, expected, sizeof(expected)) != 0) {
        std::puts("JCE_XP_EMBEDDING_SYMBOL_MISMATCH");
        return 20;
    }
    HMODULE module = GetModuleHandleA(nullptr);
    HRSRC resource = FindResourceA(module, MAKEINTRESOURCEA(101), MAKEINTRESOURCEA(10));
    if (!resource || SizeofResource(module, resource) != sizeof(expected)) {
        std::puts("JCE_XP_EMBEDDING_RESOURCE_SIZE_FAIL");
        return 21;
    }
    HGLOBAL loaded = LoadResource(module, resource);
    const void *bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || std::memcmp(bytes, expected, sizeof(expected)) != 0) {
        std::puts("JCE_XP_EMBEDDING_RESOURCE_BYTES_FAIL");
        return 22;
    }
    std::printf("JCE_XP_EMBEDDING_POINTER_BITS=%u SIZE=%u\n",
                static_cast<unsigned>(sizeof(void *) * 8),
                static_cast<unsigned>(assets_pak_data_size));
    std::puts("JCE_XP_EMBEDDING_PROBE_OK");
    return 0;
}

int main()
{
    return run_probe();
}

/* Entry adapter only: both subsystem startup conventions run the same checks. */
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    return run_probe();
}
