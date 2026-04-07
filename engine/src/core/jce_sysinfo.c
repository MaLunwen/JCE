/*
 * jce_sysinfo.c  Cross-platform system information.
 *
 * Uses SDL3 for static info, platform APIs for dynamic info.
 */

/* Ensure POSIX / GNU extensions (clock_gettime, struct timespec,
   CLOCK_MONOTONIC, sysconf, etc.) are visible under -std=c99. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

#include <jce/core/jce_sysinfo.h>

#include <SDL3/SDL.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
/* Emscripten: no meaningful CPU/RAM measurement available from the
   browser sandbox.  All dynamic values remain at their init defaults (0). */
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#elif defined(__APPLE__)
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
#elif defined(_WIN32)
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
#elif defined(__linux__)
    {
        FILE *fp = fopen("/proc/self/stat", "r");
        if (fp) {
            unsigned long utime = 0, stime = 0;
            /* Skip first 13 fields, read utime (14) and stime (15). */
            char buf[1024];
            if (fgets(buf, sizeof(buf), fp)) {
                /* Find the closing ')' of the comm field. */
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
            fclose(fp);
            info->_prev_kernel = stime;
            info->_prev_user   = utime;
        }
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        info->_prev_time = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
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
#elif defined(_WIN32)
    {
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
            info->ram_used_mb = (int)(pmc.WorkingSetSize / (1024 * 1024));
    }
#elif defined(__linux__)
    {
        FILE *fp = fopen("/proc/self/status", "r");
        if (fp) {
            char line[256];
            while (fgets(line, sizeof(line), fp)) {
                if (strncmp(line, "VmRSS:", 6) == 0) {
                    long kb = 0;
                    sscanf(line + 6, " %ld", &kb);
                    info->ram_used_mb = (int)(kb / 1024);
                    break;
                }
            }
            fclose(fp);
        }
    }
#elif defined(__APPLE__)
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
    /* No CPU usage on Emscripten  handled above. */
#elif defined(_WIN32)
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
#elif defined(__linux__)
    {
        unsigned long utime = 0, stime = 0;
        FILE *fp = fopen("/proc/self/stat", "r");
        if (fp) {
            char buf[1024];
            if (fgets(buf, sizeof(buf), fp)) {
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
            fclose(fp);
        }

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
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
#elif defined(__APPLE__)
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
