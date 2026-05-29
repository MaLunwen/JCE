/*
 * jce_process.h  Cross-platform external-process management.
 *
 * Thin SDL3-backed wrapper around SDL_CreateProcessWithProperties /
 * SDL_KillProcess / SDL_WaitProcess.  Used by the editor's run manager
 * (and any tool) to launch and supervise child processes without ever
 * touching CreateProcess / fork / exec directly.
 *
 * Threading: all functions are expected to be called from the same
 * thread (typically the main thread).  The implementation is non-
 * blocking; jce_process_poll_exit / jce_process_read_* drive progress.
 *
 * Layer: Core (Layer 0 — depends on SDL3 only).
 */

#ifndef JCE_PROCESS_H
#define JCE_PROCESS_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_PROCESS_IDLE     = 0,
    JCE_PROCESS_RUNNING  = 1,
    JCE_PROCESS_STOPPING = 2,
    JCE_PROCESS_EXITED   = 3,
    JCE_PROCESS_FAILED   = 4
} JceProcessState;

typedef struct {
    /* Required: absolute or relative path to the executable. */
    const char *executable_path;

    /* Optional: working directory.  NULL or "" means inherit from the
       caller.  Must exist if non-empty. */
    const char *working_directory;

    /* Optional: a single string of extra arguments.  Tokens are split
       respecting single/double quoting (no shell expansion).  May be
       NULL or "". */
    const char *arguments;

    /* If true, redirect the child's stdout to a non-blocking pipe that
       can be drained with jce_process_read_stdout.  If false, the
       child's stdout is inherited from the caller. */
    bool capture_stdout;

    /* Same as capture_stdout for stderr. */
    bool capture_stderr;
} JceProcessConfig;

typedef struct JceProcess JceProcess;

/* Spawn a new process.  Returns NULL on failure (use SDL_GetError
   for details).  Caller owns the returned handle. */
JCE_API JceProcess *jce_process_spawn(const JceProcessConfig *cfg);

/* Destroy a process handle.  If the child is still running, it is
   force-killed and reaped first.  Always safe to call with NULL. */
JCE_API void jce_process_destroy(JceProcess *p);

/* Read up to `cap` bytes from the child's stdout into `buf`.
   Returns the number of bytes actually read (0 if no data is
   currently available or stdout was not captured). */
JCE_API size_t jce_process_read_stdout(JceProcess *p, char *buf, size_t cap);

/* Same as jce_process_read_stdout but for stderr. */
JCE_API size_t jce_process_read_stderr(JceProcess *p, char *buf, size_t cap);

/* Request a graceful stop (SDL_KillProcess(force=false)).
   Returns false if the underlying SDL call failed. */
JCE_API bool jce_process_request_stop(JceProcess *p);

/* Force-kill the child (SDL_KillProcess(force=true)). */
JCE_API bool jce_process_force_kill(JceProcess *p);

/* Non-blocking exit poll.  Returns true if the process has exited;
   in that case *out_exit_code (if non-NULL) receives the exit code.
   Returns false while the process is still running. */
JCE_API bool jce_process_poll_exit(JceProcess *p, int *out_exit_code);

/* Return the SDL error message captured by the most recent failed
 * jce_process_spawn() call on the current thread.  Returns "" if no
 * error is pending.  Useful for diagnosing why spawn returned NULL. */
JCE_API const char *jce_process_get_last_spawn_error(void);

JCE_EXTERN_C_END

#endif /* JCE_PROCESS_H */
