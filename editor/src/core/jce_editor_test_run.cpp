#include "jce_editor_test_run.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_trace.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace {
constexpr size_t output_limit = 1024 * 1024;
constexpr size_t poll_budget = 64 * 1024;
void observe(const JceEditorTestRun &run)
{
    if (!run.trace_id || !jce_trace_enabled()) return;
    jce_trace_counter(JCE_TRACE_CATEGORY_TASK, "editor_test_run_id", (int64_t)run.trace_id);
    jce_trace_counter(JCE_TRACE_CATEGORY_TASK, "editor_test_run_state", (int64_t)run.status);
}
void stop(JceEditorTestRun &run, JceEditorTestRunStatus terminal)
{
    if (!run.process || run.exit_observed) return;
    if (run.requested_terminal == JceEditorTestRunStatus::Idle)
        run.requested_terminal = terminal;
    run.status = JceEditorTestRunStatus::Stopping;
    if (!jce_process_force_kill(run.process))
        run.error = "Child stop failed; process remains owned and is still being polled.";
    run.next_stop_ms = jce_time_ticks_ms() + 1000;
    observe(run);
}
void append(JceEditorTestRun &run, const char *data, size_t size)
{
    const size_t kept = std::min(size, output_limit - run.output.size());
    run.output.append(data, kept);
    if (kept && !run.log_failed && !jce_fs_host_append(run.log_path.c_str(), data, kept)) {
        run.log_failed = true;
        run.error = "Test log write failed; output evidence is incomplete.";
        stop(run, JceEditorTestRunStatus::Failed);
    }
    if (kept != size && !run.output_truncated) {
        run.output_truncated = true;
        run.error = "Child output exceeded 1 MiB; truncated evidence cannot PASS.";
        static const char marker[] = "\n[OUTPUT TRUNCATED: 1 MiB limit; result is not PASS]\n";
        if (!run.log_failed && !jce_fs_host_append(run.log_path.c_str(), marker, sizeof(marker)-1))
            run.log_failed = true;
        stop(run, JceEditorTestRunStatus::Failed);
    }
}
/* Reads alternate stdout/stderr chunks; a live pipe is never drained without
   a per-tick bound. After exit, quiet pipes are required before release. */
bool drain(JceEditorTestRun &run)
{
    char buffer[4096]; size_t remaining = poll_budget;
    while (remaining) {
        bool any = false;
        size_t got = jce_process_read_stdout(run.process, buffer, std::min(remaining, sizeof(buffer)));
        if (got) { append(run,buffer,got); remaining -= got; any = true; }
        if (!remaining) return false;
        got = jce_process_read_stderr(run.process, buffer, std::min(remaining, sizeof(buffer)));
        if (got) { append(run,buffer,got); remaining -= got; any = true; }
        if (!any) return true;
    }
    return false;
}
void finish(JceEditorTestRun &run)
{
    jce_process_destroy(run.process); run.process = nullptr;
    if (run.log_failed || run.output_truncated) run.status = JceEditorTestRunStatus::Failed;
    else if (run.requested_terminal != JceEditorTestRunStatus::Idle) run.status = run.requested_terminal;
    else run.status = run.exit_code == 0 ? JceEditorTestRunStatus::Passed : JceEditorTestRunStatus::Failed;
    char footer[192];
    const int size = std::snprintf(footer,sizeof(footer),
        "\n[editor-test-run: %s exit=%d duration_s=%.3f]\n",
        jce_editor_test_run_status_name(run.status),run.exit_code,run.duration_s);
    if (!run.log_failed && (size<0 || (size_t)size>=sizeof(footer) ||
            !jce_fs_host_append(run.log_path.c_str(),footer,(size_t)size))) {
        run.log_failed = true; run.status = JceEditorTestRunStatus::Failed;
        run.error = "Test result log could not be finalized; result is not PASS.";
    }
    observe(run);
}
}
const char *jce_editor_test_run_status_name(JceEditorTestRunStatus status)
{
    switch(status) {
    case JceEditorTestRunStatus::Running: return "RUNNING";
    case JceEditorTestRunStatus::Stopping: return "STOPPING";
    case JceEditorTestRunStatus::Passed: return "PASS";
    case JceEditorTestRunStatus::Failed: return "FAIL";
    case JceEditorTestRunStatus::Cancelled: return "CANCELLED";
    case JceEditorTestRunStatus::TimedOut: return "TIMEOUT";
    default: return "PENDING";
    }
}
bool jce_editor_test_run_arguments(const JceJson *array,std::string &out,std::string &error)
{
    std::string encoded; error.clear();
    if (array && !jce_json_is_array(array)) { error="Test args must be a JSON array of strings."; return false; }
    const int count = array ? jce_json_array_size(array) : 0;
    for(int i=0;i<count;++i) {
        const JceJson *value=jce_json_array_at(array,i);
        const char *text=jce_json_string_value(value,nullptr);
        if (!text || !text[0] || std::strlen(text)>1023) {
            error="Test argv cannot be represented losslessly by the public process token contract.";
            return false;
        }
        if (i) encoded+=' ';
        encoded+='"';
        for(const char *p=text; *p; ++p) {
            /* Close double quote, quote a literal double quote with single
               quotes, then reopen. The common splitter joins these pieces. */
            if (*p=='"') encoded+="\"'\"'\"";
            else encoded+=*p;
        }
        encoded+='"';
    }
    out.swap(encoded); return true;
}
bool jce_editor_test_run_start(JceEditorTestRun &run,const char *executable,
    const char *arguments,const char *working_directory,const char *log_path,uint32_t timeout_ms)
{
    if (run.process || !executable || !executable[0] || !log_path || !log_path[0] || !timeout_ms) {
        run.error="Test start refused: active child, missing executable/log or invalid timeout."; return false;
    }
    char parent[1024];
    static const char header[]="[editor-test-run: started]\n";
    if (!jce_path_parent(parent,sizeof(parent),log_path) || !parent[0] ||
        !jce_fs_host_create_directory(parent) ||
        !jce_fs_host_write_new(log_path,header,sizeof(header)-1)) {
        run.error=std::string("Test start refused: exclusive log creation/write/close failed; previous result is retained. Candidate evidence may remain at ")+log_path;
        return false;
    }
    run.output.clear(); run.log_path=log_path; run.error.clear();
    run.exit_code=-1; run.exit_observed=false; run.output_truncated=run.log_failed=false;
    run.requested_terminal=JceEditorTestRunStatus::Idle; run.timeout_ms=timeout_ms;
    run.started_ms=jce_time_ticks_ms(); run.next_stop_ms=0; run.duration_s=0;
    run.trace_id=jce_trace_enabled()?jce_trace_next_id():0;
    JceProcessConfig config{};
    config.executable_path=executable; config.arguments=arguments; config.working_directory=working_directory;
    config.capture_stdout=config.capture_stderr=true;
    run.process=jce_process_spawn(&config);
    if (!run.process) {
        run.error=std::string("Test child spawn failed: ")+jce_process_get_last_spawn_error();
        run.status=JceEditorTestRunStatus::Failed; run.log_failed=false;
        finish(run); return false;
    }
    run.status=JceEditorTestRunStatus::Running; observe(run); return true;
}
void jce_editor_test_run_poll(JceEditorTestRun &run)
{
    if (!run.process) return;
    const uint64_t now=jce_time_ticks_ms();
    run.duration_s=now>=run.started_ms?(double)(now-run.started_ms)/1000.0:0;
    /* Deadline covers observed completion, including bounded pipe drain.
       Latch before exit polling; the common process port does not report the
       exact native exit timestamp, so a late poll cannot certify on-time exit. */
    if (run.requested_terminal==JceEditorTestRunStatus::Idle &&
            (now<run.started_ms || now-run.started_ms>=run.timeout_ms)) {
        run.requested_terminal=JceEditorTestRunStatus::TimedOut;
        run.status=JceEditorTestRunStatus::Stopping; run.next_stop_ms=0;
        observe(run);
    }
    if (!run.exit_observed) run.exit_observed=jce_process_poll_exit(run.process,&run.exit_code);
    if (!run.exit_observed && run.status==JceEditorTestRunStatus::Stopping && now>=run.next_stop_ms)
        stop(run,run.requested_terminal);
    const bool quiet=drain(run);
    if (run.exit_observed && quiet) finish(run);
}
void jce_editor_test_run_cancel(JceEditorTestRun &run) { stop(run,JceEditorTestRunStatus::Cancelled); }
void jce_editor_test_run_close(JceEditorTestRun &run)
{
    if (!run.process) return;
    if (run.requested_terminal==JceEditorTestRunStatus::Idle)
        run.requested_terminal=JceEditorTestRunStatus::Cancelled;
    /* Shutdown only: common process destroy owns force-kill/reap. UI polling
       never calls this until exit and pipe drain are observed. */
    finish(run);
}
JceEditorTestRun::~JceEditorTestRun() { jce_editor_test_run_close(*this); }
