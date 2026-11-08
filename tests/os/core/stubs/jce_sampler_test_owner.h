/* Shared private fake-native port around the actual first-party sampler owner.
 * Callers may rename jce_sampler_init/shutdown before inclusion for isolation.
 * port_reset requires completed sampler cleanup; it is a test setup operation. */
#ifndef JCE_SAMPLER_TEST_OWNER_H
#define JCE_SAMPLER_TEST_OWNER_H

#include "unity.h"
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_filesystem.h>
#include "os/core/jce_memory.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *sampler_port_getenv(const char *key);
void *sampler_port_malloc(size_t size);
void *sampler_port_calloc(size_t count, size_t size);
void sampler_port_free(void *memory);
bool JCE_CALL sampler_port_write_all(const char *path, const void *memory,
                                     uint64_t bytes);

/* Keep the compiler ABI native, selecting only the observation implementation. */
#if JCE_PLATFORM_WINDOWS
#define SAMPLER_PORT_HOST_WINDOWS 1
#else
#define SAMPLER_PORT_HOST_WINDOWS 0
#endif
#undef JCE_PLATFORM_WINDOWS
#define JCE_PLATFORM_WINDOWS 1
#define JCE_SAMPLER_TEST_PORT "jce_sampler_test_port.h"
#define jce__malloc_tracked sampler_port_malloc
#define jce__calloc_tracked sampler_port_calloc
#define jce__free_tracked sampler_port_free
#define jce_fs_host_write_all sampler_port_write_all
#define getenv sampler_port_getenv
#ifndef JCE_TEST_SAMPLER_OWNER
#define JCE_TEST_SAMPLER_OWNER "os/core/jce_sampler.c"
#endif
#include JCE_TEST_SAMPLER_OWNER
#undef getenv

enum { PORT_HANDLES = 1024, PORT_THREADS = 192 };
typedef struct {
    DWORD tid;
    uint64_t born;
    uint64_t exited;
    bool terminated; /* distinct from undefined/live exit-time contents */
    unsigned suspend_count;
} PortLifetime;
struct SamplerPortHandle {
    unsigned kind, closes;
    DWORD tid;
    DWORD access;
    uint64_t born, pc;
    PortLifetime *lifetime;
    wchar_t name[48];
};
typedef struct { DWORD tid; uint64_t born, pc; const wchar_t *name; } PortThread;
static struct SamplerPortHandle s_handles[PORT_HANDLES];
static unsigned s_handle_count, s_created_workers, s_snapshot_index;
static PortLifetime s_lifetimes[PORT_THREADS];
static unsigned s_lifetime_count;
static PortThread s_roster[PORT_THREADS];
static unsigned s_roster_count, s_suspends, s_resumes, s_waits;
static unsigned s_times_queries;
static unsigned s_thread_waits;
static unsigned s_writes, s_slot_frees, s_invalid_worker_access;
static DWORD s_wait_result;
static bool s_stop_in_sleep, s_resume_failure, s_identity_failure, s_exit_query_failure;
static bool s_all;
static unsigned s_sleeps;
static int s_delay;
static SampSlot *s_initial_slots;
static bool s_slots_live;
static char s_report[512u * 1024u];
/* KernelBase / Kernel32 are borrowed module observations, never fake DLLs. */
static unsigned s_module_tokens[2];
static bool s_native_modules[2], s_native_exports[2], s_description_null;
static HRESULT s_description_result;
static unsigned s_module_queries, s_export_queries, s_description_queries;
static unsigned s_description_frees, s_resolved_module;
/* Keep optional DbgHelp observations separate from thread-name assertions. */
static unsigned s_dbghelp_module_token;
static bool s_dbghelp_module_available, s_refresh_export_available;
static BOOL s_refresh_result;
static DWORD s_last_error, s_refresh_error;
static unsigned s_dbghelp_module_queries, s_refresh_export_queries, s_refresh_calls;
static HANDLE s_refresh_process;

static PortLifetime *port_lifetime(DWORD tid, uint64_t born)
{
    for (unsigned i = 0; i < s_lifetime_count; ++i)
        if (s_lifetimes[i].tid == tid && s_lifetimes[i].born == born)
            return &s_lifetimes[i];
    if (s_lifetime_count == PORT_THREADS) return NULL;
    PortLifetime *life = &s_lifetimes[s_lifetime_count++];
    life->tid = tid; life->born = born; life->exited = 0;
    life->terminated = false; life->suspend_count = 0;
    return life;
}

static HANDLE port_handle(unsigned kind)
{
    if (s_handle_count == PORT_HANDLES) return NULL;
    HANDLE h = &s_handles[s_handle_count++];
    memset(h, 0, sizeof(*h));
    h->kind = kind;
    return h;
}

static void port_roster(unsigned count, DWORD first_tid, uint64_t born)
{
    TEST_ASSERT_TRUE(count <= PORT_THREADS);
    s_roster_count = count;
    for (unsigned i = 0; i < count; ++i) {
        s_roster[i].tid = first_tid + i;
        s_roster[i].born = born + i;
        s_roster[i].pc = 0x1000u + i * 16u;
        s_roster[i].name = L"worker";
    }
}

static char *sampler_port_getenv(const char *key)
{
    if (!strcmp(key, "JCE_SAMPLER")) return "100";
    if (!strcmp(key, "JCE_SAMPLER_ALL")) return s_all ? "1" : "0";
    if (!strcmp(key, "JCE_SAMPLER_DELAY")) return s_delay ? "4000" : "0";
    if (!strcmp(key, "JCE_SAMPLER_OUT")) return "sampler-control-output";
    return NULL;
}

void *sampler_port_malloc(size_t size) { return jce_malloc(size); }
void *sampler_port_calloc(size_t count, size_t size)
{
    void *memory = jce_malloc(count * size);
    if (memory) memset(memory, 0, count * size);
    if (memory && count == SAMP_CAP && size == sizeof(SampSlot) && !g_s.slots) {
        s_initial_slots = memory;
        s_slots_live = true;
    }
    return memory;
}
void sampler_port_free(void *memory)
{
    if (memory && memory == s_initial_slots) {
        ++s_slot_frees;
        s_slots_live = false;
    }
    jce_free(memory);
}
bool JCE_CALL sampler_port_write_all(const char *path, const void *memory,
                                     uint64_t bytes)
{
    (void)path;
    ++s_writes;
    if (bytes >= sizeof(s_report)) return false;
    memcpy(s_report, memory, (size_t)bytes);
    s_report[bytes] = '\0';
    return true;
}

static HANDLE GetCurrentProcess(void) { return NULL; }
static HANDLE GetCurrentThread(void) { return NULL; }
static DWORD GetCurrentProcessId(void) { return 7; }
static DWORD GetCurrentThreadId(void) { return 999; }
static BOOL DuplicateHandle(HANDLE from, HANDLE thread, HANDLE to, HANDLE *out,
                            DWORD access, BOOL inherit, DWORD options)
{
    (void)from; (void)thread; (void)to; (void)access; (void)inherit; (void)options;
    *out = port_handle(1);
    if (*out) {
        (*out)->lifetime = port_lifetime(0, 1);
        (*out)->access = access;
    }
    return *out != NULL;
}
static HANDLE CreateThread(void *security, size_t stack, DWORD (*entry)(LPVOID),
                           LPVOID arg, DWORD flags, DWORD *tid)
{
    (void)security; (void)stack; (void)entry; (void)arg; (void)flags; (void)tid;
    ++s_created_workers;
    return port_handle(2);
}
static BOOL SetThreadPriority(HANDLE handle, int priority)
{ (void)handle; (void)priority; return TRUE; }
static BOOL CloseHandle(HANDLE handle)
{
    if (!handle || handle == INVALID_HANDLE_VALUE) return FALSE;
    ++handle->closes;
    return TRUE;
}
static DWORD WaitForSingleObject(HANDLE handle, DWORD timeout_ms)
{
    if (handle && handle->kind == 2) { ++s_waits; return s_wait_result; }
    ++s_thread_waits;
    if (!handle || timeout_ms != 0 || !(handle->access & SYNCHRONIZE) ||
        s_exit_query_failure) return WAIT_FAILED;
    return handle->lifetime->terminated ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
static LONG InterlockedCompareExchange(volatile LONG *value, LONG desired,
                                      LONG expected)
{
    LONG prior = *value;
    if (prior == expected) *value = desired;
    return prior;
}
static LONG InterlockedExchange(volatile LONG *value, LONG desired)
{ LONG prior = *value; *value = desired; return prior; }
static void Sleep(DWORD ms)
{
    (void)ms;
    ++s_sleeps;
    if (s_stop_in_sleep || s_sleeps > 1) InterlockedExchange(&g_s.stop, 1);
}
static HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid)
{ (void)flags; (void)pid; return port_handle(3); }
static BOOL port_next(THREADENTRY32 *entry)
{
    if (s_snapshot_index >= s_roster_count) {
        SetLastError(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    entry->th32OwnerProcessID = GetCurrentProcessId();
    entry->th32ThreadID = s_roster[s_snapshot_index++].tid;
    return TRUE;
}
static BOOL Thread32First(HANDLE snapshot, THREADENTRY32 *entry)
{ (void)snapshot; s_snapshot_index = 0; return port_next(entry); }
static BOOL Thread32Next(HANDLE snapshot, THREADENTRY32 *entry)
{ (void)snapshot; return port_next(entry); }
static HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid)
{
    (void)access; (void)inherit;
    for (unsigned i = 0; i < s_roster_count; ++i) {
        if (s_roster[i].tid != tid) continue;
        HANDLE h = port_handle(4);
        if (!h) return NULL;
        h->tid = tid; h->access = access;
        h->born = s_roster[i].born; h->pc = s_roster[i].pc;
        h->lifetime = port_lifetime(h->tid, h->born);
        wcsncpy(h->name, s_roster[i].name, 47);
        return h;
    }
    return NULL;
}
static BOOL GetThreadTimes(HANDLE handle, FILETIME *created, FILETIME *exited,
                           FILETIME *kernel, FILETIME *user)
{
    (void)kernel; (void)user;
    ++s_times_queries;
    if (s_identity_failure || s_exit_query_failure ||
        !(handle->access & THREAD_QUERY_LIMITED_INFORMATION)) return FALSE;
    created->dwLowDateTime = (DWORD)handle->born;
    created->dwHighDateTime = (DWORD)(handle->born >> 32);
    exited->dwLowDateTime = (DWORD)handle->lifetime->exited;
    exited->dwHighDateTime = (DWORD)(handle->lifetime->exited >> 32);
    return TRUE;
}
static DWORD GetLastError(void) { return s_last_error; }
static void WINAPI SetLastError(DWORD error) { s_last_error = error; }
static HMODULE GetModuleHandleW(const wchar_t *name)
{
    if (wcscmp(name, L"dbghelp.dll") == 0) {
        ++s_dbghelp_module_queries;
        return s_dbghelp_module_available ? &s_dbghelp_module_token : NULL;
    }
    ++s_module_queries;
    const unsigned index = wcscmp(name, L"KernelBase.dll") == 0 ? 0u : 1u;
    TEST_ASSERT_TRUE(index == 0u || wcscmp(name, L"Kernel32.dll") == 0);
    return s_native_modules[index] ? &s_module_tokens[index] : NULL;
}
static FARPROC GetProcAddress(HMODULE module, const char *name)
{
    if (module == &s_dbghelp_module_token) {
        ++s_refresh_export_queries;
        TEST_ASSERT_EQUAL_STRING("SymRefreshModuleList", name);
        if (!s_refresh_export_available) return NULL;
        BOOL (WINAPI *function)(HANDLE) = sampler_port_sym_refresh;
        FARPROC address = NULL;
        TEST_ASSERT_EQUAL_UINT(sizeof function, sizeof address);
        memcpy(&address, &function, sizeof address);
        return address;
    }
    ++s_export_queries;
    TEST_ASSERT_EQUAL_STRING("GetThreadDescription", name);
    const unsigned index = module == &s_module_tokens[0] ? 0u : 1u;
    TEST_ASSERT_TRUE(module == &s_module_tokens[index]);
    if (!s_native_exports[index]) return NULL;
    s_resolved_module = index;
    HRESULT (WINAPI *function)(HANDLE, PWSTR *) = sampler_port_thread_description;
    FARPROC address = NULL;
    TEST_ASSERT_EQUAL_UINT(sizeof function, sizeof address);
    memcpy(&address, &function, sizeof address);
    return address;
}
static HRESULT WINAPI sampler_port_thread_description(HANDLE handle, PWSTR *description)
{
    ++s_description_queries;
    *description = s_description_null ? NULL : handle->name;
    return s_description_result;
}
static int WideCharToMultiByte(unsigned codepage, DWORD flags,
                              const wchar_t *wide, int count, char *text,
                              int capacity, const char *fallback, BOOL *used)
{
    (void)codepage; (void)flags; (void)count; (void)fallback; (void)used;
    int i = 0;
    while (wide[i] && i + 1 < capacity) { text[i] = (char)wide[i]; ++i; }
    text[i] = '\0';
    return i + 1;
}
static void *LocalFree(void *memory)
{
    TEST_ASSERT_NOT_NULL(memory);
    ++s_description_frees;
    return NULL;
}
static DWORD SuspendThread(HANDLE handle)
{ ++s_suspends; return handle->lifetime->suspend_count++; }
static BOOL GetThreadContext(HANDLE handle, CONTEXT *context)
{ context->Rip = context->Eip = context->Pc = handle->pc; return TRUE; }
static DWORD ResumeThread(HANDLE handle)
{
    ++s_resumes;
    if (s_resume_failure || handle->lifetime->terminated) return (DWORD)-1;
    if (!handle->lifetime->suspend_count) return (DWORD)-1;
    return handle->lifetime->suspend_count--;
}
static DWORD SymSetOptions(DWORD options) { return options; }
static BOOL SymInitialize(HANDLE process, const char *path, BOOL invade)
{ (void)process; (void)path; (void)invade; return TRUE; }
static BOOL WINAPI sampler_port_sym_refresh(HANDLE process)
{
    ++s_refresh_calls;
    s_refresh_process = process;
    SetLastError(s_refresh_error);
    return s_refresh_result;
}
static BOOL SymGetModuleInfo64(HANDLE process, DWORD64 pc,
                              IMAGEHLP_MODULE64 *module)
{ (void)process; (void)pc; strcpy(module->ModuleName, "control"); module->SymType = 0; return TRUE; }
static BOOL SymFromAddr(HANDLE process, DWORD64 pc, DWORD64 *displacement,
                       SYMBOL_INFO *symbol)
{
    (void)process;
    *displacement = 0; symbol->Size = 16;
    snprintf(symbol->Name, symbol->MaxNameLen, "pc_%llx", (unsigned long long)pc);
    return TRUE;
}
static DWORD64 SymGetModuleBase64(HANDLE process, DWORD64 pc)
{ (void)process; (void)pc; return 0; }
static BOOL SymGetLineFromAddr64(HANDLE process, DWORD64 pc,
                                DWORD *displacement, IMAGEHLP_LINE64 *line)
{ (void)process; (void)pc; (void)displacement; (void)line; return FALSE; }
static BOOL SymCleanup(HANDLE process) { (void)process; return TRUE; }

static void port_reset(void)
{
    TEST_ASSERT_NULL(g_s.worker);
    TEST_ASSERT_NULL(g_s.slots);
    TEST_ASSERT_EQUAL_UINT(0, g_thr_count);
    memset(&g_s, 0, sizeof(g_s));
    memset(g_thr, 0, sizeof(g_thr)); g_thr_count = 0;
    memset(s_handles, 0, sizeof(s_handles));
    memset(s_lifetimes, 0, sizeof(s_lifetimes)); s_lifetime_count = 0;
    s_handle_count = s_created_workers = s_snapshot_index = 0;
    s_roster_count = s_suspends = s_resumes = s_waits = s_times_queries = 0;
    s_thread_waits = 0;
    s_writes = s_slot_frees = s_invalid_worker_access = s_sleeps = 0;
    s_stop_in_sleep = s_resume_failure = s_identity_failure = false;
    s_exit_query_failure = false; s_all = true;
    s_delay = 0; s_wait_result = WAIT_OBJECT_0;
    s_initial_slots = NULL; s_slots_live = false; s_report[0] = '\0';
    s_native_modules[0] = s_native_modules[1] = true;
    s_native_exports[0] = s_native_exports[1] = true;
    s_description_null = false; s_description_result = 0;
    s_module_queries = s_export_queries = s_description_queries = 0;
    s_description_frees = 0; s_resolved_module = UINT32_MAX;
    s_dbghelp_module_available = s_refresh_export_available = true;
    s_refresh_result = TRUE; s_refresh_error = 0;
    s_last_error = ERROR_NO_MORE_FILES;
    s_dbghelp_module_queries = s_refresh_export_queries = s_refresh_calls = 0;
    s_refresh_process = NULL;
}
static void port_start(void)
{
    jce_sampler_init();
    s_initial_slots = g_s.slots; s_slots_live = s_initial_slots != NULL;
    TEST_ASSERT_NOT_NULL(g_s.worker);
    TEST_ASSERT_NOT_NULL(s_initial_slots);
}
static void port_delayed_worker_access(void)
{
    if (!s_slots_live || g_s.slots != s_initial_slots) {
        ++s_invalid_worker_access;
        return;
    }
    samp_record(0x2000, SAMP_TIDX_MAIN);
}


/* Keep fake allocator/environment/filesystem aliases local to the owner TU. */
#undef jce__malloc_tracked
#undef jce__calloc_tracked
#undef jce__free_tracked
#undef jce_fs_host_write_all
#undef JCE_SAMPLER_TEST_PORT
#undef JCE_PLATFORM_WINDOWS
#if SAMPLER_PORT_HOST_WINDOWS
#define JCE_PLATFORM_WINDOWS 1
#else
#define JCE_PLATFORM_WINDOWS 0
#endif
#undef SAMPLER_PORT_HOST_WINDOWS
#endif
