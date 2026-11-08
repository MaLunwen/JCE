/* Complete production adapter with typed native ports; no SDL or engine link. */
#include <jce/os/core/jce_alloc.h>
#include <windows.h>
#include <stdio.h>
#include <jce/os/platform/jce_entropy.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CHECK(test) do { if (!(test)) { \
    printf("XP_PATH_CHECK_FAILED:%d\n", __LINE__); return 1; } } while (0)
static const WCHAR *input;
static size_t input_size;
static unsigned allocations, frees, module_calls, conversion_calls;
static int force_truncation, use_real_image;
static DWORD last_flags;
void *jce_malloc(size_t size) { ++allocations; return malloc(size); }
void jce_free(void *memory) { ++frees; free(memory); }
static DWORD WINAPI module_port(HMODULE module, LPWSTR output, DWORD capacity)
{
    ++module_calls;
    if (use_real_image) return GetModuleFileNameW(module, output, capacity);
    if (force_truncation || input_size >= capacity) return capacity;
    if (input_size) memcpy(output, input, input_size * sizeof *output);
    output[input_size] = 0;
    return (DWORD)input_size;
}
static int WINAPI conversion_port(UINT page, DWORD flags, LPCWCH wide, int size,
    LPSTR output, int capacity, LPCCH replacement, LPBOOL used)
{
    ++conversion_calls;
    last_flags = flags;
    /* An invalid source must be rejected before reaching this boundary. */
    return WideCharToMultiByte(page, flags, wide, size, output, capacity, replacement, used);
}
#define GetModuleFileNameW module_port
#define WideCharToMultiByte conversion_port
#ifndef JCE_EXECUTABLE_PATH_HEADER
#define JCE_EXECUTABLE_PATH_HEADER "os/platform/jce_executable_path_native_internal.h"
#endif
#include JCE_EXECUTABLE_PATH_HEADER
#undef GetModuleFileNameW
#undef WideCharToMultiByte
static void select_input(const WCHAR *value, size_t count)
{
    input = value; input_size = count; allocations = frees = module_calls = conversion_calls = 0;
    force_truncation = use_real_image = 0; last_flags = 0xffffffffu;
}
static int reject_surrogate(const WCHAR *value, size_t count)
{
    char output[64] = "sentinel";
    select_input(value, count);
    const bool accepted = jce_fs_native_get_executable_path_internal(output, sizeof output);
    if (accepted || conversion_calls != 0)
        puts("JCE_XP_PATH_SURROGATE_REJECT_FAILED");
    CHECK(!accepted);
    CHECK(output[0] == 0 && conversion_calls == 0 && allocations == frees);
    return 0;
}
int main(void)
{
    unsigned char entropy[16];
    CHECK(jce_host_random_bytes(entropy, sizeof entropy));
    const unsigned char expected_bmp[] = {0x43, 0x3a, 0x5c, 0x70, 0x2e, 0x65, 0x78, 0x65, 0};
    const unsigned char expected_pair[] = {0x43, 0x3a, 0x5c, 0xf0, 0x9f, 0x98, 0x83, 0};
    const WCHAR bmp[] = {0x43, 0x3a, 0x5c, 0x70, 0x2e, 0x65, 0x78, 0x65, 0};
    const WCHAR pair[] = {0x43, 0x3a, 0x5c, 0xd83d, 0xde03, 0};
    const WCHAR high[] = {0x43, 0x3a, 0x5c, 0xd800, 0};
    const WCHAR low[] = {0x43, 0x3a, 0x5c, 0xdc00, 0};
    const WCHAR high_ascii[] = {0x43, 0x3a, 0x5c, 0xd800, 0x78, 0};
    const WCHAR high_high[] = {0x43, 0x3a, 0x5c, 0xd800, 0xd801, 0xdc00, 0};
    char output[1024], exact[1024], tiny[2] = {'x', 'y'};
    CHECK(!jce_fs_native_get_executable_path_internal(NULL, 32));
    CHECK(!jce_fs_native_get_executable_path_internal(tiny, 0) && tiny[0] == 'x');
    select_input(bmp, 8);
    CHECK(!jce_fs_native_get_executable_path_internal(tiny, 1));
    CHECK(tiny[0] == 0 && tiny[1] == 'y' && allocations == frees);
    select_input(bmp, 8);
    CHECK(jce_fs_native_get_executable_path_internal(output, sizeof output));
    CHECK(memcmp(output, expected_bmp, sizeof expected_bmp) == 0 && conversion_calls == 2 && last_flags == 0 && allocations == frees);
    select_input(pair, 5);
    CHECK(jce_fs_native_get_executable_path_internal(output, sizeof output));
    CHECK(memcmp(output, expected_pair, sizeof expected_pair) == 0 && last_flags == 0 && allocations == frees);
    CHECK(reject_surrogate(high, 4) == 0);
    CHECK(reject_surrogate(low, 4) == 0);
    CHECK(reject_surrogate(high_ascii, 5) == 0);
    CHECK(reject_surrogate(high_high, 6) == 0);
    select_input(bmp, 0);
    CHECK(!jce_fs_native_get_executable_path_internal(output, sizeof output));
    CHECK(output[0] == 0 && conversion_calls == 0 && allocations == frees);
    {
        WCHAR long_path[301];
        for (size_t i = 0; i < 300; ++i) long_path[i] = 'x';
        long_path[300] = 0;
        select_input(long_path, 300);
        CHECK(jce_fs_native_get_executable_path_internal(output, sizeof output));
        CHECK(strlen(output) == 300 && module_calls == 2 && allocations == frees);
    }
    select_input(bmp, 8); force_truncation = 1;
    CHECK(!jce_fs_native_get_executable_path_internal(output, sizeof output));
    CHECK(output[0] == 0 && module_calls == 8 && conversion_calls == 0 && allocations == frees);
    select_input(NULL, 0); use_real_image = 1;
    CHECK(jce_fs_native_get_executable_path_internal(output, sizeof output));
    CHECK(strlen(output) > 3 && output[1] == ':' && allocations == frees);
    CHECK(!jce_fs_native_get_executable_path_internal(exact, (uint32_t)strlen(output)) && exact[0] == 0);
    CHECK(jce_fs_native_get_executable_path_internal(exact, (uint32_t)strlen(output) + 1));
    CHECK(strcmp(output, exact) == 0 && allocations == frees);
    puts("JCE_XP_EXECUTABLE_PATH_OK");
    return 0;
}
