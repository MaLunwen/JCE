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
        FILETIME creation, exit, kernel, user;
        GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
        ULARGE_INTEGER k, u;
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime;   u.HighPart = user.dwHighDateTime;
        info->_prev_kernel = k.QuadPart;
        info->_prev_user   = u.QuadPart;

        FILETIME now_ft;
        GetSystemTimeAsFileTime(&now_ft);
        ULARGE_INTEGER now_ul;
        now_ul.LowPart = now_ft.dwLowDateTime;
        now_ul.HighPart = now_ft.dwHighDateTime;
        info->_prev_time = now_ul.QuadPart;
    }
#elif defined(SDL_PLATFORM_LINUX)
    {
        SDL_IOStream *io = SDL_IOFromFile("/proc/self/stat", "r");
        if (io) {
            unsigned long utime = 0, stime = 0;
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
                                   &utime, &stime);
                    (void)n;
                }
            }
            info->_prev_kernel = stime;
            info->_prev_user   = utime;
        }
        /* Use SDL high-resolution counter instead of clock_gettime. */
        uint64_t freq = jce_time_perf_freq();
        uint64_t ctr  = jce_time_perf_counter();
        info->_prev_time = (freq > 0) ? ctr * 1000000000ULL / freq : 0;
    }
#endif
}

/* -- Dynamic info -------------------------------------------------- */

// cppcheck-suppress constParameterPointer   ; info is written on _WIN32 / __linux__ / __APPLE__
void jce_sysinfo_update(JceSysInfo *info)
{
    /* -- Process RAM ---------------------------------------------- */

#ifdef __EMSCRIPTEN__
    /* No-op: browser sandbox provides no process-level metrics. */
    (void)info;
#elif defined(SDL_PLATFORM_WINDOWS)
    {
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
            info->ram_used_mb = (int)(pmc.WorkingSetSize / (1024 * 1024));
    }
#elif defined(SDL_PLATFORM_LINUX)
    {
        SDL_IOStream *io = SDL_IOFromFile("/proc/self/status", "r");
        if (io) {
            char status_buf[4096];
            size_t nread = SDL_ReadIO(io, status_buf, sizeof(status_buf) - 1);
            SDL_CloseIO(io);
            if (nread > 0) {
                status_buf[nread] = '\0';
                const char *vmrss = strstr(status_buf, "VmRSS:");
                if (vmrss) {
                    long kb = 0;
                    sscanf(vmrss + 6, " %ld", &kb);
                    info->ram_used_mb = (int)(kb / 1024);
                }
            }
        }
    }
#elif defined(SDL_PLATFORM_MACOS)
    {
        mach_task_basic_info_data_t task_info_data;
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                      (task_info_t)&task_info_data, &count) == KERN_SUCCESS) {
            info->ram_used_mb = (int)(task_info_data.resident_size / (1024 * 1024));
        }
    }
#endif

    /* -- CPU usage ------------------------------------------------ */

#if defined(__EMSCRIPTEN__)
    /* No CPU usage on Emscripten — handled above. */
#elif defined(SDL_PLATFORM_WINDOWS)
    {
        FILETIME creation, exit, kernel, user;
        GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
        ULARGE_INTEGER k, u;
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime;   u.HighPart = user.dwHighDateTime;

        FILETIME now_ft;
        GetSystemTimeAsFileTime(&now_ft);
        ULARGE_INTEGER now_ul;
        now_ul.LowPart = now_ft.dwLowDateTime;
        now_ul.HighPart = now_ft.dwHighDateTime;

        uint64_t dt_wall   = now_ul.QuadPart - info->_prev_time;
        uint64_t dt_kernel = k.QuadPart - info->_prev_kernel;
        uint64_t dt_user   = u.QuadPart - info->_prev_user;

        if (dt_wall > 0) {
            info->cpu_usage = (float)(dt_kernel + dt_user) / (float)dt_wall * 100.0f;
        }

        info->_prev_time   = now_ul.QuadPart;
        info->_prev_kernel = k.QuadPart;
        info->_prev_user   = u.QuadPart;
    }
#elif defined(SDL_PLATFORM_LINUX)
    {
        unsigned long utime = 0, stime = 0;
        SDL_IOStream *io = SDL_IOFromFile("/proc/self/stat", "r");
        if (io) {
            char buf[1024];
            size_t nread = SDL_ReadIO(io, buf, sizeof(buf) - 1);
            SDL_CloseIO(io);
            if (nread > 0) {
                buf[nread] = '\0';
                char *p = strrchr(buf, ')');
                if (p) {
                    p += 2;
                    char state;
                    unsigned long ppid, pgrp, session, tty, tpgid, flags;
                    unsigned long minflt, cminflt, majflt, cmajflt;
                    sscanf(p, "%c %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                           &state, &ppid, &pgrp, &session, &tty, &tpgid,
                           &flags, &minflt, &cminflt, &majflt, &cmajflt,
                           &utime, &stime);
                }
            }
        }

        /* Use SDL high-resolution counter instead of clock_gettime. */
        uint64_t freq = jce_time_perf_freq();
        uint64_t ctr  = jce_time_perf_counter();
        uint64_t now_ns = (freq > 0) ? ctr * 1000000000ULL / freq : 0;
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
