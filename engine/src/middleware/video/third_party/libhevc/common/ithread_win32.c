/*
 * ithread_win32.c  Win32 threading abstraction for AOSP libhevc.
 *
 * Replaces ithread.c (which uses pthreads) on Windows/MSVC.
 * Implements the same ithread.h interface using Win32 primitives.
 *
 * Copyright 2024 JCE Contributors — Apache-2.0
 */

#ifdef _WIN32

#include <string.h>
#include "ihevc_typedefs.h"
#include "ithread.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>

/* ── Thread handle ─────────────────────────────────────────────── */

typedef struct {
    HANDLE handle;
    void *(*start_routine)(void *);
    void *arg;
} ithread_win32_t;

static unsigned __stdcall ithread_entry(void *param)
{
    ithread_win32_t *t = (ithread_win32_t *)param;
    t->start_routine(t->arg);
    return 0;
}

UWORD32 ithread_get_handle_size(void)
{
    return (UWORD32)sizeof(ithread_win32_t);
}

WORD32 ithread_create(void *thread_handle, void *attribute,
                      void *strt, void *argument)
{
    ithread_win32_t *t = (ithread_win32_t *)thread_handle;
    (void)attribute;

    t->start_routine = (void *(*)(void *))strt;
    t->arg = argument;
    t->handle = (HANDLE)_beginthreadex(NULL, 0, ithread_entry, t, 0, NULL);
    return (t->handle == NULL) ? -1 : 0;
}

void ithread_exit(void *val_ptr)
{
    (void)val_ptr;
    _endthreadex(0);
}

WORD32 ithread_join(void *thread_id, void **val_ptr)
{
    ithread_win32_t *t = (ithread_win32_t *)thread_id;
    (void)val_ptr;
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    t->handle = NULL;
    return 0;
}

/* ── Mutex (CRITICAL_SECTION) ──────────────────────────────────── */

UWORD32 ithread_get_mutex_lock_size(void)
{
    return (UWORD32)sizeof(CRITICAL_SECTION);
}

WORD32 ithread_get_mutex_struct_size(void)
{
    return (WORD32)sizeof(CRITICAL_SECTION);
}

WORD32 ithread_mutex_init(void *mutex)
{
    InitializeCriticalSection((CRITICAL_SECTION *)mutex);
    return 0;
}

WORD32 ithread_mutex_destroy(void *mutex)
{
    DeleteCriticalSection((CRITICAL_SECTION *)mutex);
    return 0;
}

WORD32 ithread_mutex_lock(void *mutex)
{
    EnterCriticalSection((CRITICAL_SECTION *)mutex);
    return 0;
}

WORD32 ithread_mutex_unlock(void *mutex)
{
    LeaveCriticalSection((CRITICAL_SECTION *)mutex);
    return 0;
}

/* ── Yield / Sleep ─────────────────────────────────────────────── */

void ithread_yield(void)
{
    SwitchToThread();
}

void ithread_sleep(UWORD32 u4_time)
{
    Sleep(u4_time * 1000);
}

void ithread_msleep(UWORD32 u4_time_ms)
{
    Sleep(u4_time_ms);
}

void ithread_usleep(UWORD32 u4_time_us)
{
    /* Win32 Sleep has millisecond resolution; round up. */
    DWORD ms = (u4_time_us + 999) / 1000;
    if (ms == 0) ms = 1;
    Sleep(ms);
}

/* ── Semaphore ─────────────────────────────────────────────────── */

UWORD32 ithread_get_sem_struct_size(void)
{
    return (UWORD32)sizeof(HANDLE);
}

WORD32 ithread_sem_init(void *sem, WORD32 pshared, UWORD32 value)
{
    HANDLE *h = (HANDLE *)sem;
    (void)pshared;
    *h = CreateSemaphoreA(NULL, (LONG)value, 0x7FFFFFFF, NULL);
    return (*h == NULL) ? -1 : 0;
}

WORD32 ithread_sem_post(void *sem)
{
    HANDLE *h = (HANDLE *)sem;
    return ReleaseSemaphore(*h, 1, NULL) ? 0 : -1;
}

WORD32 ithread_sem_wait(void *sem)
{
    HANDLE *h = (HANDLE *)sem;
    return (WaitForSingleObject(*h, INFINITE) == WAIT_OBJECT_0) ? 0 : -1;
}

WORD32 ithread_sem_destroy(void *sem)
{
    HANDLE *h = (HANDLE *)sem;
    CloseHandle(*h);
    *h = NULL;
    return 0;
}

/* ── Affinity ──────────────────────────────────────────────────── */

WORD32 ithread_set_affinity(WORD32 core_id)
{
    DWORD_PTR mask = (DWORD_PTR)1 << core_id;
    return SetThreadAffinityMask(GetCurrentThread(), mask) ? 0 : -1;
}

/* ── Condition variable ────────────────────────────────────────── */

WORD32 ithread_get_cond_struct_size(void)
{
    return (WORD32)sizeof(CONDITION_VARIABLE) + (WORD32)sizeof(CRITICAL_SECTION);
}

/* Layout: [CONDITION_VARIABLE][CRITICAL_SECTION_for_pairing] */
WORD32 ithread_cond_init(void *cond)
{
    InitializeConditionVariable((CONDITION_VARIABLE *)cond);
    return 0;
}

WORD32 ithread_cond_destroy(void *cond)
{
    (void)cond; /* Win32 condition variables don't need explicit destruction. */
    return 0;
}

WORD32 ithread_cond_wait(void *cond, void *mutex)
{
    SleepConditionVariableCS(
        (CONDITION_VARIABLE *)cond,
        (CRITICAL_SECTION *)mutex,
        INFINITE);
    return 0;
}

WORD32 ithread_cond_signal(void *cond)
{
    WakeConditionVariable((CONDITION_VARIABLE *)cond);
    return 0;
}

#endif /* _WIN32 */
