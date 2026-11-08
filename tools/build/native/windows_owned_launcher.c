/* Tooling-only, current-token child launch. No graphics or process-tree claim.
 * Held ChildOps/ChildLife retirement derives from the unchanged first-party
 * xp-interactive-graphics-capability-trial/fixture.c. File capture is polled:
 * output can overshoot its observed limit between polls and termination.
 */
#include <windows.h>
#include <sddl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#define OUTPUT_LIMIT (32ULL * 1024ULL * 1024ULL)
#define OUTPUT_POLL_MS 25U
#define CLEANUP_LIMIT_MS 5000U
#define REPORT_LIMIT 16384U

typedef struct {
    DWORD (WINAPI *wait)(HANDLE, DWORD);
    BOOL (WINAPI *exit_code)(HANDLE, LPDWORD);
    BOOL (WINAPI *terminate)(HANDLE, UINT);
    BOOL (WINAPI *times)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME, LPFILETIME);
    BOOL (WINAPI *close)(HANDLE);
    DWORD (WINAPI *error)(void);
} ChildOps;
typedef struct {
    DWORD first_wait, final_wait, child_exit, error;
    BOOL birth_known, birth_stable, exit_known, timed_out, terminate_attempted, terminate_ok;
    BOOL reaped, thread_closed, process_closed, ok;
    FILETIME birth;
} ChildLife;
typedef struct {
    HANDLE out, err;
    ULONGLONG out_bytes, err_bytes;
    BOOL out_closed, err_closed, output_limit_exceeded;
} Streams;
typedef struct {
    const WCHAR *target, *workdir, *nonce, *desktop;
    DWORD timeout_ms;
} Options;
typedef struct { HANDLE handle; DWORD flags; } OldStandard;
typedef struct {
    char user_sid[256];
    DWORD token_type, session_id, query_error;
    BOOL user_known, type_known, session_known;
} Context;
static void observe_context(HANDLE process, Context *context) {
    HANDLE token; DWORD needed; TOKEN_TYPE type; DWORD session; char *sid = NULL;
    union { TOKEN_USER aligned; BYTE bytes[512]; } user;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) { context->query_error = GetLastError(); return; }
    if (GetTokenInformation(token, TokenUser, &user, sizeof user, &needed) &&
        IsValidSid(user.aligned.User.Sid) && ConvertSidToStringSidA(user.aligned.User.Sid, &sid)) {
        if (strlen(sid) < sizeof context->user_sid) {
            strcpy(context->user_sid, sid); context->user_known = TRUE;
        } else context->query_error = ERROR_INSUFFICIENT_BUFFER;
        LocalFree(sid);
    } else context->query_error = GetLastError();
    if (GetTokenInformation(token, TokenType, &type, sizeof type, &needed)) {
        context->token_type = (DWORD)type; context->type_known = TRUE;
    } else if (!context->query_error) context->query_error = GetLastError();
    if (GetTokenInformation(token, TokenSessionId, &session, sizeof session, &needed)) {
        context->session_id = session; context->session_known = TRUE;
    } else if (!context->query_error) context->query_error = GetLastError();
    if (!CloseHandle(token) && !context->query_error) context->query_error = GetLastError();
}
static char report_text[REPORT_LIMIT];
static size_t report_bytes;
static BOOL report_overflow;

static void first_error(DWORD *error, DWORD value) {
    if (!*error) *error = value ? value : ERROR_GEN_FAILURE;
}
static BOOL same_time(FILETIME a, FILETIME b) {
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}
static BOOL stream_sizes(Streams *streams, DWORD *error) {
    LARGE_INTEGER out_size, err_size;
    if (!GetFileSizeEx(streams->out, &out_size) || !GetFileSizeEx(streams->err, &err_size)) {
        first_error(error, GetLastError()); return FALSE;
    }
    if (out_size.QuadPart < 0 || err_size.QuadPart < 0) {
        first_error(error, ERROR_INVALID_DATA); return FALSE;
    }
    streams->out_bytes = (ULONGLONG)out_size.QuadPart;
    streams->err_bytes = (ULONGLONG)err_size.QuadPart;
    if (streams->out_bytes > OUTPUT_LIMIT || streams->err_bytes > OUTPUT_LIMIT - streams->out_bytes)
        streams->output_limit_exceeded = TRUE;
    return TRUE;
}
static DWORD observe_child(HANDLE process, const ChildOps *ops, ChildLife *life,
                           Streams *streams, DWORD timeout_ms) {
    DWORD started = GetTickCount(), elapsed, wait;
    for (;;) {
        if (!stream_sizes(streams, &life->error)) return WAIT_FAILED;
        if (streams->output_limit_exceeded) return WAIT_TIMEOUT;
        elapsed = GetTickCount() - started;
        if (elapsed >= timeout_ms) { life->timed_out = TRUE; return WAIT_TIMEOUT; }
        wait = ops->wait(process, timeout_ms - elapsed < OUTPUT_POLL_MS ? timeout_ms - elapsed : OUTPUT_POLL_MS);
        if (wait == WAIT_OBJECT_0) return wait;
        if (wait != WAIT_TIMEOUT) { first_error(&life->error, ops->error()); return wait; }
    }
}
static BOOL retire_child(HANDLE process, HANDLE thread, const ChildOps *ops,
                         ChildLife *life, Streams *streams, DWORD timeout_ms) {
    FILETIME end, kernel, user, after;
    memset(life, 0, sizeof *life);
    life->birth_known = ops->times(process, &life->birth, &end, &kernel, &user);
    if (!life->birth_known) first_error(&life->error, ops->error());
    life->thread_closed = ops->close(thread);
    if (!life->thread_closed) first_error(&life->error, ops->error());
    life->first_wait = life->error ? WAIT_FAILED : observe_child(process, ops, life, streams, timeout_ms);
    life->final_wait = life->first_wait;
    if (life->first_wait != WAIT_OBJECT_0) {
        life->terminate_attempted = TRUE;
        life->terminate_ok = ops->terminate(process, 92);
        if (!life->terminate_ok) first_error(&life->error, ops->error());
        life->final_wait = ops->wait(process, CLEANUP_LIMIT_MS);
    }
    life->reaped = life->final_wait == WAIT_OBJECT_0;
    if (life->reaped) {
        life->exit_known = ops->exit_code(process, &life->child_exit);
        if (!life->exit_known) first_error(&life->error, ops->error());
        if (!ops->times(process, &after, &end, &kernel, &user)) first_error(&life->error, ops->error());
        else life->birth_stable = life->birth_known && same_time(life->birth, after);
        if (!life->birth_stable) first_error(&life->error, ERROR_INVALID_DATA);
    } else first_error(&life->error, ERROR_TIMEOUT);
    if (!stream_sizes(streams, &life->error)) life->ok = FALSE;
    life->process_closed = ops->close(process);
    if (!life->process_closed) first_error(&life->error, ops->error());
    /* Observation success preserves a real nonzero child exit or bounded timeout. */
    life->ok = !life->error && life->reaped && life->exit_known && life->birth_stable &&
               life->thread_closed && life->process_closed;
    return life->ok;
}
static BOOL local_path(const WCHAR *path) {
    const WCHAR *part, *p;
    size_t size = wcslen(path);
    if (size < 3 || size >= MAX_PATH || !((path[0] >= L'A' && path[0] <= L'Z') ||
        (path[0] >= L'a' && path[0] <= L'z')) || path[1] != L':' || path[2] != L'\\') return FALSE;
    part = path + 3;
    for (p = part; ; ++p) {
        if (*p && (*p < 32 || wcschr(L"/\"<>|?*:", *p))) return FALSE;
        if (!*p || *p == L'\\') {
            size_t count = (size_t)(p - part);
            if ((!count && p != path + 3) || (count == 1 && part[0] == L'.') ||
                (count == 2 && part[0] == L'.' && part[1] == L'.') ||
                (count && (part[count - 1] == L'.' || part[count - 1] == L' '))) return FALSE;
            if (!*p) break;
            part = p + 1;
        }
    }
    return TRUE;
}
static BOOL parse_options(int argc, WCHAR **argv, Options *options) {
    int i; unsigned seen = 0; WCHAR *end; unsigned long timeout;
    memset(options, 0, sizeof *options);
    if (argc != 11) return FALSE;
    for (i = 1; i < argc; i += 2) {
        unsigned bit;
        if (!wcscmp(argv[i], L"--target")) { bit = 1; options->target = argv[i + 1]; }
        else if (!wcscmp(argv[i], L"--workdir")) { bit = 2; options->workdir = argv[i + 1]; }
        else if (!wcscmp(argv[i], L"--nonce")) { bit = 4; options->nonce = argv[i + 1]; }
        else if (!wcscmp(argv[i], L"--desktop")) { bit = 8; options->desktop = argv[i + 1]; }
        else if (!wcscmp(argv[i], L"--timeout-ms")) {
            bit = 16;
            if (!argv[i + 1][0] || wcscspn(argv[i + 1], L"0123456789") ||
                wcsspn(argv[i + 1], L"0123456789") != wcslen(argv[i + 1])) return FALSE;
            timeout = wcstoul(argv[i + 1], &end, 10);
            if (*end || timeout < 1 || timeout > 3600000UL) return FALSE;
            options->timeout_ms = (DWORD)timeout;
        } else return FALSE;
        if (seen & bit) return FALSE;
        seen |= bit;
    }
    return seen == 31 && local_path(options->target) && local_path(options->workdir) &&
        wcslen(options->nonce) == 32 && wcsspn(options->nonce, L"0123456789abcdef") == 32 &&
        !wcscmp(options->desktop, L"WinSta0\\Default");
}
static BOOL output_path(WCHAR *path, const WCHAR *workdir, const WCHAR *name) {
    size_t size = wcslen(workdir), name_size = wcslen(name);
    BOOL slash = workdir[size - 1] != L'\\';
    if (size + slash + name_size >= MAX_PATH) return FALSE;
    wcscpy(path, workdir);
    if (slash) wcscat(path, L"\\");
    wcscat(path, name); return TRUE;
}
static BOOL save_standard_flags(OldStandard *old, unsigned *count, DWORD *error) {
    const DWORD ids[] = { STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
    unsigned i, j;
    *count = 0;
    for (i = 0; i < 3; ++i) {
        HANDLE handle = GetStdHandle(ids[i]);
        if (!handle || handle == INVALID_HANDLE_VALUE) continue;
        for (j = 0; j < *count && old[j].handle != handle; ++j) {}
        if (j < *count) continue;
        old[*count].handle = handle;
        if (!GetHandleInformation(handle, &old[*count].flags)) { first_error(error, GetLastError()); return FALSE; }
        ++*count;
        if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0)) { first_error(error, GetLastError()); return FALSE; }
    }
    /* Only the three borrowed std handles are inspected; no complete inventory claim. */
    return TRUE;
}
static void restore_standard_flags(OldStandard *old, unsigned count, DWORD *error) {
    unsigned i;
    for (i = 0; i < count; ++i)
        if (!SetHandleInformation(old[i].handle, HANDLE_FLAG_INHERIT, old[i].flags & HANDLE_FLAG_INHERIT))
            first_error(error, GetLastError());
}
static void close_file(HANDLE handle, BOOL *closed, DWORD *error) {
    if (handle == INVALID_HANDLE_VALUE) return;
    *closed = CloseHandle(handle);
    if (!*closed) first_error(error, GetLastError());
}
static void append(const char *format, ...) {
    int count; va_list args;
    if (report_overflow) return;
    va_start(args, format);
    count = vsnprintf(report_text + report_bytes, REPORT_LIMIT - report_bytes, format, args);
    va_end(args);
    if (count < 0 || (size_t)count >= REPORT_LIMIT - report_bytes) report_overflow = TRUE;
    else report_bytes += (size_t)count;
}
static void json_wide(const WCHAR *value) {
    append("\"");
    for (; *value; ++value) {
        unsigned c = (unsigned)*value;
        if (c == '\\' || c == '"') append("\\%c", (int)c);
        else if (c >= 32 && c < 127) append("%c", (int)c);
        else append("\\u%04x", c);
    }
    append("\"");
}
static const char *boolean(BOOL value) { return value ? "true" : "false"; }
static BOOL write_report(HANDLE file, const Options *options, BOOL created, DWORD pid,
                         const ChildLife *life, const Streams *streams, const Context *context, DWORD error,
                         const WCHAR *out_name, const WCHAR *err_name) {
    DWORD written;
    append("{\"schema\":\"jce.windows.owned-launch.v1\",\"nonce\":"); json_wide(options->nonce);
    append(",\"target_path\":"); json_wide(options->target);
    append(",\"working_dir\":"); json_wide(options->workdir);
    append(",\"requested_desktop\":"); json_wide(options->desktop);
    append(",\"context\":{\"user_sid\":");
    if (context->user_known) append("\"%s\"", context->user_sid); else append("null");
    append(",\"token_type\":");
    if (context->type_known) append("%lu", (unsigned long)context->token_type); else append("null");
    append(",\"session_id\":");
    if (context->session_known) append("%lu", (unsigned long)context->session_id); else append("null");
    append(",\"query_error\":%lu}", (unsigned long)context->query_error);
    append(",\"controller_error\":%lu,\"child\":{\"created\":%s,\"pid\":%lu,\"birth_low\":%lu,\"birth_high\":%lu,",
        (unsigned long)error, boolean(created), (unsigned long)pid,
        (unsigned long)life->birth.dwLowDateTime, (unsigned long)life->birth.dwHighDateTime);
    append("\"birth_known\":%s,\"birth_stable\":%s,\"first_wait\":%lu,\"final_wait\":%lu,\"exit_known\":%s,\"exit_code\":%lu,",
        boolean(life->birth_known), boolean(life->birth_stable), (unsigned long)life->first_wait,
        (unsigned long)life->final_wait, boolean(life->exit_known), (unsigned long)life->child_exit);
    append("\"timed_out\":%s,\"terminate_attempted\":%s,\"terminate_ok\":%s,\"reaped\":%s,\"thread_closed\":%s,\"process_closed\":%s},",
        boolean(life->timed_out), boolean(life->terminate_attempted), boolean(life->terminate_ok),
        boolean(life->reaped), boolean(life->thread_closed), boolean(life->process_closed));
    append("\"streams\":{\"stdout_name\":"); json_wide(out_name);
    append(",\"stderr_name\":"); json_wide(err_name);
    append(",\"stdout_bytes\":%llu,\"stderr_bytes\":%llu,\"stdout_closed\":%s,\"stderr_closed\":%s,\"output_limit_exceeded\":%s,",
        (unsigned long long)streams->out_bytes, (unsigned long long)streams->err_bytes,
        boolean(streams->out_closed), boolean(streams->err_closed), boolean(streams->output_limit_exceeded));
    append("\"output_limit_bytes\":33554432,\"output_poll_ms\":25,\"byte_limit_policy\":\"observed-periodic-file-size; overshoot possible\"}}\n");
    return !report_overflow && WriteFile(file, report_text, (DWORD)report_bytes, &written, NULL) && written == report_bytes;
}
int wmain(int argc, WCHAR **argv) {
    Options options; ChildLife life; Streams streams; OldStandard old[3]; Context context;
    STARTUPINFOW startup; PROCESS_INFORMATION process;
    ChildOps ops = { WaitForSingleObject, GetExitCodeProcess, TerminateProcess, GetProcessTimes, CloseHandle, GetLastError };
    SECURITY_ATTRIBUTES inherit = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    WCHAR out_name[80], err_name[80], report_name[80], path[MAX_PATH], command[MAX_PATH + 3];
    HANDLE report = INVALID_HANDLE_VALUE, input = INVALID_HANDLE_VALUE;
    DWORD error = 0, attributes; unsigned old_count = 0; BOOL created = FALSE, closed;
    memset(&life, 0, sizeof life); memset(&streams, 0, sizeof streams); memset(&context, 0, sizeof context);
    memset(&process, 0, sizeof process); memset(&startup, 0, sizeof startup);
    streams.out = streams.err = INVALID_HANDLE_VALUE;
    if (!parse_options(argc, argv, &options)) return 2;
    attributes = GetFileAttributesW(options.workdir);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 2;
    attributes = GetFileAttributesW(options.target);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return 2;
    swprintf(out_name, 80, L"jce-child-%ls.stdout.bin", options.nonce);
    swprintf(err_name, 80, L"jce-child-%ls.stderr.bin", options.nonce);
    swprintf(report_name, 80, L"jce-launch-%ls.json", options.nonce);
    if (!output_path(path, options.workdir, report_name)) return 2;
    report = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (report == INVALID_HANDLE_VALUE) return 3;
    if (!save_standard_flags(old, &old_count, &error)) goto cleanup;
    if (!output_path(path, options.workdir, out_name)) { first_error(&error, ERROR_FILENAME_EXCED_RANGE); goto cleanup; }
    streams.out = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (streams.out == INVALID_HANDLE_VALUE) { first_error(&error, GetLastError()); goto cleanup; }
    if (!output_path(path, options.workdir, err_name)) { first_error(&error, ERROR_FILENAME_EXCED_RANGE); goto cleanup; }
    streams.err = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (streams.err == INVALID_HANDLE_VALUE) { first_error(&error, GetLastError()); goto cleanup; }
    input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (input == INVALID_HANDLE_VALUE) { first_error(&error, GetLastError()); goto cleanup; }
    startup.cb = sizeof startup; startup.lpDesktop = (WCHAR *)options.desktop;
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_FORCEOFFFEEDBACK;
    startup.hStdInput = input; startup.hStdOutput = streams.out; startup.hStdError = streams.err;
    swprintf(command, MAX_PATH + 3, L"\"%ls\"", options.target);
    created = CreateProcessW(options.target, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                             NULL, options.workdir, &startup, &process);
    if (!created) first_error(&error, GetLastError());
    if (created) {
        observe_context(process.hProcess, &context);
        if (!SetHandleInformation(streams.out, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(streams.err, HANDLE_FLAG_INHERIT, 0)) first_error(&error, GetLastError());
        retire_child(process.hProcess, process.hThread, &ops, &life, &streams, options.timeout_ms);
        if (life.error) first_error(&error, life.error);
    }
cleanup:
    close_file(input, &closed, &error);
    close_file(streams.out, &streams.out_closed, &error);
    close_file(streams.err, &streams.err_closed, &error);
    restore_standard_flags(old, old_count, &error);
    if (!write_report(report, &options, created, process.dwProcessId, &life, &streams, &context, error, out_name, err_name))
        first_error(&error, GetLastError());
    close_file(report, &closed, &error);
    return error || !created || !life.ok ? 1 : 0;
}
