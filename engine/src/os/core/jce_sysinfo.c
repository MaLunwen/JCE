/*
 * jce_sysinfo.c  Cross-platform system information.
 *
 * Uses SDL3 for static info, platform APIs for dynamic info.
 */

#include <jce/os/core/jce_sysinfo.h>
#include <jce/os/core/jce_timer.h>

#include <SDL3/SDL.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
/* Emscripten: no meaningful CPU/RAM measurement available from the
   browser sandbox.  All dynamic values remain at their init defaults (0). */
#elif defined(SDL_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <psapi.h>
#elif defined(SDL_PLATFORM_LINUX)
#include <stdio.h>
#include <unistd.h>
#elif defined(SDL_PLATFORM_MACOS)
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_info.h>
#endif

/* -- Platform counter readers -------------------------------------- *
 *
 * jce_sysinfo_init seeds the CPU baseline that jce_sysinfo_update then
 * differentiates, so both used to carry their own copy of the same
 * per-platform counter read.  The readers live here once; the #if chain
 * mirrors the include chain above so each one is only compiled for the
 * platform whose API it uses. */

#ifdef __EMSCRIPTEN__
/* Browser sandbox exposes no process-level counters — nothing to read. */
#elif defined(SDL_PLATFORM_WINDOWS)

/* Process kernel/user CPU time plus the matching wall clock, all in 100 ns
   FILETIME ticks so the three can be differenced against each other. */
static void win_process_times(uint64_t *out_kernel, uint64_t *out_user,
                              uint64_t *out_now)
{
    FILETIME creation, exit, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;   u.HighPart = user.dwHighDateTime;

    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    ULARGE_INTEGER now_ul;
    now_ul.LowPart  = now_ft.dwLowDateTime;
    now_ul.HighPart = now_ft.dwHighDateTime;

    *out_kernel = k.QuadPart;
    *out_user   = u.QuadPart;
    *out_now    = now_ul.QuadPart;
}

#elif defined(SDL_PLATFORM_LINUX)

/* utime/stime (fields 14/15 of /proc/self/stat, in clock ticks).  Both
   outputs are zeroed up front, so a short or garbled read leaves them at 0
   exactly as the open-coded copies did.  Returns false only when the file
   could not be opened — the callers key off that. */
static bool linux_read_proc_times(unsigned long *out_utime,
                                  unsigned long *out_stime)
{
    *out_utime = 0;
    *out_stime = 0;
    SDL_IOStream *io = SDL_IOFromFile("/proc/self/stat", "r");
    if (!io) return false;
    char buf[1024];
    size_t nread = SDL_ReadIO(io, buf, sizeof(buf) - 1);
    SDL_CloseIO(io);
    if (nread > 0) {
        buf[nread] = '\0';
        char *p = strrchr(buf, ')');
        if (p) {
            p += 2; /* skip ") " */
            char state;
            unsigned long ppid, pgrp, session, tty, tpgid, flags;
            unsigned long minflt, cminflt, majflt, cmajflt;
            int n = sscanf(p, "%c %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                           &state, &ppid, &pgrp, &session, &tty, &tpgid,
                           &flags, &minflt, &cminflt, &majflt, &cmajflt,
                           out_utime, out_stime);
            (void)n;
        }
    }
    return true;
}

/* Monotonic nanoseconds from the SDL high-resolution counter (this file
   deliberately avoids clock_gettime). */
static uint64_t linux_now_ns(void)
{
    uint64_t freq = jce_time_perf_freq();
    uint64_t ctr  = jce_time_perf_counter();
    return (freq > 0) ? ctr * 1000000000ULL / freq : 0;
}

#endif

/* -- Static info --------------------------------------------------- */

void jce_sysinfo_init(JceSysInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->cpu_cores    = SDL_GetNumLogicalCPUCores();
    info->ram_total_mb = SDL_GetSystemRAM();

    /* Initialize CPU usage baseline. */
#ifdef __EMSCRIPTEN__
    /* No-op: browser sandbox provides no process-level metrics. */
#elif defined(SDL_PLATFORM_WINDOWS)
    {
        uint64_t kernel = 0, user = 0, now = 0;
        win_process_times(&kernel, &user, &now);
        info->_prev_kernel = kernel;
        info->_prev_user   = user;
        info->_prev_time   = now;
    }
#elif defined(SDL_PLATFORM_LINUX)
    {
        unsigned long utime = 0, stime = 0;
        if (linux_read_proc_times(&utime, &stime)) {
            info->_prev_kernel = stime;
            info->_prev_user   = utime;
        }
        /* Use SDL high-resolution counter instead of clock_gettime. */
        info->_prev_time = linux_now_ns();
    }
#endif
}

/* -- Dynamic info -------------------------------------------------- */

void jce_sysinfo_update(JceSysInfo *info)
{
    /* -- Process RAM ---------------------------------------------- */

    /* The working-set read is the same counter jce_sysinfo_process_mem
       already reads on every platform (Win32 WorkingSetSize, /proc/self/status
       VmRSS, mach resident_size), so go through that single implementation
       instead of keeping a second per-platform copy here.  Emscripten reports
       nothing and leaves ram_used_mb at its init value. */
    {
        uint64_t working_set = 0;
        if (jce_sysinfo_process_mem(&working_set, NULL, NULL))
            info->ram_used_mb = (int)(working_set / (1024 * 1024));
    }

    /* -- CPU usage ------------------------------------------------ */

#if defined(__EMSCRIPTEN__)
    /* No CPU usage on Emscripten — handled above. */
#elif defined(SDL_PLATFORM_WINDOWS)
    {
        uint64_t kernel = 0, user = 0, now = 0;
        win_process_times(&kernel, &user, &now);

        uint64_t dt_wall   = now - info->_prev_time;
        uint64_t dt_kernel = kernel - info->_prev_kernel;
        uint64_t dt_user   = user - info->_prev_user;

        if (dt_wall > 0) {
            info->cpu_usage = (float)(dt_kernel + dt_user) / (float)dt_wall * 100.0f;
        }

        info->_prev_time   = now;
        info->_prev_kernel = kernel;
        info->_prev_user   = user;
    }
#elif defined(SDL_PLATFORM_LINUX)
    {
        unsigned long utime = 0, stime = 0;
        (void)linux_read_proc_times(&utime, &stime);

        /* Use SDL high-resolution counter instead of clock_gettime. */
        uint64_t now_ns = linux_now_ns();
        uint64_t dt_ns = now_ns - info->_prev_time;

        long ticks_per_sec = sysconf(_SC_CLK_TCK);
        uint64_t dt_ticks = (utime + stime) - (info->_prev_kernel + info->_prev_user);

        if (dt_ns > 0 && ticks_per_sec > 0) {
            double dt_sec = (double)dt_ns / 1e9;
            double cpu_sec = (double)dt_ticks / (double)ticks_per_sec;
            info->cpu_usage = (float)(cpu_sec / dt_sec * 100.0);
        }

        info->_prev_time   = now_ns;
        info->_prev_kernel = stime;
        info->_prev_user   = utime;
    }
#elif defined(SDL_PLATFORM_MACOS)
    {
        /* Sum CPU time across all threads in the process. */
        thread_act_array_t threads;
        mach_msg_type_number_t thread_count;
        if (task_threads(mach_task_self(), &threads, &thread_count) == KERN_SUCCESS) {
            double total_cpu = 0.0;
            for (mach_msg_type_number_t i = 0; i < thread_count; i++) {
                thread_basic_info_data_t tbi;
                mach_msg_type_number_t tbi_count = THREAD_BASIC_INFO_COUNT;
                if (thread_info(threads[i], THREAD_BASIC_INFO,
                                (thread_info_t)&tbi, &tbi_count) == KERN_SUCCESS) {
                    if (!(tbi.flags & TH_FLAGS_IDLE)) {
                        total_cpu += (double)tbi.cpu_usage / (double)TH_USAGE_SCALE * 100.0;
                    }
                }
                mach_port_deallocate(mach_task_self(), threads[i]);
            }
            vm_deallocate(mach_task_self(), (vm_address_t)threads,
                          thread_count * sizeof(thread_act_t));
            info->cpu_usage = (float)total_cpu;
        }
    }
#endif
}

bool jce_sysinfo_process_mem(uint64_t *out_working_set,
                             uint64_t *out_private_bytes,
                             uint64_t *out_peak_working_set)
{
    bool ok = false;
#ifdef __EMSCRIPTEN__
    (void)out_working_set; (void)out_private_bytes; (void)out_peak_working_set;
#elif defined(SDL_PLATFORM_WINDOWS)
    {
        PROCESS_MEMORY_COUNTERS_EX pmc;
        memset(&pmc, 0, sizeof(pmc));
        if (GetProcessMemoryInfo(GetCurrentProcess(),
                                 (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) {
            if (out_working_set)      *out_working_set      = (uint64_t)pmc.WorkingSetSize;
            if (out_peak_working_set) *out_peak_working_set = (uint64_t)pmc.PeakWorkingSetSize;
            if (out_private_bytes)    *out_private_bytes    = (uint64_t)pmc.PrivateUsage;
            ok = true;
        }
    }
#elif defined(SDL_PLATFORM_LINUX)
    {
        SDL_IOStream *io = SDL_IOFromFile("/proc/self/status", "r");
        if (io) {
            char buf[8192];
            size_t nread = SDL_ReadIO(io, buf, sizeof(buf) - 1);
            SDL_CloseIO(io);
            if (nread > 0) {
                buf[nread] = '\0';
                long kb = 0;
                const char *p;
                if (out_working_set && (p = strstr(buf, "VmRSS:"))) {
                    if (sscanf(p + 6, " %ld", &kb) == 1) {
                        *out_working_set = (uint64_t)kb * 1024ULL; ok = true;
                    }
                }
                if (out_peak_working_set && (p = strstr(buf, "VmHWM:"))) {
                    if (sscanf(p + 6, " %ld", &kb) == 1) {
                        *out_peak_working_set = (uint64_t)kb * 1024ULL; ok = true;
                    }
                }
                if (out_private_bytes && (p = strstr(buf, "VmData:"))) {
                    if (sscanf(p + 7, " %ld", &kb) == 1) {
                        *out_private_bytes = (uint64_t)kb * 1024ULL; ok = true;
                    }
                }
            }
        }
    }
#elif defined(SDL_PLATFORM_MACOS)
    {
        /* macOS exposes resident_size via mach_task_basic_info; peak and
           private bytes aren't directly available without sampling VM
           regions, so only working_set is provided here. */
        mach_task_basic_info_data_t ti;
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                      (task_info_t)&ti, &count) == KERN_SUCCESS) {
            if (out_working_set) { *out_working_set = (uint64_t)ti.resident_size; ok = true; }
        }
    }
#endif
    return ok;
}
