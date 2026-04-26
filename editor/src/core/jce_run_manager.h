/*
 * jce_run_manager.h  External game process runner for the editor.
 */

#ifndef JCE_RUN_MANAGER_H
#define JCE_RUN_MANAGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JCE_RUN_IDLE = 0,
    JCE_RUN_RUNNING,
    JCE_RUN_STOPPING,
    JCE_RUN_EXITED,
    JCE_RUN_FAILED,
} JceRunState;

typedef struct {
    char executable_path[512];
    char working_directory[512];
    char arguments[1024];
    bool capture_stdout;
    bool capture_stderr;
} JceRunConfig;

typedef struct {
    JceRunState state;
    bool running;
    int exit_code;
    char last_error[256];
} JceRunStatus;

void jce_run_manager_init(void);
void jce_run_manager_shutdown(void);
void jce_run_manager_poll(void);

/* Public run manager calls are expected from the editor main thread. */
bool jce_run_manager_start(const JceRunConfig *cfg);
void jce_run_manager_request_stop(void);
bool jce_run_manager_is_running(void);
JceRunState jce_run_manager_state(void);
void jce_run_manager_get_status(JceRunStatus *out_status);

#ifdef __cplusplus
}
#endif

#endif /* JCE_RUN_MANAGER_H */
