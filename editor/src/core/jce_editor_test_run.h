#ifndef JCE_EDITOR_TEST_RUN_H
#define JCE_EDITOR_TEST_RUN_H
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_json.h>
#include <cstdint>
#include <string>

enum class JceEditorTestRunStatus { Idle, Running, Stopping, Passed, Failed, Cancelled, TimedOut };
struct JceEditorTestRun {
    JceProcess *process = nullptr; /* Sole owned child; main-thread API. */
    JceEditorTestRunStatus status = JceEditorTestRunStatus::Idle;
    JceEditorTestRunStatus requested_terminal = JceEditorTestRunStatus::Idle;
    std::string output, log_path, error;
    uint64_t started_ms = 0, next_stop_ms = 0, trace_id = 0;
    uint32_t timeout_ms = 60000; /* Deadline of observed completion, not native exit time. */
    int exit_code = -1;
    double duration_s = 0;
    bool exit_observed = false, output_truncated = false, log_failed = false;
    JceEditorTestRun() = default;
    JceEditorTestRun(const JceEditorTestRun &) = delete;
    JceEditorTestRun &operator=(const JceEditorTestRun &) = delete;
    ~JceEditorTestRun();
};
/* A failed preflight does not replace a previous result or spawn a child.
   Public write_new exclusively creates the log; a failed write/close can leave
   partial evidence at that candidate path, never implying absence.
   Logs must be a fresh path in the caller's owned writable build/profile tree. */
bool jce_editor_test_run_start(JceEditorTestRun &run, const char *executable,
    const char *arguments, const char *working_directory, const char *log_path,
    uint32_t timeout_ms);
void jce_editor_test_run_poll(JceEditorTestRun &run);
void jce_editor_test_run_cancel(JceEditorTestRun &run);
void jce_editor_test_run_close(JceEditorTestRun &run);
const char *jce_editor_test_run_status_name(JceEditorTestRunStatus status);
/* Uses public JSON values and the existing process quote/token contract;
   unsupported or lossy argv representations fail instead of being truncated. */
bool jce_editor_test_run_arguments(const JceJson *array, std::string &out, std::string &error);
#endif
