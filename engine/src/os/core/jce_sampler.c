/*
 * jce_sampler.c  Sampling profiler for the main thread.  See the header for why
 * this exists rather than another layer of rdtsc brackets.
 */

#include "os/core/jce_sampler.h"

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_log.h>
#include "os/core/jce_memory.h"

#define LOG_TAG "sampler"

/* #if, not #ifdef: JCE_PLATFORM_WINDOWS is ALWAYS defined -- 1 on
 * Windows and 0 elsewhere (jce_defs.h) -- so #ifdef is true on every
 * platform and this file pulled <windows.h> into the wasm build. The
 * non-Windows stub at the bottom was already written and unreachable.
 * 47 other sites in engine/ use #if; this was the only #ifdef. */
#if JCE_PLATFORM_WINDOWS

/* Native thread suspend + dbghelp, the same class of platform work the crash
 * handler does, and listed alongside it in the lint allowlists for the same
 * reason: there is no portable way to read another thread's program counter. */
#if defined(JCE_SAMPLER_TEST_PORT)
/* Private deterministic owner tests provide only the native observation port. */
#include JCE_SAMPLER_TEST_PORT
#else
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>   /* thread enumeration for the all-thread sweep */
#pragma comment(lib, "dbghelp.lib")
#endif

#include "os/platform/jce_thread_description_native_internal.h"
#include "os/platform/jce_dbghelp_native_internal.h"
#include <jce/os/core/jce_filesystem.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Open-addressed PC histogram. Sized so a 60-second run at 1 kHz cannot fill
 * it: distinct PCs in a hot loop are in the thousands, not the millions. */
#define SAMP_CAP   (1u << 16)
#define SAMP_MASK  (SAMP_CAP - 1u)
#define SAMP_LINE_MAX 8192u   /* distinct (function, line) rows kept */

/* Keyed on (thread, pc), not pc alone.
 *
 * The first whole-process sweep reported every thread at an identical 1.564%
 * -- because it sampled each thread once per tick, so the per-thread count was
 * just the number of ticks. It proved the threads existed and nothing else.
 * Carrying the thread index in the key makes the report able to say WHICH
 * thread is in RtlAcquireSRWLockExclusive, which is the only form of the
 * question that is about contention. */
typedef struct {
    uint64_t pc;      /* 0 = free slot */
    uint32_t count;
    uint16_t tidx;    /* index into g_thr, or SAMP_TIDX_MAIN */
} SampSlot;

#define SAMP_TIDX_MAIN 0xFFFFu

static struct {
    HANDLE      target;         /* duplicated handle to the profiled thread */
    HANDLE      worker;
    volatile LONG stop;
    int         hz;
    char        out_path[512];
    SampSlot   *slots;
    int         delay_ms;       /* warm-up skipped before sampling starts */
    int         all_threads;    /* JCE_SAMPLER_ALL: sweep the whole process */
    uint64_t    total;          /* samples taken */
    uint64_t    missed;         /* suspends that produced no usable context */
    uint64_t    thread_cap_skipped; /* refresh observations beyond session cap */
    uint64_t    thread_open_failed;
    uint64_t    thread_identity_failed;
    uint64_t    thread_snapshot_failed;
    uint64_t    resume_failed;
    bool        target_suspended; /* only the increment acquired by this sampler */
} g_s;

static void samp_record(uint64_t pc, uint16_t tidx)
{
    const uint64_t k = pc ^ ((uint64_t)tidx * 0xD6E8FEB86659FD93ull);
    uint32_t i = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 45) & SAMP_MASK;
    for (uint32_t probe = 0; probe < SAMP_CAP; probe++) {
        SampSlot *s = &g_s.slots[i];
        if (s->pc == pc && s->tidx == tidx) { s->count++; return; }
        if (s->pc == 0) { s->pc = pc; s->tidx = tidx; s->count = 1; return; }
        i = (i + 1u) & SAMP_MASK;
    }
    /* Table full: drop the sample rather than corrupt the histogram.  Shows up
     * as total > sum(count), which the report prints. */
}

/* At most 128 thread lifetimes in this process, excluding the sampler itself.
 *
 * Lock contention does not happen on the thread you are profiling -- it happens
 * on the ones waiting. A main-thread-only profile shows the SYMPTOM (a wait) and
 * never the holder, so JCE_SAMPLER_ALL=1 sweeps the whole process. The list is
 * refreshed periodically because pools spawn and retire threads. A retained
 * entry owns its histogram identity until shutdown, even after thread exit.
 * New lifetimes beyond this bounded session table are explicitly skipped. */
#define SAMP_MAX_THREADS 128u

typedef struct {
    HANDLE h;
    DWORD tid;
    uint64_t created; /* OS creation timestamp distinguishes reused TIDs */
    uint64_t samples;
    char name[48];
    bool seen;
    bool suspended; /* sampler owns exactly one acquired suspend increment */
} SampThread;
static SampThread g_thr[SAMP_MAX_THREADS];
static uint32_t   g_thr_count;

static void samp_close_threads(void)
{
    for (uint32_t i = 0; i < g_thr_count; i++)
        if (g_thr[i].h) CloseHandle(g_thr[i].h);
    memset(g_thr, 0, sizeof g_thr);
    g_thr_count = 0;
}

static void samp_refresh_threads(DWORD self_tid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) { g_s.thread_snapshot_failed++; return; }
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te;
    memset(&te, 0, sizeof te);
    te.dwSize = sizeof te;
    BOOL have_thread = Thread32First(snap, &te);
    if (!have_thread && GetLastError() != ERROR_NO_MORE_FILES) {
        g_s.thread_snapshot_failed++;
        CloseHandle(snap);
        return;
    }
    for (uint32_t t = 0; t < g_thr_count; t++) g_thr[t].seen = false;
    while (have_thread) {
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self_tid) {
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                  THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                  FALSE, te.th32ThreadID);
            if (!h) {
                g_s.thread_open_failed++;
            } else {
                FILETIME created, exited, kernel, user;
                if (!GetThreadTimes(h, &created, &exited, &kernel, &user)) {
                    /* A TID alone cannot safely recover historical ownership. */
                    g_s.thread_identity_failed++;
                    CloseHandle(h);
                } else {
                    const uint64_t identity = ((uint64_t)created.dwHighDateTime << 32) |
                                              (uint64_t)created.dwLowDateTime;
                    uint32_t t = 0;
                    for (; t < g_thr_count; t++)
                        if (g_thr[t].tid == te.th32ThreadID &&
                            g_thr[t].created == identity) break;
                    if (t == g_thr_count && t == SAMP_MAX_THREADS) {
                        g_s.thread_cap_skipped++;
                        CloseHandle(h);
                    } else {
                        SampThread *thread = &g_thr[t];
                        if (t == g_thr_count) {
                            memset(thread, 0, sizeof *thread);
                            thread->tid = te.th32ThreadID;
                            thread->created = identity;
                            PWSTR desc = NULL;
                            if (SUCCEEDED(jce_native_get_thread_description_internal(h, &desc)) &&
                                desc && desc[0])
                                WideCharToMultiByte(CP_UTF8, 0, desc, -1, thread->name,
                                                    (int)sizeof thread->name, NULL, NULL);
                            if (desc) LocalFree(desc);
                            thread->name[sizeof thread->name - 1u] = '\0';
                            if (!thread->name[0])
                                snprintf(thread->name, sizeof thread->name, "tid-%lu",
                                         (unsigned long)thread->tid);
                            g_thr_count++;
                        }
                        /* Keep handles and immutable indexes for retained lives.
                         * A transient observation handle cannot rekey samples. */
                        if (thread->h) CloseHandle(h);
                        else thread->h = h;
                        thread->seen = true;
                    }
                }
            }
        }
        have_thread = Thread32Next(snap, &te);
    }
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        /* An incomplete enumeration must not retire the unobserved handles. */
        g_s.thread_snapshot_failed++;
        CloseHandle(snap);
        return;
    }
    CloseHandle(snap);
    for (uint32_t t = 0; t < g_thr_count; t++) {
        if (g_thr[t].seen || !g_thr[t].h) continue;
        CloseHandle(g_thr[t].h);
        g_thr[t].h = NULL; /* name, lifetime and histogram index remain retained */
    }
}

static DWORD WINAPI samp_thread(LPVOID unused)
{
    (void)unused;
    /* A period, not a deadline: if a sample takes longer than the interval the
     * rate drops, which is visible in the report as a lower total, rather than
     * the sampler spinning and stealing the core it is measuring. */
    const DWORD period_ms = (DWORD)(1000 / (g_s.hz > 0 ? g_s.hz : 1000));
    const DWORD sleep_ms  = period_ms > 0 ? period_ms : 1;

    /* Skip the warm-up.  The first profile taken here put 3.9% in ZwReadFile
     * and 1.5% in zstd -- both real, both scene load and the 200k spawn,
     * neither present in the steady state anyone wants to optimise.  A profile
     * that silently averages startup into the frame loop answers a question
     * nobody asked. */
    {
        DWORD waited = 0;
        while (waited < (DWORD)g_s.delay_ms &&
               !InterlockedCompareExchange(&g_s.stop, 0, 0)) {
            Sleep(20); waited += 20;
        }
    }

    const DWORD self_tid = GetCurrentThreadId();
    uint32_t refresh_countdown = 0;

    while (!InterlockedCompareExchange(&g_s.stop, 0, 0)) {
        Sleep(sleep_ms);
        if (InterlockedCompareExchange(&g_s.stop, 0, 0)) break;

        if (g_s.all_threads) {
            if (refresh_countdown == 0) {
                samp_refresh_threads(self_tid);
                refresh_countdown = (uint32_t)(g_s.hz / 2);   /* ~2x a second */
                if (!refresh_countdown) refresh_countdown = 1;
            }
            refresh_countdown--;
            for (uint32_t t = 0; t < g_thr_count; t++) {
                if (InterlockedCompareExchange(&g_s.stop, 0, 0)) break;
                HANDLE h = g_thr[t].h;
                if (!h) continue; /* historical entry, no live thread to sample */
                if (SuspendThread(h) == (DWORD)-1) { g_s.missed++; continue; }
                g_thr[t].suspended = true;
                CONTEXT ctx;
                memset(&ctx, 0, sizeof ctx);
                ctx.ContextFlags = CONTEXT_CONTROL;
                uint64_t pc = 0;
                if (GetThreadContext(h, &ctx)) {
#if JCE_ARCH_X64
                    pc = (uint64_t)ctx.Rip;
#elif JCE_ARCH_X86
                    pc = (uint64_t)ctx.Eip;
#elif JCE_ARCH_ARM64
                    pc = (uint64_t)ctx.Pc;
#endif
                }
                if (ResumeThread(h) == (DWORD)-1) {
                    g_s.resume_failed++; g_s.missed++;
                    InterlockedExchange(&g_s.stop, 1);
                    break; /* reject this PC and retain the acquired suspend */
                }
                g_thr[t].suspended = false;
                if (pc) { g_s.total++; g_thr[t].samples++;
                         samp_record(pc, (uint16_t)t); }
                else    { g_s.missed++; }
            }
            continue;
        }

        if (SuspendThread(g_s.target) == (DWORD)-1) { g_s.missed++; continue; }
        g_s.target_suspended = true;
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL;
        uint64_t pc = 0;
        if (GetThreadContext(g_s.target, &ctx)) {
#if JCE_ARCH_X64
            pc = (uint64_t)ctx.Rip;
#elif JCE_ARCH_X86
            pc = (uint64_t)ctx.Eip;
#elif JCE_ARCH_ARM64
            pc = (uint64_t)ctx.Pc;
#endif
        }
        if (ResumeThread(g_s.target) == (DWORD)-1) {
            g_s.resume_failed++; g_s.missed++;
            InterlockedExchange(&g_s.stop, 1);
            break;
        }
        g_s.target_suspended = false;
        /* Record AFTER resuming: the hash insert must not extend the window the
         * profiled thread is stopped for. */
        if (pc) { g_s.total++; samp_record(pc, SAMP_TIDX_MAIN); }
        else    { g_s.missed++; }
    }
    /* Do NOT close the thread table here: it holds the per-thread sample
     * counts the report prints, and closing zeroes the count. The first
     * whole-process profile came back with an empty per-thread section for
     * exactly that reason. jce_sampler_shutdown closes it after reporting. */
    return 0;
}

void jce_sampler_init(void)
{
    if (g_s.worker) return;
    const char *hz_env = getenv("JCE_SAMPLER");
    if (!hz_env || !hz_env[0] || hz_env[0] == '0') return;
    memset(&g_s, 0, sizeof g_s);
    g_s.hz = atoi(hz_env);
    if (g_s.hz <= 0)   g_s.hz = 1000;
    if (g_s.hz > 8000) g_s.hz = 8000;

    const char *av = getenv("JCE_SAMPLER_ALL");
    g_s.all_threads = (av && av[0] && av[0] != '0') ? 1 : 0;

    const char *dv = getenv("JCE_SAMPLER_DELAY");
    g_s.delay_ms = (dv && dv[0]) ? atoi(dv) : 4000;
    if (g_s.delay_ms < 0) g_s.delay_ms = 0;

    const char *out = getenv("JCE_SAMPLER_OUT");
    if (out && out[0]) snprintf(g_s.out_path, sizeof g_s.out_path, "%s", out);

    g_s.slots = (SampSlot *)JCE_CALLOC(SAMP_CAP, sizeof(SampSlot));
    if (!g_s.slots) { LOG_WARN(LOG_TAG, "sampler: out of memory"); return; }

    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &g_s.target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | SYNCHRONIZE,
                         FALSE, 0)) {
        LOG_WARN(LOG_TAG, "sampler: cannot duplicate the target thread handle");
        JCE_FREE(g_s.slots); g_s.slots = NULL;
        return;
    }
    g_s.stop = 0;
    g_s.worker = CreateThread(NULL, 0, samp_thread, NULL, 0, NULL);
    if (!g_s.worker) {
        CloseHandle(g_s.target); g_s.target = NULL;
        JCE_FREE(g_s.slots); g_s.slots = NULL;
        LOG_WARN(LOG_TAG, "sampler: cannot start the sampling thread");
        return;
    }
    /* Above normal so a busy main thread cannot starve the sampler and quietly
     * bias the profile toward whatever runs while the machine is idle. */
    SetThreadPriority(g_s.worker, THREAD_PRIORITY_ABOVE_NORMAL);
    LOG_INFO(LOG_TAG, "JCE_SAMPLER=%d Hz, %d ms warm-up, %s%s%s",
             g_s.hz, g_s.delay_ms,
             g_s.all_threads ? "process sweep (128 thread lifetimes per session)" : "main thread only",
             g_s.out_path[0] ? " -> " : "", g_s.out_path);
}

typedef struct { uint64_t pc; uint32_t count; uint16_t tidx; } SampOut;

static int samp_cmp(const void *a, const void *b)
{
    const uint32_t ca = ((const SampOut *)a)->count;
    const uint32_t cb = ((const SampOut *)b)->count;
    return (cb > ca) - (cb < ca);
}

/* Aggregate by resolved symbol, since one function spans many PCs. */
typedef struct { char name[192]; uint32_t count; uint32_t line_lo, line_hi;
                 uint32_t orig; } SampFn;   /* orig: index before the sort */

/* Per-line rows for the hottest functions.  A function-level profile said
 * `sr_draw_entities [3109..5502]` -- 2400 lines, which names the file and
 * nothing else.  Folding a second histogram on (function, line) turns that into
 * the handful of lines the samples actually land on, at no extra sampling cost:
 * the line numbers were already being resolved and thrown into a min/max. */
typedef struct { uint32_t fn; uint32_t line; uint32_t count; } SampLine;

/* (thread, function) rows: the only form in which "which thread is blocked on
 * what" is answerable. */
typedef struct { uint16_t tidx; uint32_t fn; uint32_t count; } SampThrFn;
#define SAMP_THRFN_MAX 4096u

/* Basename of the module owning `pc`, or "?" -- used to qualify every row so
 * same-named functions from different modules do not merge. */
static const char *mod_short(HANDLE proc, uint64_t pc)
{
    static char buf[64];
    IMAGEHLP_MODULE64 mi;
    memset(&mi, 0, sizeof mi);
    mi.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);
    if (SymGetModuleInfo64(proc, (DWORD64)pc, &mi) && mi.ModuleName[0]) {
        snprintf(buf, sizeof buf, "%s", mi.ModuleName);
        return buf;
    }
    return "?";
}

/* True when this module has no private symbols -- only its export table.
 *
 * The Size that SymFromAddr reports for an EXPORT is not the function's
 * extent, so "disp < Size" cannot mean "inside". Trusting it produced a
 * profile with 17.5% in VCRUNTIME140!_NLG_Return2, a tiny SEH helper: the
 * samples were really in memcpy a few exports later. The earlier fix only
 * caught the Size == 0 case; an export table that supplies a size defeats it,
 * so ask the module instead. */
static bool mod_exports_only(HANDLE proc, uint64_t pc)
{
    IMAGEHLP_MODULE64 mi;
    memset(&mi, 0, sizeof mi);
    mi.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);
    if (!SymGetModuleInfo64(proc, (DWORD64)pc, &mi)) return true;
    return mi.SymType == SymExport || mi.SymType == SymNone ||
           mi.SymType == SymDeferred;
}

/* Only after a true join may the owner retry a suspend the sampler acquired.
 * Never resume on a timeout: the live worker may still own that operation. */
static bool samp_recover_suspends(void)
{
    bool recovered = true;
    if (g_s.target_suspended) {
        if (ResumeThread(g_s.target) == (DWORD)-1) {
            g_s.resume_failed++;
            /* Only a signaled retained thread handle proves termination.
             * GetThreadTimes exit time is undefined while a thread is live. */
            if (WaitForSingleObject(g_s.target, 0) == WAIT_OBJECT_0)
                g_s.target_suspended = false;
            else recovered = false;
        } else g_s.target_suspended = false;
    }
    for (uint32_t t = 0; t < g_thr_count; t++) {
        if (!g_thr[t].suspended) continue;
        if (ResumeThread(g_thr[t].h) == (DWORD)-1) {
            g_s.resume_failed++;
            if (WaitForSingleObject(g_thr[t].h, 0) == WAIT_OBJECT_0)
                g_thr[t].suspended = false;
            else recovered = false;
        } else g_thr[t].suspended = false;
    }
    return recovered;
}

bool jce_sampler_shutdown(void)
{
    if (!g_s.worker) return true;
    InterlockedExchange(&g_s.stop, 1);
    const DWORD joined = WaitForSingleObject(g_s.worker, 2000);
    if (joined != WAIT_OBJECT_0) {
        /* Native suspend/context requests have no local deadline. A timed-out
         * wait is not cancellation or proof that shared state is unused. */
        LOG_WARN(LOG_TAG, "sampler: worker join incomplete (wait=%lu); retaining state for shutdown retry",
                 (unsigned long)joined);
        return false;
    }
    if (!samp_recover_suspends()) {
        LOG_WARN(LOG_TAG, "sampler: owned suspend recovery incomplete; retaining state for shutdown retry");
        return false;
    }
    CloseHandle(g_s.worker); g_s.worker = NULL;
    if (g_s.target) { CloseHandle(g_s.target); g_s.target = NULL; }
    /* g_thr stays alive until after the report; closed at the end. */

    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    /* The crash handler already calls SymInitialize for this process, and a
     * second call returns FALSE. Treating that as "no symbols" is what turned
     * the first profile into a list of hex addresses -- so initialise, ignore
     * the result, and let SymFromAddr itself say whether it can resolve.
     * (An earlier version of this comment claimed SymCleanup was deliberately
     * skipped; the code below calls it when sym_fresh, and that is correct --
     * the crash handler initialises inside its exception filter, not at
     * startup, so in a clean run the sampler does own the session.)
     *
     * _NT_SYMBOL_PATH is honoured when set; without it dbghelp searches the
     * module directories only, which is enough for this engine's own PDBs and
     * is why system modules resolve to exports rather than private symbols. */
    const BOOL sym_fresh = SymInitialize(proc, NULL, TRUE);
    (void)jce_native_sym_refresh_module_list_internal(proc);
    const BOOL sym_ok = TRUE;

    /* Flatten, sort by count, then fold into per-function totals. */
    SampOut  *flat  = (SampOut *)JCE_CALLOC(SAMP_CAP, sizeof(SampOut));
    SampFn   *fns   = (SampFn *)JCE_CALLOC(4096, sizeof(SampFn));
    SampLine *lines = (SampLine *)JCE_CALLOC(SAMP_LINE_MAX, sizeof(SampLine));
    SampThrFn *tfn  = (SampThrFn *)JCE_CALLOC(SAMP_THRFN_MAX, sizeof(SampThrFn));
    uint32_t ntf = 0;
    uint32_t n = 0, nf = 0, nl = 0, sum = 0;
    if (flat && fns) {
        for (uint32_t i = 0; i < SAMP_CAP; i++)
            if (g_s.slots[i].pc) {
                flat[n].pc = g_s.slots[i].pc;
                flat[n].count = g_s.slots[i].count;
                flat[n].tidx = g_s.slots[i].tidx;
                sum += g_s.slots[i].count;
                n++;
            }
        qsort(flat, n, sizeof(SampOut), samp_cmp);

        char symbuf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)symbuf;
        for (uint32_t i = 0; i < n; i++) {
            char name[192];
            uint32_t line = 0;
            snprintf(name, sizeof name, "0x%llx",
                     (unsigned long long)flat[i].pc);
            if (sym_ok) {
                /* Name a PC only when it is actually INSIDE the symbol.
                 *
                 * SymFromAddr returns the nearest preceding symbol and reports
                 * how far past it the address sits. Discarding that
                 * displacement -- which the first version did -- silently files
                 * every PC in a symbol-less region under whatever export
                 * happens to precede it, and modules shipped without private
                 * symbols are mostly such regions. That is how a profile of
                 * this engine came to show 1.8% in `strchr` and 1.8% in
                 * `RtlCreateUnicodeString`: plausible-looking rows that are
                 * really "somewhere in ntdll after that export". A profiler
                 * that invents attributions is worse than none, because the
                 * numbers are actionable-looking.
                 *
                 * Inside the symbol (disp < Size, or Size unknown and disp
                 * small) keeps the name. Otherwise fall back to module+RVA,
                 * which is honest and still groups by module. */
                memset(sym, 0, sizeof(SYMBOL_INFO));
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen   = 255;
                DWORD64 disp = 0;
                bool named = false;
                const bool exports_only = mod_exports_only(proc, flat[i].pc);
                if (SymFromAddr(proc, (DWORD64)flat[i].pc, &disp, sym)) {
                    if (!exports_only && sym->Size && disp < sym->Size) {
                        /* Confirmed: the PC lies inside the symbol's extent. */
                        snprintf(name, sizeof name, "%s!%s",
                                 mod_short(proc, flat[i].pc), sym->Name);
                        named = true;
                    } else if (disp < 4096u) {
                        /* Export-only module (no private symbols): Size is 0, so
                         * "inside" cannot be checked and this is the nearest
                         * PRECEDING export, not necessarily the function running.
                         * Print the displacement so the row says so -- a reader
                         * seeing `ntdll!RtlCreateUnicodeString+0x2a0` knows to
                         * distrust the name, where a bare name invites acting on
                         * it. This engine nearly spent a day optimising two such
                         * rows. */
                        snprintf(name, sizeof name, "%s!%s+0x%llx",
                                 mod_short(proc, flat[i].pc), sym->Name,
                                 (unsigned long long)disp);
                        named = true;
                    }
                }
                if (!named) {
                    DWORD64 base = SymGetModuleBase64(proc, (DWORD64)flat[i].pc);
                    if (base)
                        snprintf(name, sizeof name, "%s+0x%llx",
                                 mod_short(proc, flat[i].pc),
                                 (unsigned long long)(flat[i].pc - base));
                }
                IMAGEHLP_LINE64 li = { sizeof(IMAGEHLP_LINE64), 0, 0, 0 };
                DWORD ldisp = 0;
                if (named &&
                    SymGetLineFromAddr64(proc, (DWORD64)flat[i].pc, &ldisp, &li))
                    line = (uint32_t)li.LineNumber;
            }
            /* Fold on the module-qualified name: ntdll!strchr and
             * VCRUNTIME140!strchr are different functions. */
            uint32_t k = 0;
            for (; k < nf; k++) if (strcmp(fns[k].name, name) == 0) break;
            if (k == nf && nf < 4096) {
                snprintf(fns[nf].name, sizeof fns[nf].name, "%s", name);
                fns[nf].line_lo = fns[nf].line_hi = line;
                fns[nf].orig = nf;
                nf++;
            }
            if (k < nf) {
                fns[k].count += flat[i].count;
                if (tfn) {
                    uint32_t q = 0;
                    for (; q < ntf; q++)
                        if (tfn[q].fn == k && tfn[q].tidx == flat[i].tidx) break;
                    if (q == ntf && ntf < SAMP_THRFN_MAX) {
                        tfn[ntf].fn = k; tfn[ntf].tidx = flat[i].tidx;
                        tfn[ntf].count = 0; ntf++;
                    }
                    if (q < ntf) tfn[q].count += flat[i].count;
                }
                if (line) {
                    if (!fns[k].line_lo || line < fns[k].line_lo) fns[k].line_lo = line;
                    if (line > fns[k].line_hi) fns[k].line_hi = line;
                    if (lines) {
                        uint32_t q = 0;
                        for (; q < nl; q++)
                            if (lines[q].fn == k && lines[q].line == line) break;
                        if (q == nl && nl < SAMP_LINE_MAX) {
                            lines[nl].fn = k; lines[nl].line = line;
                            lines[nl].count = 0; nl++;
                        }
                        if (q < nl) lines[q].count += flat[i].count;
                    }
                }
            }
        }
        /* Reuse the comparator: SampFn's count sits at a different offset, so
         * sort a small index array instead of casting. */
        for (uint32_t i = 0; i + 1 < nf; i++)
            for (uint32_t j = i + 1; j < nf; j++)
                if (fns[j].count > fns[i].count) {
                    SampFn t = fns[i]; fns[i] = fns[j]; fns[j] = t;
                }
        /* lines[].fn indexes the PRE-sort order; the sort moved the rows, so
         * build orig -> sorted and rewrite them. Reading fns[lines[q].fn] after
         * an in-place sort attributes every line to the wrong function. */
        if (lines || tfn) {
            uint32_t *remap = (uint32_t *)JCE_CALLOC(nf ? nf : 1u, sizeof(uint32_t));
            if (remap) {
                for (uint32_t i = 0; i < nf; i++)
                    if (fns[i].orig < nf) remap[fns[i].orig] = i;
                for (uint32_t q = 0; q < nl; q++)
                    if (lines[q].fn < nf) lines[q].fn = remap[lines[q].fn];
                for (uint32_t q = 0; q < ntf; q++)
                    if (tfn[q].fn < nf) tfn[q].fn = remap[tfn[q].fn];
                JCE_FREE(remap);
            } else {
                nl = ntf = 0; /* cannot remap: omit both indexed summaries */
            }
        }
    }

    /* Build the report in one buffer and write it once.
     *
     * The streaming version produced an EMPTY file, and it took two rebuilds to
     * see why: the open had been switched to SDL_IOStream while the writes were
     * still fprintf/fclose. MSVC in C mode only WARNS about the mismatched
     * pointer, so it built, ran, and silently wrote nothing. One buffer and one
     * jce_fs_host_write_all cannot fail that quietly -- and the byte count goes
     * in the log so a zero shows up as a zero. */
    char *rep = NULL;
    size_t rep_len = 0, rep_cap = 0;
    if (g_s.out_path[0]) {
        rep_cap = 512u * 1024u;
        rep = (char *)JCE_MALLOC(rep_cap);
        if (!rep) LOG_WARN(LOG_TAG, "sampler: out of memory for the report");
    }
    /* A silent truncation here is how the whole-process report came back
     * with an EMPTY per-thread section: the function table filled the buffer
     * and every later append vanished. Now it records the fact. */
    bool rep_full = false;
    #define SAMP_APPEND(...)                                               \
        do { if (rep && rep_cap > rep_len + 1u) {                          \
                 const int _n = snprintf(rep + rep_len, rep_cap - rep_len, \
                                         __VA_ARGS__);                     \
                 if (_n > 0) rep_len += (size_t)_n;                        \
                 if (rep_len >= rep_cap) { rep_len = rep_cap - 1u;         \
                                           rep_full = true; }              \
             } else if (rep) { rep_full = true; } } while (0)

    LOG_INFO(LOG_TAG,
             "sampler: %llu samples at requested %d Hz (%llu missed, %u distinct PCs, "
             "%u functions)%s",
             (unsigned long long)g_s.total, g_s.hz,
             (unsigned long long)g_s.missed, n, nf,
             sum != g_s.total ? "  [HISTOGRAM FULL -- samples dropped]" : "");
    SAMP_APPEND("# jce sampler: %llu samples at requested %d Hz, %llu missed, "
                "%d ms warm-up\n# percent  samples  function  [lines]\n",
                (unsigned long long)g_s.total, g_s.hz,
                (unsigned long long)g_s.missed, g_s.delay_ms);
    if (g_s.all_threads) {
        LOG_INFO(LOG_TAG, "sampler: thread lifetime cap %u, %u admitted, %llu capacity-skipped refresh observations, "
                 "%llu open failures, %llu identity failures, %llu snapshot failures",
                 SAMP_MAX_THREADS, g_thr_count,
                 (unsigned long long)g_s.thread_cap_skipped,
                 (unsigned long long)g_s.thread_open_failed,
                 (unsigned long long)g_s.thread_identity_failed,
                 (unsigned long long)g_s.thread_snapshot_failed);
        SAMP_APPEND("# thread lifetime cap %u, %u admitted, %llu capacity-skipped refresh observations, "
                    "%llu open failures, %llu identity failures, %llu snapshot failures\n",
                    SAMP_MAX_THREADS, g_thr_count,
                    (unsigned long long)g_s.thread_cap_skipped,
                    (unsigned long long)g_s.thread_open_failed,
                    (unsigned long long)g_s.thread_identity_failed,
                    (unsigned long long)g_s.thread_snapshot_failed);
    }
    if (g_s.resume_failed) LOG_WARN(LOG_TAG, "sampler: %llu resume failures; failed-resume PCs were discarded",
                                   (unsigned long long)g_s.resume_failed);
    SAMP_APPEND("# resume failures %llu; only PCs resumed successfully are accepted\n",
                (unsigned long long)g_s.resume_failed);

    if (g_s.all_threads && g_thr_count && tfn) {
        /* Per thread, where its samples actually landed. The first version of
         * this section printed only a sample COUNT per thread and every thread
         * read an identical 1.564% -- one sample per thread per tick, which
         * says nothing. What matters is the function. */
        SAMP_APPEND("%s", "\n# per-thread hotspots (whole-process sweep)\n");
        for (uint32_t t = 0; t < g_thr_count; t++) {
            if (!g_thr[t].samples) continue;
            SAMP_APPEND("# %s [tid=%lu created=0x%llx%s]  (%llu samples)\n", g_thr[t].name,
                        (unsigned long)g_thr[t].tid, (unsigned long long)g_thr[t].created,
                        g_thr[t].h ? "" : " retired",
                        (unsigned long long)g_thr[t].samples);
            for (uint32_t shown = 0; shown < 4u; shown++) {
                uint32_t best = SAMP_THRFN_MAX, bestc = 0;
                for (uint32_t q = 0; q < ntf; q++) {
                    if (tfn[q].tidx != (uint16_t)t) continue;
                    if (tfn[q].count <= bestc) continue;
                    if (tfn[q].fn >= nf) continue;
                    best = q; bestc = tfn[q].count;
                }
                if (best == SAMP_THRFN_MAX) break;
                SAMP_APPEND("%7.2f%% of thread   %s\n",
                            100.0 * (double)bestc / (double)g_thr[t].samples,
                            fns[tfn[best].fn].name);
                tfn[best].count = 0;
            }
        }
    }

    const uint32_t shown = nf < 40u ? nf : 40u;
    for (uint32_t i = 0; i < shown && sum; i++) {
        const double pct = 100.0 * (double)fns[i].count / (double)sum;
        if (pct < 0.20) break;
        if (fns[i].line_lo)
            LOG_INFO(LOG_TAG, "  %6.2f%%  %7u  %s  [%u..%u]", pct,
                     fns[i].count, fns[i].name, fns[i].line_lo, fns[i].line_hi);
        else
            LOG_INFO(LOG_TAG, "  %6.2f%%  %7u  %s", pct, fns[i].count, fns[i].name);
    }
    for (uint32_t i = 0; i < nf && sum; i++)
        SAMP_APPEND("%7.3f %8u %s %u %u\n",
                    100.0 * (double)fns[i].count / (double)sum,
                    fns[i].count, fns[i].name, fns[i].line_lo, fns[i].line_hi);


    /* Per-line breakdown of the hottest functions. The fold above ran before
     * fns[] was sorted, so lines[].fn still indexes the ORIGINAL order --
     * remap through the sort by name rather than by index. */
    if (lines && nl && sum) {
        SAMP_APPEND("\n# per-line, top functions\n");
        for (uint32_t f = 0; f < nf && f < 4u; f++) {
            if (100.0 * (double)fns[f].count / (double)sum < 2.0) break;
            SAMP_APPEND("# %s (%.2f%%)\n", fns[f].name,
                        100.0 * (double)fns[f].count / (double)sum);
            /* Selection sort over this function's rows: nl is small and this
             * runs once at shutdown. */
            for (uint32_t shown = 0; shown < 14u; shown++) {
                uint32_t best = SAMP_LINE_MAX, bestc = 0;
                for (uint32_t q = 0; q < nl; q++) {
                    if (lines[q].count <= bestc) continue;
                    if (lines[q].fn != f) continue;
                    best = q; bestc = lines[q].count;
                }
                if (best == SAMP_LINE_MAX) break;
                SAMP_APPEND("%7.3f %8u   line %u\n",
                            100.0 * (double)bestc / (double)sum, bestc,
                            lines[best].line);
                lines[best].count = 0;   /* consumed */
            }
        }
    }
    if (rep) {
        if (rep_full)
            LOG_WARN(LOG_TAG, "sampler: report TRUNCATED at %u bytes -- rows "
                     "were dropped", (unsigned)rep_cap);
        if (!jce_fs_host_write_all(g_s.out_path, rep, (uint64_t)rep_len))
            LOG_WARN(LOG_TAG, "sampler: cannot write %s", g_s.out_path);
        else
            LOG_INFO(LOG_TAG, "sampler: wrote %u bytes to %s",
                     (unsigned)rep_len, g_s.out_path);
        JCE_FREE(rep);
    }
    #undef SAMP_APPEND

    samp_close_threads();
    if (sym_fresh) SymCleanup(proc);   /* only if WE opened it */
    JCE_FREE(flat); JCE_FREE(fns); JCE_FREE(lines); JCE_FREE(tfn);
    JCE_FREE(g_s.slots); g_s.slots = NULL;
    memset(&g_s, 0, sizeof g_s);
    return true;
}

#else /* !JCE_PLATFORM_WINDOWS */

void jce_sampler_init(void) { }
bool jce_sampler_shutdown(void) { return true; }

#endif
