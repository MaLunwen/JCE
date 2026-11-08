/* Authored deterministic declarations for the sampler's private native port.
 * This is not a Windows SDK copy and never enters a consumer or real build. */
#ifndef JCE_SAMPLER_TEST_PORT_H
#define JCE_SAMPLER_TEST_PORT_H

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

typedef uint32_t DWORD;
typedef uint64_t DWORD64;
typedef int32_t LONG;
typedef LONG HRESULT;
typedef void *HMODULE;
typedef int BOOL;
typedef void *LPVOID;
typedef wchar_t *PWSTR;
typedef struct SamplerPortHandle *HANDLE;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct { DWORD ContextFlags; uint64_t Rip, Eip, Pc; } CONTEXT;
typedef struct {
    DWORD dwSize, th32OwnerProcessID, th32ThreadID;
} THREADENTRY32;
typedef struct {
    DWORD SizeOfStruct;
    char ModuleName[64];
    int SymType;
} IMAGEHLP_MODULE64;
typedef struct {
    DWORD SizeOfStruct, MaxNameLen, Size;
    char Name[1];
} SYMBOL_INFO;
typedef struct {
    DWORD SizeOfStruct;
    void *Key;
    DWORD LineNumber;
    char *FileName;
    DWORD64 Address;
} IMAGEHLP_LINE64;

#define WINAPI
typedef intptr_t (WINAPI *FARPROC)(void);
#define E_POINTER ((HRESULT)0x80004003L)
#define E_NOTIMPL ((HRESULT)0x80004001L)
#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED UINT32_MAX
#define ERROR_NO_MORE_FILES 18u
#define ERROR_CALL_NOT_IMPLEMENTED 120u
#define TH32CS_SNAPTHREAD 4u
#define THREAD_SUSPEND_RESUME 2u
#define THREAD_GET_CONTEXT 8u
#define THREAD_QUERY_LIMITED_INFORMATION 0x800u
#define SYNCHRONIZE 0x00100000u
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define CONTEXT_CONTROL 1u
#define CP_UTF8 65001u
#define SYMOPT_DEFERRED_LOADS 1u
#define SYMOPT_UNDNAME 2u
#define SYMOPT_LOAD_LINES 4u
#define SymExport 1
#define SymNone 2
#define SymDeferred 3
#define SUCCEEDED(value) ((value) >= 0)

static HANDLE GetCurrentProcess(void);
static HANDLE GetCurrentThread(void);
static DWORD GetCurrentProcessId(void);
static DWORD GetCurrentThreadId(void);
static BOOL DuplicateHandle(HANDLE from, HANDLE thread, HANDLE to, HANDLE *out,
                            DWORD access, BOOL inherit, DWORD options);
static HANDLE CreateThread(void *security, size_t stack, DWORD (*entry)(LPVOID),
                           LPVOID arg, DWORD flags, DWORD *tid);
static BOOL SetThreadPriority(HANDLE handle, int priority);
static BOOL CloseHandle(HANDLE handle);
static DWORD WaitForSingleObject(HANDLE handle, DWORD timeout_ms);
static LONG InterlockedCompareExchange(volatile LONG *value, LONG desired,
                                      LONG expected);
static LONG InterlockedExchange(volatile LONG *value, LONG desired);
static void Sleep(DWORD ms);
static HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid);
static BOOL Thread32First(HANDLE snapshot, THREADENTRY32 *entry);
static BOOL Thread32Next(HANDLE snapshot, THREADENTRY32 *entry);
static HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid);
static BOOL GetThreadTimes(HANDLE handle, FILETIME *created, FILETIME *exited,
                           FILETIME *kernel, FILETIME *user);
static DWORD GetLastError(void);
static void WINAPI SetLastError(DWORD error);
static HMODULE GetModuleHandleW(const wchar_t *name);
static FARPROC GetProcAddress(HMODULE module, const char *name);
static HRESULT WINAPI sampler_port_thread_description(HANDLE handle, PWSTR *description);
static int WideCharToMultiByte(unsigned codepage, DWORD flags,
                              const wchar_t *wide, int count, char *text,
                              int capacity, const char *fallback, BOOL *used);
static void *LocalFree(void *memory);
static DWORD SuspendThread(HANDLE handle);
static BOOL GetThreadContext(HANDLE handle, CONTEXT *context);
static DWORD ResumeThread(HANDLE handle);
static DWORD SymSetOptions(DWORD options);
static BOOL SymInitialize(HANDLE process, const char *path, BOOL invade);
static BOOL WINAPI sampler_port_sym_refresh(HANDLE process);
static BOOL SymGetModuleInfo64(HANDLE process, DWORD64 pc,
                              IMAGEHLP_MODULE64 *module);
static BOOL SymFromAddr(HANDLE process, DWORD64 pc, DWORD64 *displacement,
                       SYMBOL_INFO *symbol);
static DWORD64 SymGetModuleBase64(HANDLE process, DWORD64 pc);
static BOOL SymGetLineFromAddr64(HANDLE process, DWORD64 pc,
                                DWORD *displacement, IMAGEHLP_LINE64 *line);
static BOOL SymCleanup(HANDLE process);

#endif
