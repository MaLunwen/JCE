/* C99 complete-header fixture: real Win32 threads, unchanged mimalloc atomics.
 * Typed lookup ports exercise optional capability policy without fabricating
 * production exports. No window, GPU, SDL or JCE allocator is involved.
 */
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static HMODULE fixture_kernel, fixture_other_module;
static BOOL fixture_cpu_present = TRUE, fixture_kernel_present = TRUE;
static unsigned fixture_module_calls, fixture_cpu_probes, fixture_forwarded;
static unsigned fixture_cpu_calls;
static HMODULE fixture_last_module, fixture_cpu_module;
static LPCSTR fixture_last_name;
static HMODULE WINAPI fixture_module_handle(LPCWSTR name);
static FARPROC WINAPI fixture_proc_address(HMODULE module, LPCSTR name);
#define JCE_XP_MIMALLOC_GET_MODULE_HANDLE_W fixture_module_handle
#define JCE_XP_MIMALLOC_GET_PROC_ADDRESS fixture_proc_address
#ifndef JCE_XP_MIMALLOC_PORT_HEADER
#define JCE_XP_MIMALLOC_PORT_HEADER "os/platform/compatibility/jce_windows_xp_mimalloc_internal.h"
#endif
#include JCE_XP_MIMALLOC_PORT_HEADER
/* Consume the complete actual upstream header, never a copied lock subset.
 * LLVM-MinGW supports its C11 atomic extension in this strict C99 target.
 */
#include <mimalloc/atomic.h>

#define CHECK(label, expression) do { if (!(expression)) { \
    fprintf(stderr, "JCE_XP_MIMALLOC_ASSERT_FAILED:%s\n", label); return 1; } } while (0)
#define THREAD_COUNT 4
#define ITERATIONS 2500
#define WAIT_MS 10000

static DWORD WINAPI fixture_current_processor(void) {
    ++fixture_cpu_calls;
    return 37;
}
static INT_PTR WINAPI fixture_other_export(void) { return 0; }
static FARPROC fixture_sentinel(void) {
    return (FARPROC)(void (*)(void))fixture_other_export;
}
static HMODULE WINAPI fixture_module_handle(LPCWSTR name) {
    ++fixture_module_calls;
    if (wcscmp(name, L"kernel32.dll") != 0) return NULL;
    return fixture_kernel_present ? fixture_kernel : NULL;
}
static FARPROC WINAPI fixture_proc_address(HMODULE module, LPCSTR name) {
    if ((uintptr_t)name > UINT16_MAX && module == fixture_kernel &&
        strcmp(name, "GetCurrentProcessorNumber") == 0) {
        ++fixture_cpu_probes;
        fixture_cpu_module = module;
        return fixture_cpu_present ?
            (FARPROC)(void (*)(void))fixture_current_processor : NULL;
    }
    ++fixture_forwarded;
    fixture_last_module = module;
    fixture_last_name = name;
    return fixture_sentinel();
}
static void fixture_reset(void) {
    fixture_cpu_present = TRUE;
    fixture_kernel_present = TRUE;
    fixture_module_calls = fixture_cpu_probes = fixture_forwarded = fixture_cpu_calls = 0;
    fixture_last_module = fixture_cpu_module = NULL;
    fixture_last_name = NULL;
}
static int lock_case(void) {
    mi_lock_t lock = MI_LOCK_INITIALIZER;
    CHECK("static-zero", jce_xp_sync_load(&lock.mutex.Ptr) == 0);
    mi_lock_init(&lock);
    CHECK("free-try", mi_lock_try_acquire(&lock));
    /* The omitted-try control fails here before another operation or thread.
     * No deliberately broken lock is ever used for concurrent non-atomic data.
     */
    CHECK("try-publishes-held-state", jce_xp_sync_load(&lock.mutex.Ptr) == 1);
    CHECK("held-try", !mi_lock_try_acquire(&lock));
    mi_lock_release(&lock);
    CHECK("release-publishes-free-state", jce_xp_sync_load(&lock.mutex.Ptr) == 0);
    mi_lock_acquire(&lock);
    CHECK("acquire-publishes-held-state", jce_xp_sync_load(&lock.mutex.Ptr) == 1);
    mi_lock_release(&lock);
    mi_lock_done(&lock);
    return 0;
}
typedef struct FixtureRun {
    mi_lock_t lock;
    HANDLE start;
    unsigned counter;
} FixtureRun;
static DWORD WINAPI fixture_worker(LPVOID opaque) {
    FixtureRun *run = opaque;
    unsigned i;
    if (WaitForSingleObject(run->start, WAIT_MS) != WAIT_OBJECT_0) return 2;
    for (i = 0; i < ITERATIONS; ++i) {
        mi_lock_acquire(&run->lock);
        ++run->counter;
        mi_lock_release(&run->lock);
    }
    return 0;
}
static int concurrency_case(void) {
    FixtureRun run = { MI_LOCK_INITIALIZER, NULL, 0 };
    HANDLE workers[THREAD_COUNT];
    unsigned i;
    run.start = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK("start-event", run.start != NULL);
    mi_lock_init(&run.lock);
    for (i = 0; i < THREAD_COUNT; ++i) {
        workers[i] = CreateThread(NULL, 0, fixture_worker, &run, 0, NULL);
        CHECK("create-worker", workers[i] != NULL);
    }
    CHECK("start-workers", SetEvent(run.start));
    for (i = 0; i < THREAD_COUNT; ++i) {
        DWORD status = 99;
        CHECK("join-worker", WaitForSingleObject(workers[i], WAIT_MS) == WAIT_OBJECT_0);
        CHECK("worker-status", GetExitCodeThread(workers[i], &status) && status == 0);
        CHECK("close-worker", CloseHandle(workers[i]));
    }
    CHECK("close-start", CloseHandle(run.start));
    CHECK("protected-counter", run.counter == THREAD_COUNT * ITERATIONS);
    mi_lock_done(&run.lock);
    return 0;
}
static const char *const fixture_numa_names[] = {
    "GetNumaProcessorNodeEx", "GetNumaNodeProcessorMaskEx",
    "GetNumaProcessorNode", "GetNumaNodeProcessorMask", "GetNumaHighestNodeNumber"
};
static int exports_case(BOOL cpu_present, BOOL kernel_present) {
    unsigned i;
    fixture_reset();
    fixture_cpu_present = cpu_present;
    fixture_kernel_present = kernel_present;
    for (i = 0; i < sizeof fixture_numa_names / sizeof fixture_numa_names[0]; ++i) {
        FARPROC found = GetProcAddress(fixture_other_module, fixture_numa_names[i]);
        if (cpu_present && kernel_present) {
            CHECK("numa-present-forwards", found == fixture_sentinel());
            CHECK("numa-original-module", fixture_last_module == fixture_other_module);
            CHECK("numa-original-name", fixture_last_name == fixture_numa_names[i]);
        } else {
            CHECK("numa-absent-refuses", found == NULL);
        }
    }
    CHECK("borrow-kernel-per-probe", fixture_module_calls == 5);
    CHECK("real-cpu-probe-count", fixture_cpu_probes == (kernel_present ? 5u : 0u));
    CHECK("optional-forward-count", fixture_forwarded == (cpu_present && kernel_present ? 5u : 0u));
    CHECK("lookup-never-invokes-cpu", fixture_cpu_calls == 0);
    if (kernel_present) CHECK("cpu-probe-borrows-kernel", fixture_cpu_module == fixture_kernel);
    if (cpu_present && kernel_present) {
        CHECK("actual-cpu-value", GetCurrentProcessorNumber() == 37);
        CHECK("actual-cpu-called", fixture_cpu_calls == 1);
        CHECK("actual-cpu-reprobed", fixture_cpu_probes == 6 && fixture_module_calls == 6);
    }
    return 0;
}
static int ordinal_case(void) {
    const uintptr_t ordinals[] = { 0, 1, UINT16_MAX };
    unsigned i;
    fixture_reset();
    fixture_cpu_present = FALSE;
    for (i = 0; i < sizeof ordinals / sizeof ordinals[0]; ++i) {
        LPCSTR ordinal = (LPCSTR)ordinals[i];
        CHECK("ordinal-forwarded", GetProcAddress(fixture_other_module, ordinal) == fixture_sentinel());
        CHECK("ordinal-unchanged", fixture_last_name == ordinal);
        CHECK("ordinal-module-unchanged", fixture_last_module == fixture_other_module);
    }
    CHECK("ordinal-no-probe", fixture_module_calls == 0 && fixture_cpu_probes == 0);
    CHECK("ordinal-forward-count", fixture_forwarded == 3);
    return 0;
}
static int forwarding_case(void) {
    const char *const names[] = { "GetCurrentProcessorNumberEx", "GetLargePageMinimum",
                                 "JceUnrelatedExport", "GetNumaProcessorNodeExtra" };
    unsigned i;
    fixture_reset();
    fixture_cpu_present = FALSE;
    for (i = 0; i < sizeof names / sizeof names[0]; ++i) {
        CHECK("unrelated-forwarded", GetProcAddress(fixture_other_module, names[i]) == fixture_sentinel());
        CHECK("unrelated-name-unchanged", fixture_last_name == names[i]);
        CHECK("unrelated-module-unchanged", fixture_last_module == fixture_other_module);
    }
    CHECK("unrelated-no-probe", fixture_module_calls == 0 && fixture_cpu_probes == 0);
    CHECK("unrelated-forward-count", fixture_forwarded == 4);
    return 0;
}
static int all_case(void) {
    CHECK("all-locks", lock_case() == 0);
    CHECK("all-concurrency", concurrency_case() == 0);
    CHECK("all-present", exports_case(TRUE, TRUE) == 0);
    CHECK("all-missing", exports_case(FALSE, TRUE) == 0);
    CHECK("all-kernel-missing", exports_case(TRUE, FALSE) == 0);
    CHECK("all-ordinals", ordinal_case() == 0);
    CHECK("all-forwarding", forwarding_case() == 0);
    return 0;
}
static void fixture_fail_stop_marker(const char *name) {
    printf("JCE_XP_MIMALLOC_FAIL_STOP:%s\n", name);
    fflush(stdout); /* TerminateProcess bypasses CRT exit/flush. */
}
int main(int argc, char **argv) {
    int result;
#ifdef JCE_XP_MIMALLOC_OMITTED_TRY_CONTROL
    const char *name = argc == 2 ? argv[1] : "locks";
#else
    const char *name = argc == 2 ? argv[1] : "all";
#endif
    /* The native module lookup occurs before any typed port is called. */
    fixture_kernel = GetModuleHandleW(L"kernel32.dll");
    fixture_other_module = GetModuleHandleW(L"ntdll.dll");
    CHECK("native-borrowed-modules", fixture_kernel && fixture_other_module);
#ifdef JCE_XP_MIMALLOC_OMITTED_TRY_CONTROL
    /* A deliberately omitted atomic cannot be admitted to the threaded case. */
    if (strcmp(name, "locks") != 0) return 2;
#endif
    if (strcmp(name, "all") == 0) result = all_case();
    else if (strcmp(name, "locks") == 0) result = lock_case();
    else if (strcmp(name, "concurrency") == 0) result = concurrency_case();
    else if (strcmp(name, "exports-present") == 0) result = exports_case(TRUE, TRUE);
    else if (strcmp(name, "exports-missing") == 0) result = exports_case(FALSE, TRUE);
    else if (strcmp(name, "kernel-missing") == 0) result = exports_case(TRUE, FALSE);
    else if (strcmp(name, "ordinals") == 0) result = ordinal_case();
    else if (strcmp(name, "forwarding") == 0) result = forwarding_case();
    else if (strcmp(name, "fail-current") == 0) {
        fixture_reset();
        fixture_cpu_present = FALSE;
        fixture_fail_stop_marker(name);
        (void)GetCurrentProcessorNumber();
        CHECK("missing-cpu-must-fail-stop", false);
    } else if (strcmp(name, "fail-lock") == 0) {
        mi_lock_t lock = MI_LOCK_INITIALIZER;
        jce_xp_sync_store(&lock.mutex.Ptr, 2);
        fixture_fail_stop_marker(name);
        (void)mi_lock_try_acquire(&lock);
        CHECK("invalid-lock-must-fail-stop", false);
    } else if (strcmp(name, "fail-release") == 0) {
        mi_lock_t lock = MI_LOCK_INITIALIZER;
        fixture_fail_stop_marker(name);
        mi_lock_release(&lock);
        CHECK("unowned-release-must-fail-stop", false);
    } else {
        fputs("JCE_XP_MIMALLOC_USAGE\n", stderr);
        return 2;
    }
    if (!result) printf("JCE_XP_MIMALLOC_OK:%s\n", name);
    return result;
}
