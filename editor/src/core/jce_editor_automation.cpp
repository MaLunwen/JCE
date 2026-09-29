/*
 * jce_editor_automation.cpp — see jce_editor_automation.h.
 */
#include "jce_editor_automation.h"

#include <stdio.h>
#include <string.h>

#include <jce/os/core/jce_defs.h>   /* JCE_PLATFORM_WINDOWS */
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_timer.h>
#include <jce/middleware/scene/jce_scene.h>

#include "dialogs/jce_editor_dialogs.h"   /* open_file_dialog_async */
#include "jce_editor_i18n.h"
#include "jce_editor_project_state.h"
#include "jce_editor_state.h"
#include "jce_editor_toast.h"
#include "ui/jce_editor_panels.h"   /* jce_editor_path_to_relative_to */

/* AN UNDEFINED MACRO IS 0 IN #if, SILENTLY.  Were jce_defs.h ever not
 * reached from here, python_name below would take the POSIX path on Windows and
 * look for python3 -- a spawn failure blamed on a missing interpreter that is
 * in fact installed.  #error turns that into a build error instead. */
#if !defined(JCE_PLATFORM_WINDOWS)
#error "JCE_PLATFORM_WINDOWS is not defined; include <jce/os/core/jce_defs.h>"
#endif

#define LOG_TAG "automation"

namespace {

enum { AUTOMATION_OUT_MAX = 1 << 20 };   /* 1 MiB of envelope is plenty */

/* A WRITING CALL IS THREE CALLS, and that is not an implementation detail --
 * it is what REQ-AUTO-01 means in practice.  api.call does
 * `if spec.writes: require_changeset()`, so none of the twelve project-writing
 * tools can run outside one, and the CLI has no single-shot form.  So the door
 * runs a plan:
 *
 *     changeset.begin  ->  the tool  ->  changeset.commit
 *                          and on ANY failure: changeset.rollback
 *
 * A read-only call is the same machinery with a one-step plan, so there is one
 * process-owning path rather than two.
 *
 * WHY ROLLBACK IS NOT OPTIONAL HERE.  A failed write that leaves its changeset
 * open leaves the project half-changed AND holds the one call slot for the
 * rest of the session -- the failure and the refusal to try again would arrive
 * together, and the second would be blamed on the first. */
enum CallStep {
    STEP_BEGIN = 0,     /* changeset.begin                       */
    STEP_TOOL,          /* the tool the caller asked for         */
    STEP_VALIDATE,      /* changeset.validate -- NOT optional    */
    STEP_COMMIT,        /* changeset.commit                      */
    STEP_ROLLBACK,      /* changeset.rollback, on any failure    */
    STEP_DONE
};

struct Call {
    JceProcess *proc = nullptr;
    char        tool[128] = {0};      /* what the caller asked for */
    /* HEAP, NOT char[1024], AND THAT IS THE WHOLE POINT.  physics.author_apply
     * takes the entire physics.author_plan result as one argument, and a plan
     * for a ONE-part prop measures 2671 bytes (Fox.glb, measured).  The fixed
     * buffer this replaced truncated silently at 1023, as did split_args() in
     * jce_process.c and the argv[] in spawn_step -- three silent truncations
     * on one path.  Truncated JSON usually fails to parse; "usually" is the
     * problem, because the remainder can be a WELL-FORMED object that means
     * something else, and nothing anywhere records the dropped bytes. */
    char       *args = nullptr;
    char        args_file[640] = {0}; /* where args were written for --json-file */
    char        title[160] = {0};     /* changeset title / commit message */
    char        changeset[128] = {0}; /* id, once begin has answered */
    /* The changeset a write left OPEN, awaiting the human's commit or
     * rollback.  It OUTLIVES one call by design -- the review happens between
     * processes -- so call_reset() deliberately does not clear it.
     *
     * A FIELD HERE RATHER THAN A SECOND FILE-STATIC.  As its own name it was
     * global-state 860 -> 861, and this module already IS one struct holding
     * the module; a second name would have been the shape this detector has
     * been teaching since the text shape cache.  Caught the only time it
     * could be: after committing, because find_duplicate_symbols.py
     * enumerates with `git ls-files`. */
    char        open_cs[128] = {0};
    /* The last PLAN, kept as JSON text, plus which tool produced it and what
     * it describes.  Like open_cs it OUTLIVES one call on purpose: a plan is
     * read in one process and written in another, with a person reading the
     * reasons in between -- that gap is what REQ-PHY-03 is for.
     *
     * ONE SLOT FOR THREE PLANNERS (physics.author_plan, physics.ragdoll_plan,
     * scene.compile), which is why plan_tool exists.  Three slots would be
     * three chances for an apply to read a stale one, and the failure would
     * not look like a failure: every applier takes an `object`, so feeding
     * ragdoll_apply a collider plan is a type-correct call that writes
     * nonsense.  plan_tool makes that unrepresentable instead of unlikely.
     *
     * plan_subject is the same defence one level down: planning model A,
     * selecting B and applying would author A onto B, and the scene would
     * look entirely plausible. */
    char       *plan = nullptr;
    char        plan_tool[64] = {0};
    char        plan_subject[512] = {0};
    /* Distinguishes the args files of the steps within one plan.  A fixed
     * name would be a shared mutable path, and this tree has already paid for
     * one of those: three call sites writing a fixed path overwrote each
     * other's input and the same recipe produced three different answers.
     * The nanosecond clock separates two editors; this separates two steps
     * inside one of them, which can share a nanosecond.
     *
     * A FIELD, not a file-static: the state belongs to the one call this
     * module is, and a second name would be global-state +1 for nothing. */
    unsigned    args_seq = 0;
    /* A recipe file pick in flight.  scene.compile takes a recipe OBJECT, so
     * something has to choose one, and the host file dialog is asynchronous --
     * hence a second small state here rather than a blocking call on the UI
     * thread.  It is driven from the same per-frame poll the call plan uses,
     * which is why it lives in this struct and not in the menu bar: the menu
     * bar is drawn, not pumped, and a dialog that completes while a menu is
     * closed would never be noticed. */
    bool        pick_dispatched = false;
    bool        pick_ready = false;
    bool        pick_cancelled = false;
    char        pick_path[1024] = {0};
    char        running[128] = {0};   /* the tool of the step in flight */
    int         step = STEP_DONE;
    bool        writing = false;      /* three-step plan rather than one */
    char       *out = nullptr;
    size_t      len = 0;
    bool        truncated = false;   /* the reply outgrew AUTOMATION_OUT_MAX */
};

Call s_call;

void call_reset(void)
{
    if (s_call.proc) {
        jce_process_destroy(s_call.proc);
        s_call.proc = nullptr;
    }
    if (s_call.out) {
        jce_free(s_call.out);
        s_call.out = nullptr;
    }
    if (s_call.args) {
        jce_free(s_call.args);
        s_call.args = nullptr;
    }
    if (s_call.args_file[0]) {
        /* Best effort: the CLI has already read it, and a leftover under
         * build/ is harmless.  Not worth a diagnostic -- but not worth
         * leaving the caller's arguments on disk either. */
        jce_fs_host_remove_file(s_call.args_file);
        s_call.args_file[0] = 0;
    }
    s_call.len = 0;
    s_call.truncated = false;
    s_call.tool[0] = 0;
    s_call.title[0] = 0;
    s_call.changeset[0] = 0;
    s_call.running[0] = 0;
    s_call.step = STEP_DONE;
    s_call.writing = false;
}

/* Take a copy of the caller's arguments, of whatever length.
 *
 * Its own function because the two entry points both need it and both used to
 * do it with a truncating snprintf into a fixed buffer.  Returns false only on
 * allocation failure, which call_reset() then cleans up after. */
bool call_set_args(const char *json_args)
{
    const char *src = (json_args && json_args[0]) ? json_args : "{}";
    size_t n = strlen(src);
    if (s_call.args)
        jce_free(s_call.args);
    s_call.args = (char *)jce_malloc(n + 1);
    if (!s_call.args) {
        call_reset();
        return false;
    }
    memcpy(s_call.args, src, n + 1);
    return true;
}

/* The interpreter.  Deliberately the bare name: the editor runs from a build
 * tree, an install tree or a developer's shell, and the one thing common to
 * all three is that whatever put python on PATH is what the CLI expects to be
 * run with.  A failed spawn says which name it tried rather than silently
 * guessing a second location. */
const char *python_name(void)
{
    /* JCE_PLATFORM_WINDOWS, not _WIN32: raw platform macros are refused by
     * tools/lint/check_platform_macros.py, and it caught this one. */
#if JCE_PLATFORM_WINDOWS
    return "python.exe";
#else
    return "python3";
#endif
}

/* Empty the child's stdout pipe into the envelope buffer.
 *
 * Named automation_drain_stdout, not drain_stdout: jce_run_manager.cpp
 * already has a file-local drain_stdout and the dedup audit counts by NAME
 * across the
 * tree, not by linkage.  Two anonymous-namespace functions cannot see each
 * other, so nothing breaks -- but a detector that reports a collision it
 * cannot distinguish from a real copy is one whose findings get ignored, and
 * this was caught the only time it could be: AFTER committing, because
 * find_duplicate_symbols.py enumerates sources with `git ls-files`.
 *
 * Called TWICE per completed call -- once before polling for exit and once
 * after the reap -- because those are two separate calls and anything written
 * between them is still in the pipe.  One function rather than two copies:
 * the tree's dedup audit counts near-identical blocks, and the second copy is
 * exactly where a fix to one of them would fail to land. */
void automation_drain_stdout(void)
{
    char buf[4096];
    for (;;) {
        size_t got = jce_process_read_stdout(s_call.proc, buf, sizeof(buf));
        if (got == 0)
            break;
        if (s_call.out && s_call.len + got < AUTOMATION_OUT_MAX) {
            memcpy(s_call.out + s_call.len, buf, got);
            s_call.len += got;
        } else {
            /* SAYS SO RATHER THAN TRUNCATING SILENTLY.  A cut-off envelope
             * fails to parse, and "not JSON" about a reply that was merely too
             * long sends the reader after the wrong thing. */
            s_call.truncated = true;
        }
    }
}


void report_envelope(const char *text, size_t len)
{
    JceJson *root = jce_json_parse(text, len);
    if (!root) {
        /* NOT an empty result.  "Could not read the answer" and "the answer
         * was empty" are different things to show somebody, and collapsing
         * them is how a broken call reads as a clean one. */
        LOG_ERROR(LOG_TAG, "could not parse the reply as JSON (%d bytes); "
                           "raw output follows", (int)len);
        LOG_INFO(LOG_TAG, "%.*s", (int)(len > 2000 ? 2000 : len), text);
        jce_toast_error("%s", jce_editor_i18n_or(
            "toast.automationBadReply",
            "The automation tool replied with something that is not JSON"));
        return;
    }

    const bool ok = jce_json_get_bool(root, "ok", false);
    if (!ok) {
        JceJson *err = jce_json_get(root, "error");
        const char *code = err ? jce_json_get_string(err, "code", "UNKNOWN")
                               : "UNKNOWN";
        const char *msg = err ? jce_json_get_string(err, "message", "") : "";
        LOG_ERROR(LOG_TAG, "%s refused: %s -- %s", s_call.tool, code, msg);
        jce_toast_error("%s: %s", s_call.tool, code);
        jce_json_free(root);
        return;
    }

    JceJson *res = jce_json_get(root, "result");
    if (!res) {
        LOG_INFO(LOG_TAG, "%s: ok, no result body", s_call.tool);
        jce_json_free(root);
        return;
    }

    /* physics.probe's own shape.  Reported FIELD BY FIELD rather than as a
     * verdict, because a verdict without the numbers it was made from is
     * precisely what this layer exists to stop producing. */
    JceJson *metrics = jce_json_get(res, "metrics");
    const bool measured = jce_json_get_bool(res, "dynamic_measured", false);
    const int  errors = (int)jce_json_get_number(res, "static_error_count", 0);

    if (metrics && measured) {
        LOG_INFO(LOG_TAG,
                 "%s: penetration %.5f m | nan %d | settled %s | jitter "
                 "%.4f m/s | tilt %.2f deg",
                 s_call.tool,
                 jce_json_get_number(metrics, "max_penetration_m", 0.0),
                 (int)jce_json_get_number(metrics, "nan_events", 0),
                 jce_json_get_bool(metrics, "settled", false) ? "yes" : "no",
                 /* jitter_speed_m_per_s, NOT max_jitter_speed_m_per_s.  The
                  * THRESHOLD carries the max_ prefix; the measurement does
                  * not, and reading the threshold's name off the metrics
                  * object returns the default silently. */
                 jce_json_get_number(metrics, "jitter_speed_m_per_s", 0.0),
                 jce_json_get_number(metrics, "max_tilt_deg", 0.0));
    } else {
        /* MEASURED NOTHING IS NOT MEASURED FINE.  The solver harness is an
         * SDK consumer that has to be built; when it is absent the probe
         * still returns its static findings, and reporting that as a clean
         * run is the same defect as a screenshot of the wrong program. */
        const char *why = jce_json_get_string(res, "dynamic_unavailable_reason",
                                              "the solver harness did not run");
        LOG_WARN(LOG_TAG, "%s: NO DYNAMIC MEASUREMENT -- %s", s_call.tool, why);
    }
    LOG_INFO(LOG_TAG, "%s: %d static error finding(s)", s_call.tool, errors);

    /* THE FIELD NAMES ARE THE TOOL'S, TAKEN FROM ITS OUTPUT.  The first
     * version of this loop read pass / name / measured / threshold, which are
     * the four names the shape SOUNDS like it has.  It has metric / value /
     * limit / ok / unit.  Every accessor would have returned its default: the
     * editor would have printed a column of plausible FAIL rows with 0.00000
     * measured against 0.00000, from a probe whose verdicts were all ok.  A
     * parser written from an assumption about a shape is a fixture that reads
     * right and is wrong. */
    JceJson *verdicts = jce_json_get(res, "verdicts");
    const int n = verdicts ? jce_json_array_size(verdicts) : 0;
    for (int i = 0; i < n; ++i) {
        JceJson *v = jce_json_array_at(verdicts, i);
        if (!v) continue;
        LOG_INFO(LOG_TAG, "  %s %s: %.5f%s against %.5f",
                 jce_json_get_bool(v, "ok", false) ? "PASS" : "FAIL",
                 jce_json_get_string(v, "metric", "?"),
                 jce_json_get_number(v, "value", 0.0),
                 jce_json_get_string(v, "unit", ""),
                 jce_json_get_number(v, "limit", 0.0));
    }
    if (n == 0) {
        LOG_INFO(LOG_TAG, "  (no threshold verdicts: nothing dynamic was "
                          "measured, so there was nothing to judge)");
    }

    jce_toast_info("%s", jce_editor_i18n_or("toast.automationDone",
                                            "Automation call finished"));
    jce_json_free(root);
}

/* Where the automation CLI lives.  Returns false when this editor has no
 * engine tree to run it from, which is a real configuration and not an error.
 *
 * THE PROJECT ROOT IS NOT THE ENGINE ROOT, and the first version of this file
 * assumed it was: it ran `private/tools/automation/automation_cli.py` with the working
 * directory set to the open project, which resolves only when the project IS
 * the repository.  With caged_kingdom/ open there is no caged_kingdom/tools/,
 * so every call would have failed to spawn -- and NO GATE COULD SEE IT.  The
 * ONE DOOR lint checks the call structure, the build compiles a wrong path
 * happily, and nothing in the suite starts an editor and opens a menu.  It
 * would have shipped as "automation is broken in the editor" with a message
 * about python.
 *
 * Walk up from the project first, then from the executable: that covers a
 * project nested in the tree, a project beside it, an in-tree build
 * (exe at build/.../release/) and an installed layout. */
bool automation_cli_dir(char *out, size_t cap)
{
    auto has_cli = [](const char *dir, char *buf, size_t n) {
        snprintf(buf, n, "%s/private/tools/automation/automation_cli.py", dir);
        return jce_fs_host_exists_file(buf);
    };
    char probe[1024];
    char dir[1024];

    const char *starts[2] = { jce_editor_current_project_root(), nullptr };
    char base[1024];
    if (jce_fs_host_get_base_path(base, (uint32_t)sizeof(base)))
        starts[1] = base;

    for (int s = 0; s < 2; ++s) {
        if (!starts[s] || !starts[s][0])
            continue;
        snprintf(dir, sizeof(dir), "%s", starts[s]);
        jce_path_canonicalise_inplace(dir);
        for (int up = 0; up < 8 && dir[0]; ++up) {
            size_t n = strlen(dir);
            while (n > 0 && dir[n - 1] == '/')
                dir[--n] = 0;
            if (has_cli(dir, probe, sizeof(probe))) {
                snprintf(out, cap, "%s", dir);
                return true;
            }
            char *slash = strrchr(dir, '/');
            if (!slash)
                break;
            *slash = 0;
        }
    }
    return false;
}

/* Finish a recipe pick: read the chosen file and start scene.compile on it.
 *
 * THE FILE IS PARSED HERE RATHER THAN HANDED OVER AS A PATH, for two reasons.
 * scene.compile takes a recipe OBJECT -- there is no path form -- and reading
 * it here means a file that is not a JSON object is refused with a message
 * naming the file, instead of arriving at the compiler as a schema violation
 * about a recipe the person never wrote. */
void poll_recipe_pick(void)
{
    if (s_call.pick_cancelled) {
        s_call.pick_dispatched = false;
        s_call.pick_cancelled = false;
        s_call.pick_ready = false;
        return;
    }
    if (!s_call.pick_ready)
        return;
    s_call.pick_dispatched = false;
    s_call.pick_ready = false;

    uint64_t size = 0;
    void *bytes = jce_fs_host_read_all(s_call.pick_path, &size);
    JceJson *recipe = bytes ? jce_json_parse((const char *)bytes,
                                             (size_t)size) : nullptr;
    if (bytes)
        jce_fs_buffer_free(bytes);
    if (!recipe || !jce_json_is_object(recipe)) {
        jce_json_free(recipe);
        LOG_ERROR(LOG_TAG, "%s is not a JSON object, so it is not a recipe: %s",
                  s_call.pick_path, jce_json_last_error());
        jce_toast_error("%s", jce_editor_i18n_or(
            "toast.automationBadRecipe",
            "That file is not a scene recipe (it must be a JSON object)"));
        return;
    }
    JceJson *args = jce_json_object();
    if (!args) {
        jce_json_free(recipe);
        return;
    }
    jce_json_set_child(args, "recipe", recipe);   /* args owns it now */
    char *text = jce_json_print(args, false);
    jce_json_free(args);
    if (!text)
        return;
    LOG_INFO(LOG_TAG, "compiling %s with the engine's own compiler",
             s_call.pick_path);
    jce_editor_automation_call("scene.compile", text);
    jce_json_free_string(text);
}

/* Start ONE step.  `cs` is the changeset id to run inside, or NULL.
 *
 * Every step in a plan comes through here, so the argument transport, the
 * --project flag and the spawn diagnostics are stated once.  The first
 * version had them only in the public entry point, which would have meant
 * writing them again for changeset.begin and again for commit -- and the
 * second copy is where a fix to the first fails to land. */
bool spawn_step(const char *tool, const char *json_args, const char *cs)
{
    const char *root = jce_editor_current_project_root();
    if (!root || !root[0]) {
        jce_toast_error("%s", jce_editor_i18n_or(
            "toast.automationNoProject",
            "Open a project first: the automation tools act on one"));
        return false;
    }
    char cli_dir[1024];
    if (!automation_cli_dir(cli_dir, sizeof(cli_dir))) {
        LOG_ERROR(LOG_TAG, "no private/tools/automation/automation_cli.py above the "
                           "project (%s) or the executable; the automation "
                           "tools need the engine tree", root);
        jce_toast_error("%s", jce_editor_i18n_or(
            "toast.automationNoCli",
            "The automation tools need the JCE engine tree and it was not "
            "found next to this editor"));
        return false;
    }

    /* ARGUMENTS GO THROUGH A FILE, NOT THE COMMAND LINE, and that is a
     * correctness requirement rather than a size convenience.  Three separate
     * silent truncations sat on the old path: this function's argv buffer,
     * Call::args, and split_args() in jce_process.c copying each token into a
     * char[1024].  A 2671-byte plan lost 1533 bytes with nothing logged.
     *
     * It also retires the quoting rule entirely.  The splitter toggles on any
     * unescaped quote, so JSON had to be wrapped in single quotes and was then
     * forbidden from containing one -- a constraint on the DATA imposed by the
     * transport.  A path has no quotes in it because we choose the path. */
    snprintf(s_call.args_file, sizeof(s_call.args_file),
             "%s/build/automation/editor-args-%llu-%u.json", cli_dir,
             (unsigned long long)jce_time_ticks_ns(), s_call.args_seq++);
    /* The args file PATH is the one thing still crossing the command line, so
     * it is the one thing still subject to split_args' quote toggling -- and a
     * project living under D:\My 'stuff'\ is an ordinary thing for someone to
     * have.  Refused rather than escaped: the splitter understands no escape,
     * so there is no encoding that survives, and a mangled path would arrive
     * as a wrong-but-plausible argument. */
    if (strchr(s_call.args_file, '\'') || strchr(s_call.args_file, '"')) {
        LOG_ERROR(LOG_TAG, "the engine tree path contains a quote, which the "
                           "argument splitter cannot carry: %s",
                  s_call.args_file);
        s_call.args_file[0] = 0;
        return false;
    }
    const char *payload = (json_args && json_args[0]) ? json_args : "{}";
    if (!jce_fs_host_write_all(s_call.args_file, payload, strlen(payload))) {
        LOG_ERROR(LOG_TAG, "could not write the arguments for %s to %s",
                  tool, s_call.args_file);
        s_call.args_file[0] = 0;
        return false;
    }

    /* --project IS NOT OPTIONAL and is absolute: the working directory is the
     * ENGINE root now, so without this flag the CLI would default to
     * $JCE_PROJECT or the repository root and act on the engine instead of the
     * project the person has open. */
    char argv[2400];
    if (cs && cs[0]) {
        snprintf(argv, sizeof(argv),
                 "private/tools/automation/automation_cli.py --project '%s' "
                 "--changeset %s call %s --no-interactive --json-file '%s'",
                 root, cs, tool, s_call.args_file);
    } else {
        snprintf(argv, sizeof(argv),
                 "private/tools/automation/automation_cli.py --project '%s' call %s "
                 "--no-interactive --json-file '%s'",
                 root, tool, s_call.args_file);
    }

    JceProcessConfig cfg = {};
    cfg.executable_path = python_name();
    cfg.working_directory = cli_dir;
    cfg.arguments = argv;
    cfg.capture_stdout = true;
    cfg.capture_stderr = true;

    s_call.proc = jce_process_spawn(&cfg);
    if (!s_call.proc) {
        /* Says WHICH interpreter and WHY: "it did not work" about a
         * subprocess is a support ticket rather than a diagnosis. */
        LOG_ERROR(LOG_TAG, "could not start %s in %s: %s",
                  python_name(), root, jce_process_get_last_spawn_error());
        jce_toast_error("%s", jce_editor_i18n_or(
            "toast.automationNoPython",
            "Could not start python for the automation tools"));
        return false;
    }
    if (s_call.out == nullptr)
        s_call.out = (char *)jce_malloc(AUTOMATION_OUT_MAX);
    if (!s_call.out) {
        call_reset();
        return false;
    }
    s_call.len = 0;
    s_call.truncated = false;
    snprintf(s_call.running, sizeof(s_call.running), "%s", tool);
    LOG_INFO(LOG_TAG, "%s: started%s%s", tool,
             (cs && cs[0]) ? " in changeset " : "", (cs && cs[0]) ? cs : "");
    return true;
}

} /* namespace */

extern "C" bool jce_editor_automation_is_running(void)
{
    return s_call.proc != nullptr;
}

extern "C" const char *jce_editor_automation_current_tool(void)
{
    /* THE STEP IN FLIGHT, not the tool the caller asked for.  A write
     * is four calls and the slow one is changeset.validate, which runs
     * a real lint; reporting "physics.save_thresholds" for the minutes
     * spent in validate would name the step that already finished. */
    return s_call.running[0] ? s_call.running : s_call.tool;
}

extern "C" bool jce_editor_automation_call(const char *tool,
                                           const char *json_args)
{
    if (!tool || !tool[0])
        return false;
    if (s_call.proc) {
        jce_toast_warn("%s", jce_editor_i18n_or(
            "toast.automationBusy",
            "An automation call is already running"));
        return false;
    }
    snprintf(s_call.tool, sizeof(s_call.tool), "%s", tool);
    if (!call_set_args(json_args))
        return false;
    s_call.title[0] = 0;
    s_call.changeset[0] = 0;
    s_call.writing = false;
    s_call.step = STEP_TOOL;
    if (!spawn_step(tool, s_call.args, nullptr)) {
        call_reset();
        return false;
    }
    return true;
}

extern "C" bool jce_editor_automation_write(const char *tool,
                                            const char *json_args,
                                            const char *title)
{
    if (!tool || !tool[0] || !title || !title[0])
        return false;
    if (s_call.proc) {
        jce_toast_warn("%s", jce_editor_i18n_or(
            "toast.automationBusy",
            "An automation call is already running"));
        return false;
    }
    snprintf(s_call.tool, sizeof(s_call.tool), "%s", tool);
    if (!call_set_args(json_args))
        return false;
    snprintf(s_call.title, sizeof(s_call.title), "%s", title);
    s_call.changeset[0] = 0;
    s_call.writing = true;
    s_call.step = STEP_BEGIN;

    char begin[256];
    snprintf(begin, sizeof(begin), "{\"title\": \"%s\"}", title);
    if (!spawn_step("changeset.begin", begin, nullptr)) {
        call_reset();
        return false;
    }
    return true;
}

/* Did this step succeed?  Separate from report_envelope, which SHOWS a result
 * to a person; this decides whether the plan may continue.
 *
 * TWO `ok` FIELDS, AND THEY ARE NOT THE SAME QUESTION.  The envelope's `ok`
 * says the CALL was accepted -- arguments valid, capability allowed, tool ran.
 * Some results carry their OWN `ok`, and it says whether the thing the tool
 * was asked to judge passed.
 *
 * MEASURED: changeset.validate on a tree whose lint is red returns
 * envelope ok:true with result {ok:false, failed_stage:"lint",
 * error_code:"LINT_FAILED"}.  Reading only the envelope, this plan would have
 * treated a FAILED validation as success, gone to commit, and been refused
 * with "has not passed changeset.validate" -- a message about a step it had
 * just run and believed. */
static bool envelope_ok(const char *text, size_t len)
{
    JceJson *root = jce_json_parse(text, len);
    if (!root)
        return false;
    bool ok = jce_json_get_bool(root, "ok", false);
    if (ok) {
        JceJson *res = jce_json_get(root, "result");
        /* A result with no `ok` of its own is not a failure: most tools do not
         * carry one, and defaulting to false would make every successful call
         * look refused. */
        if (res && jce_json_get(res, "ok"))
            ok = jce_json_get_bool(res, "ok", true);
    }
    jce_json_free(root);
    return ok;
}

/* Which tools produce a plan, and which key in their result names its subject.
 *
 * A TABLE RATHER THAN THREE `if`s, because the appliers read it too: the pair
 * "who may produce a plan" and "who may consume one" has to be stated once or
 * the two lists drift, and a drift here is not a crash -- every applier takes
 * an `object`, so a mismatched plan is a type-correct call that writes
 * nonsense into a scene. */
struct Planner { const char *tool; const char *subject_key; };
const Planner PLANNERS[] = {
    { "physics.author_plan",  "model" },
    { "physics.ragdoll_plan", "model" },
    /* The plan HASH, because that is the identity of a compiled plan: the
     * engine's own compiler produces it, and the same recipe and catalog
     * reproduce it exactly in any process.  A recipe path would name where it
     * came from; the hash names WHAT it is. */
    { "scene.compile",        "hash"  },
};

const Planner *planner_for(const char *tool)
{
    for (const Planner &p : PLANNERS)
        if (strcmp(p.tool, tool) == 0)
            return &p;
    return nullptr;
}

/* Keep a successful plan so the matching applier can write it.
 *
 * Kept as TEXT rather than re-derived: the applier takes the plan the person
 * actually reviewed, and re-running the planner to rebuild it would mean
 * applying a plan nobody read -- which is the whole of REQ-PHY-03 undone
 * quietly.
 *
 * Not called for other tools, and not called on failure: a retained plan is a
 * claim that there is something reviewable to write. */
static void capture_plan(const char *tool, const char *text, size_t len)
{
    const Planner *p = planner_for(tool);
    if (!p)
        return;
    JceJson *root = jce_json_parse(text, len);
    if (!root)
        return;
    JceJson *res = jce_json_get(root, "result");
    char *printed = res ? jce_json_print(res, false) : nullptr;
    if (printed) {
        if (s_call.plan)
            jce_free(s_call.plan);
        const size_t n = strlen(printed);
        s_call.plan = (char *)jce_malloc(n + 1);
        if (s_call.plan)
            memcpy(s_call.plan, printed, n + 1);
        snprintf(s_call.plan_tool, sizeof(s_call.plan_tool), "%s", tool);
        snprintf(s_call.plan_subject, sizeof(s_call.plan_subject), "%s",
                 jce_json_get_string(res, p->subject_key, ""));
        LOG_INFO(LOG_TAG, "%s kept a plan for %s (%zu bytes); review it above, "
                          "then the matching Tools item writes it",
                 tool, s_call.plan_subject[0] ? s_call.plan_subject : "(no "
                 "subject named)", n);
        jce_json_free_string(printed);
    }
    jce_json_free(root);
}

/* The kept plan, if it came from `want` and is about `subject`.
 *
 * ONE CHECK FOR EVERY APPLIER, so that adding a fourth planner cannot add a
 * third slightly different version of it.  `subject` may be NULL for a plan
 * whose subject the editor does not re-derive (a compiled scene plan is about
 * a recipe the person chose, not about the selection). */
static const char *kept_plan(const char *want, const char *subject)
{
    if (!s_call.plan || !s_call.plan[0] || strcmp(s_call.plan_tool, want) != 0) {
        jce_toast_warn("%s", jce_editor_i18n_or(
            "toast.automationNoPlan",
            "Plan it first; there is nothing to write"));
        return nullptr;
    }
    if (subject && strcmp(subject, s_call.plan_subject) != 0) {
        LOG_ERROR(LOG_TAG, "the kept plan is for %s but %s is selected; plan "
                           "again for this one", s_call.plan_subject, subject);
        jce_toast_warn("%s", jce_editor_i18n_or(
            "toast.automationPlanIsForAnother",
            "The plan is for a different model; plan again for this one"));
        return nullptr;
    }
    return s_call.plan;
}

/* The changeset id out of changeset.begin's envelope, into s_call. */
static bool capture_changeset_id(const char *text, size_t len)
{
    JceJson *root = jce_json_parse(text, len);
    if (!root)
        return false;
    JceJson *res = jce_json_get(root, "result");
    const char *id = res ? jce_json_get_string(res, "id", "") : "";
    const bool got = id && id[0];
    if (got)
        snprintf(s_call.changeset, sizeof(s_call.changeset), "%s", id);
    jce_json_free(root);
    return got;
}

/* Drive the plan one step.  Called once per completed process. */
static void advance(bool ok, int code)
{
    (void)code;
    const int finished = s_call.step;

    if (!s_call.writing) {
        if (!s_call.truncated && s_call.len > 0) {
            if (ok)
                capture_plan(s_call.tool, s_call.out, s_call.len);
            report_envelope(s_call.out, s_call.len);
        }
        call_reset();
        return;
    }

    switch (finished) {
    case STEP_BEGIN:
        if (!ok || !capture_changeset_id(s_call.out, s_call.len)) {
            /* NOTHING TO ROLL BACK: no changeset was opened, so going to
             * STEP_ROLLBACK would call it with no id and fail for a second,
             * unrelated reason -- which is the message the reader would see. */
            LOG_ERROR(LOG_TAG, "could not open a changeset for %s", s_call.tool);
            if (s_call.len > 0)
                report_envelope(s_call.out, s_call.len);
            jce_toast_error("%s", jce_editor_i18n_or(
                "toast.automationNoChangeset",
                "Could not open a changeset, so nothing was written"));
            call_reset();
            return;
        }
        s_call.step = STEP_TOOL;
        if (!spawn_step(s_call.tool, s_call.args, s_call.changeset))
            call_reset();
        return;

    case STEP_TOOL:
        if (!ok) {
            if (s_call.len > 0)
                report_envelope(s_call.out, s_call.len);
            LOG_WARN(LOG_TAG, "%s failed; rolling the changeset back",
                     s_call.tool);
            s_call.step = STEP_ROLLBACK;
            if (!spawn_step("changeset.rollback", "{}", s_call.changeset))
                call_reset();
            return;
        }
        report_envelope(s_call.out, s_call.len);
        /* THE WRITE STOPS HERE, WITH THE CHANGESET OPEN.  Not a shortcut --
         * it is the loop REQ-SCN-04 describes: the human previews in the
         * editor (the scene-file watcher shows what landed) and approves, and
         * the approval is the commit.
         *
         * WHY NOT COMMIT AUTOMATICALLY.  changeset.commit refuses an
         * unvalidated changeset (REQ-TXN-03), and changeset.validate runs a
         * REAL LINT -- minutes on this tree.  The two ways out were both
         * worse: committing automatically would freeze the one call slot for
         * minutes behind a menu item that looks instant, and validating with
         * skip_build/skip_tests would give an editor-made changeset a WEAKER
         * gate than a CLI-made one -- two standards for one operation, which
         * is the thing REQ-ARCH-02 exists to prevent.
         *
         * So the slow step is opt-in and explicit: jce_editor_automation_
         * commit_open() runs validate then commit, and the menu says what it
         * costs before you choose it. */
        snprintf(s_call.open_cs, sizeof(s_call.open_cs), "%s", s_call.changeset);
        LOG_INFO(LOG_TAG, "%s: written into changeset %s -- review, then "
                          "commit or roll back", s_call.tool, s_call.open_cs);
        jce_toast_info("%s", jce_editor_i18n_or(
            "toast.automationWritten",
            "Written into an open change; review it, then commit or roll back"));
        call_reset();
        return;

    case STEP_VALIDATE:
        if (!ok) {
            if (s_call.len > 0)
                report_envelope(s_call.out, s_call.len);
            LOG_WARN(LOG_TAG, "validate refused; rolling the changeset back");
            s_call.step = STEP_ROLLBACK;
            if (!spawn_step("changeset.rollback", "{}", s_call.changeset))
                call_reset();
            return;
        }
        s_call.step = STEP_COMMIT;
        {
            char msg[320];
            snprintf(msg, sizeof(msg), "{\"message\": \"%s\"}", s_call.title);
            if (!spawn_step("changeset.commit", msg, s_call.changeset))
                call_reset();
        }
        return;

    case STEP_COMMIT:
        if (!ok) {
            /* COMMIT CAN REFUSE: changeset.commit runs only after validate
             * passes (REQ-TXN-03).  The bytes are written but unrecorded, so
             * rolling back is the only state a reader can reason about. */
            if (s_call.len > 0)
                report_envelope(s_call.out, s_call.len);
            LOG_WARN(LOG_TAG, "commit refused; rolling back");
            s_call.step = STEP_ROLLBACK;
            if (!spawn_step("changeset.rollback", "{}", s_call.changeset))
                call_reset();
            return;
        }
        LOG_INFO(LOG_TAG, "%s: committed as \"%s\"", s_call.tool, s_call.title);
        s_call.open_cs[0] = 0;
        jce_toast_success("%s", jce_editor_i18n_or(
            "toast.automationCommitted",
            "Change committed; Ctrl+Z does not undo it -- use changeset.rollback"));
        call_reset();
        return;

    case STEP_ROLLBACK:
    default:
        if (!ok) {
            /* The worst outcome, and it must be loud: the write happened and
             * the undo for it did not. */
            LOG_ERROR(LOG_TAG, "ROLLBACK FAILED for changeset %s -- the "
                               "project may be half-changed", s_call.changeset);
            jce_toast_error("%s", jce_editor_i18n_or(
                "toast.automationRollbackFailed",
                "Rollback failed; the project may be half-changed"));
        } else {
            s_call.open_cs[0] = 0;
            jce_toast_warn("%s", jce_editor_i18n_or(
                "toast.automationRolledBack",
                "The change was rolled back; nothing was written"));
        }
        call_reset();
        return;
    }
}

extern "C" void jce_editor_automation_poll(void)
{
    if (s_call.pick_dispatched)
        poll_recipe_pick();
    if (!s_call.proc)
        return;

    /* DRAIN BEFORE POLLING FOR EXIT.  A child that has exited may still have
     * bytes in the pipe, and reading after the reap loses them -- which shows
     * up as an empty envelope from a call that worked. */
    automation_drain_stdout();
    char buf[4096];
    for (;;) {
        size_t got = jce_process_read_stderr(s_call.proc, buf, sizeof(buf));
        if (got == 0)
            break;
        LOG_WARN(LOG_TAG, "%s: %.*s", s_call.tool, (int)got, buf);
    }

    int code = 0;
    if (!jce_process_poll_exit(s_call.proc, &code))
        return;

    /* AND ONCE MORE AFTER THE REAP.  The drain above and this poll are two
     * separate calls; anything the child wrote between them is still in the
     * pipe, and the first version of this function stopped at the drain --
     * the comment above described the hazard and the code covered half of it.
     * A truncated envelope reads as "not JSON" from a call that worked. */
    automation_drain_stdout();

    const bool ok = (code == 0) && !s_call.truncated && s_call.len > 0
                    && envelope_ok(s_call.out, s_call.len);
    if (s_call.truncated) {
        LOG_ERROR(LOG_TAG, "%s wrote more than %d bytes; the reply was cut off "
                           "and is not being parsed", s_call.running,
                  (int)AUTOMATION_OUT_MAX);
    } else if (s_call.len == 0) {
        LOG_ERROR(LOG_TAG, "%s exited %d and wrote nothing", s_call.running,
                  code);
    }
    LOG_INFO(LOG_TAG, "%s: exit %d", s_call.running, code);

    /* Destroy this step's process before the next one starts: spawn_step
     * assigns s_call.proc, and leaking the previous handle would leave a
     * reaped child undestroyed for the life of the editor. */
    jce_process_destroy(s_call.proc);
    s_call.proc = nullptr;

    advance(ok, code);
}

extern "C" const char *jce_editor_automation_open_change(void)
{
    return s_call.open_cs;
}

extern "C" bool jce_editor_automation_commit_open(void)
{
    /* THE SLOW STEP, CHOSEN RATHER THAN IMPOSED.  validate runs a real lint
     * (minutes on this tree) and commit refuses without it, so this is the
     * one action that says so in the menu before you pick it. */
    if (!s_call.open_cs[0]) {
        jce_toast_warn("%s", jce_editor_i18n_or(
            "toast.automationNoOpenChange",
            "There is no open change to commit"));
        return false;
    }
    if (s_call.proc) {
        jce_toast_warn("%s", jce_editor_i18n_or(
            "toast.automationBusy",
            "An automation call is already running"));
        return false;
    }
    snprintf(s_call.tool, sizeof(s_call.tool), "%s", "changeset.commit");
    snprintf(s_call.changeset, sizeof(s_call.changeset), "%s", s_call.open_cs);
    snprintf(s_call.title, sizeof(s_call.title), "%s",
             "commit the change made from the editor");
    s_call.args[0] = 0;
    s_call.writing = true;
    s_call.step = STEP_VALIDATE;
    if (!spawn_step("changeset.validate", "{}", s_call.changeset)) {
        call_reset();
        return false;
    }
    return true;
}

extern "C" bool jce_editor_automation_rollback_open(void)
{
    if (!s_call.open_cs[0]) {
        jce_toast_warn("%s", jce_editor_i18n_or(
            "toast.automationNoOpenChange",
            "There is no open change to roll back"));
        return false;
    }
    if (s_call.proc) {
        jce_toast_warn("%s", jce_editor_i18n_or(
            "toast.automationBusy",
            "An automation call is already running"));
        return false;
    }
    snprintf(s_call.tool, sizeof(s_call.tool), "%s", "changeset.rollback");
    snprintf(s_call.changeset, sizeof(s_call.changeset), "%s", s_call.open_cs);
    s_call.args[0] = 0;
    s_call.writing = true;
    s_call.step = STEP_ROLLBACK;
    if (!spawn_step("changeset.rollback", "{}", s_call.changeset)) {
        call_reset();
        return false;
    }
    return true;
}

/* The open scene as a path the CLI will accept, or false with a toast saying
 * why.  Every tool that names the open scene needs exactly this, so it is one
 * function: physics.probe and physics.author_apply both call it, and a second
 * copy is where a fix to the first would fail to land.
 *
 * PROJECT-RELATIVE, BECAUSE AN ABSOLUTE PATH DOES NOT CROSS THE PROTOCOL.
 * Measured against the CLI: an absolute scene is refused outright with
 * "absolute paths do not cross the protocol".  The editor's own scene path is
 * absolute whenever the scene was opened through a file dialog, so passing it
 * through unchanged would fail on the most ordinary way of opening a scene.
 * jce_editor_path_to_relative_to is the editor's existing converter; this does
 * not add a second one. */
static bool scene_relpath(char *out, size_t cap)
{
    const char *scene = jce_state_get_current_scene_path();
    if (!scene || !scene[0] || strncmp(scene, "bundle://", 9) == 0) {
        jce_toast_warn("%s", jce_editor_i18n_or(
            "toast.automationNoScene",
            "Save the scene to a file first: these tools act on a file"));
        return false;
    }
    const char *proj = jce_editor_current_project_root();
    char rel[1024];
    jce_editor_path_to_relative_to(rel, sizeof(rel), scene, proj);
    if (!rel[0] || jce_path_is_absolute(rel)) {
        /* Outside the project: the tool could not reach it, and saying so
         * beats sending a path it will refuse for a reason that reads like a
         * protocol complaint. */
        LOG_ERROR(LOG_TAG, "the open scene is not inside the project (%s vs "
                           "%s), so the tools cannot address it", scene, proj);
        jce_toast_error("%s", jce_editor_i18n_or(
            "toast.automationSceneOutside",
            "The open scene is outside this project, so the automation tools "
            "cannot address it"));
        return false;
    }
    snprintf(out, cap, "%s", rel);
    return true;
}

/* The model path of the one selected entity, or NULL with a toast saying why.
 *
 * ONE entity, not the first of several: physics.author_plan describes ONE
 * model, and silently planning for whichever happened to be selected first
 * would produce a plan about something the author did not point at. */
static const char *selected_model_path(void)
{
    int count = 0;
    const uint32_t *sel = jce_state_get_selection(&count);
    if (!sel || count != 1) {
        jce_toast_warn("%s", jce_editor_i18n_or(
            "toast.automationSelectOne",
            "Select exactly one entity with a model first"));
        return nullptr;
    }
    JceScene *scene = jce_state_get_scene();
    if (!scene)
        return nullptr;
    JceEntity e = jce_state_to_ecs_entity(sel[0]);
    if (!jce_scene_has_mesh_renderer(scene, e)) {
        jce_toast_warn("%s", jce_editor_i18n_or(
            "toast.automationNoModel",
            "That entity has no MeshRenderer, so there is no model to author "
            "colliders from"));
        return nullptr;
    }
    const JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
    if (!mr || !mr->mesh_path || !mr->mesh_path[0]) {
        jce_toast_warn("%s", jce_editor_i18n_or(
            "toast.automationNoModel",
            "That entity has no MeshRenderer, so there is no model to author "
            "colliders from"));
        return nullptr;
    }
    return mr->mesh_path;
}

extern "C" bool jce_editor_automation_author_plan(void)
{
    /* READ-ONLY, and it is the half that has never been visible in the editor.
     * REQ-PHY-03 requires the automatic collider choice to come with its
     * REASON -- mesh convexity, triangle count, intended use -- so a person
     * can review it.  The Console prints the reasons; nothing is written. */
    const char *model = selected_model_path();
    if (!model)
        return false;

    /* BUILT, NOT PRINTED.  A model path is user data: snprintf("\"%s\"")
     * produces invalid JSON the moment it contains a backslash or a quote,
     * and on Windows a backslash is the ordinary case.  jce_json_set_string
     * escapes; a format string does not. */
    JceJson *args = jce_json_object();
    if (!args)
        return false;
    jce_json_set_string(args, "model", model);
    char *text = jce_json_print(args, false);
    jce_json_free(args);
    if (!text)
        return false;
    const bool started = jce_editor_automation_call("physics.author_plan", text);
    jce_json_free_string(text);
    return started;
}

/* Start `tool` with {scene, plan, ...extra} built from the kept plan.
 *
 * THE ONE APPLY PATH.  physics.author_apply, physics.ragdoll_apply and
 * scene.materialise differ only in which planner they accept and what they add
 * beside the plan; writing that three times would put the plan-identity check,
 * the scene resolution and the JSON construction in three places, and the
 * copies that matter are always the ones a later fix misses.
 *
 * Takes ownership of `extra` (or NULL). */
static bool apply_kept_plan(const char *planner, const char *tool,
                            const char *subject, const char *title,
                            JceJson *extra)
{
    const char *kept = kept_plan(planner, subject);
    if (!kept) {
        jce_json_free(extra);
        return false;
    }
    char scene[1024];
    if (!scene_relpath(scene, sizeof(scene))) {
        jce_json_free(extra);
        return false;
    }
    JceJson *plan = jce_json_parse(kept, 0);
    JceJson *args = plan ? jce_json_object() : nullptr;
    if (!args) {
        jce_json_free(plan);
        jce_json_free(extra);
        return false;
    }
    jce_json_set_string(args, "scene", scene);
    jce_json_set_child(args, "plan", plan);   /* args owns plan from here */
    /* Move the caller's extra members across rather than copying the object:
     * jce_json_detach is the tree's move primitive and exists so a relocation
     * needs no print/re-parse round trip. */
    if (extra) {
        JceJson *c = jce_json_first_child(extra);
        while (c) {
            JceJson *next = jce_json_next_sibling(c);
            const char *key = jce_json_member_key(c);
            jce_json_detach(extra, c);
            if (key)
                jce_json_set_child(args, key, c);
            else
                jce_json_free(c);
            c = next;
        }
        jce_json_free(extra);
    }
    char *text = jce_json_print(args, false);
    jce_json_free(args);
    if (!text)
        return false;
    /* Through _write, so it gets the changeset transaction: begin, the tool,
     * then STOP with the change open for review.  These all write a scene, and
     * a scene write is exactly what should be looked at before it is kept. */
    const bool started = jce_editor_automation_write(tool, text, title);
    jce_json_free_string(text);
    return started;
}

extern "C" bool jce_editor_automation_author_apply(void)
{
    /* The WRITING half, and the one the editor could not do at all.  It takes
     * the plan the person just reviewed -- not a freshly computed one, which
     * would mean writing something nobody read. */
    const char *model = selected_model_path();
    if (!model)
        return false;
    return apply_kept_plan("physics.author_plan", "physics.author_apply",
                           model, "editor: author colliders from a plan",
                           nullptr);
}

/* A minimal script that actually RUNS, per language.
 *
 * TWO LANGUAGES, AND THE OMISSION IS DELIBERATE.  script.write accepts seven
 * (lua, python, javascript, csharp, java, c, cpp), and this tree contains a
 * working example of exactly two.  A template for the other five would be
 * derived from a header comment rather than from anything observed to run,
 * and a script template that is subtly wrong is the "looks configured, does
 * nothing" failure this codebase has paid for repeatedly -- a ragdoll whose
 * seven spawn conditions each failed silently, four WheelColliders nobody
 * read.  Offering a language here is a claim that a script written this way
 * works; that claim needs evidence, and for five of them there is none.
 *
 * The shapes are copied from the tree's own examples rather than invented:
 * resources/assets/scripts/bob.lua and tests/sdk_smoke_scripting's smoke_py.py.
 * A Lua script is a module table whose METHODS take self; a Python script is a
 * module whose top-level FUNCTIONS take self.  The difference is real and
 * getting it backwards produces a script the host loads and never calls. */
struct ScriptTemplate { const char *lang, *ext, *body; };
const ScriptTemplate SCRIPT_TEMPLATES[] = {
    { "lua", "lua",
      "-- A gameplay script.  Attach it with a Script component (Inspector ->\n"
      "-- Add Component -> Script) and set the script path to this file.\n"
      "--\n"
      "-- The runtime instantiates this module PER ENTITY, calls on_start once\n"
      "-- and on_update every frame.  Per-instance state lives on `self`;\n"
      "-- `self.entity` is the owning entity id.\n"
      "\n"
      "local M = {}\n"
      "\n"
      "function M:on_start()\n"
      "    self.t = 0.0\n"
      "    jce.log(\"script: start on entity \" .. tostring(self.entity))\n"
      "end\n"
      "\n"
      "function M:on_update(dt)\n"
      "    self.t = (self.t or 0.0) + dt\n"
      "end\n"
      "\n"
      "return M\n" },
    { "python", "py",
      "# A gameplay script.  Attach it with a Script component (Inspector ->\n"
      "# Add Component -> Script) and set the script path to this file.\n"
      "#\n"
      "# A Python gameplay script is a MODULE whose top-level functions take\n"
      "# `self`, the instance -- the same shape a Lua script's methods have.\n"
      "# `jce` is injected into the module namespace by jce_script/vm.py.\n"
      "\n"
      "\n"
      "def on_start(self):\n"
      "    self.t = 0.0\n"
      "    jce.log(\"script: start\")\n"
      "\n"
      "\n"
      "def on_update(self, dt):\n"
      "    self.t = getattr(self, \"t\", 0.0) + dt\n" },
};

extern "C" int jce_editor_automation_script_language_count(void)
{
    return (int)(sizeof(SCRIPT_TEMPLATES) / sizeof(SCRIPT_TEMPLATES[0]));
}

extern "C" const char *jce_editor_automation_script_language(int i)
{
    if (i < 0 || i >= jce_editor_automation_script_language_count())
        return "";
    return SCRIPT_TEMPLATES[i].lang;
}

extern "C" bool jce_editor_automation_script_new(const char *lang)
{
    const ScriptTemplate *t = nullptr;
    for (const ScriptTemplate &c : SCRIPT_TEMPLATES)
        if (lang && strcmp(c.lang, lang) == 0)
            t = &c;
    if (!t)
        return false;
    const char *root = jce_editor_current_project_root();
    if (!root || !root[0]) {
        jce_toast_error("%s", jce_editor_i18n_or(
            "toast.automationNoProject",
            "Open a project first: the automation tools act on one"));
        return false;
    }

    /* A FRESH NAME, NOT A FIXED ONE.  The Assets browser's own "New Script"
     * writes NewScript.c unconditionally, so using it twice in a folder
     * replaces the first one with a stub and says "Created" both times.  The
     * changeset would make that undoable, which is better than today -- but
     * not overwriting is better than undoing. */
    char rel[512];
    char host[1024];
    int n = 0;
    for (;;) {
        if (n == 0)
            snprintf(rel, sizeof(rel), "resources/assets/scripts/NewScript.%s",
                     t->ext);
        else
            snprintf(rel, sizeof(rel), "resources/assets/scripts/NewScript%d.%s",
                     n, t->ext);
        snprintf(host, sizeof(host), "%s/%s", root, rel);
        if (!jce_fs_host_exists_file(host))
            break;
        if (++n > 999) {
            LOG_ERROR(LOG_TAG, "a thousand NewScript files already exist in "
                               "resources/assets/scripts/");
            return false;
        }
    }

    JceJson *args = jce_json_object();
    if (!args)
        return false;
    jce_json_set_string(args, "path", rel);
    jce_json_set_string(args, "lang", t->lang);
    jce_json_set_string(args, "content", t->body);
    char *text = jce_json_print(args, false);
    jce_json_free(args);
    if (!text)
        return false;
    char title[200];
    snprintf(title, sizeof(title), "editor: new %s script %s", t->lang, rel);
    const bool started = jce_editor_automation_write("script.write", text,
                                                     title);
    jce_json_free_string(text);
    return started;
}

extern "C" bool jce_editor_automation_compile_recipe(void)
{
    /* READ-ONLY, and the entry to a chain the editor has never had: a recipe
     * is compiled by THE ENGINE'S OWN compiler (scene.compile shells to the
     * bridge), so what the editor shows and what a headless run produces are
     * the same plan with the same hash rather than two compilers that agree
     * until they do not. */
    if (s_call.proc || s_call.pick_dispatched) {
        jce_toast_warn("%s", jce_editor_i18n_or(
            "toast.automationBusy",
            "An automation call is already running"));
        return false;
    }
    s_call.pick_ready = false;
    s_call.pick_cancelled = false;
    s_call.pick_path[0] = 0;
    s_call.pick_dispatched = true;
    open_file_dialog_async(
        jce_editor_i18n_or("menu.tools.compileRecipe",
                           "Compile a scene recipe..."),
        jce_editor_current_project_root(),
        "JSON files\0*.json\0All files\0*\0",
        s_call.pick_path, sizeof(s_call.pick_path),
        &s_call.pick_ready, &s_call.pick_cancelled);
    return true;
}

extern "C" bool jce_editor_automation_materialise(void)
{
    /* No subject check: a compiled plan is about the recipe the person chose,
     * not about the selection, so there is nothing here to compare against
     * that would not be invented.  The plan_tool check still applies, and it
     * is the one that matters -- every applier takes an `object`, so feeding
     * this a collider plan would be a type-correct call writing nonsense. */
    return apply_kept_plan("scene.compile", "scene.materialise", nullptr,
                           "editor: materialise a compiled scene plan",
                           nullptr);
}

extern "C" bool jce_editor_automation_ragdoll_plan(void)
{
    /* READ-ONLY.  Reports the capsule radius and height scale WITH the bone
     * pair that bounds them -- the same "with the reason" requirement the
     * collider planner carries, and for the same purpose: a ragdoll that
     * behaves oddly is almost never a solver problem, it is a proportion
     * somebody could have seen. */
    const char *model = selected_model_path();
    if (!model)
        return false;
    JceJson *args = jce_json_object();
    if (!args)
        return false;
    jce_json_set_string(args, "model", model);
    char *text = jce_json_print(args, false);
    jce_json_free(args);
    if (!text)
        return false;
    const bool started = jce_editor_automation_call("physics.ragdoll_plan",
                                                    text);
    jce_json_free_string(text);
    return started;
}

extern "C" bool jce_editor_automation_ragdoll_apply(void)
{
    /* Writes the Ragdoll component AND the SkeletalAnimator without which it
     * never spawns -- which is why this goes through the tool rather than the
     * Inspector: the pair is the unit, and adding one of the two by hand
     * produces an entity that reads as configured and does nothing.  Measured
     * in this tree already: a ragdoll whose seven spawn conditions each failed
     * silently, and a vehicle whose four WheelColliders were never read. */
    const char *model = selected_model_path();
    if (!model)
        return false;

    /* BY NAME, NOT BY THE SELECTION ID, and that is a correctness point.
     * jce_state_to_ecs_entity() resolves an editor id as a scene INDEX
     * (jce_scene_entity_from_index), while the tool's `entity` is a scene
     * DOCUMENT reference -- the "id" field, 1001, 1002 in a real scene.  The
     * two are different numbers that are both small integers, so passing the
     * wrong one authors a ragdoll onto a plausible-looking wrong entity and
     * nothing anywhere reports it.
     *
     * The name is a reference both sides already agree on, and the tool's
     * find_entity REFUSES an ambiguous name rather than taking the first
     * match -- which is a better answer than any disambiguation the editor
     * could invent here. */
    int count = 0;
    const uint32_t *sel = jce_state_get_selection(&count);
    if (!sel || count != 1)
        return false;   /* selected_model_path already said why */
    JceScene *scene = jce_state_get_scene();
    const char *name = scene ? jce_scene_entity_name(
        scene, jce_state_to_ecs_entity(sel[0])) : nullptr;
    if (!name || !name[0]) {
        LOG_ERROR(LOG_TAG, "the selected entity has no name, and a name is "
                           "how the scene document is addressed");
        jce_toast_warn("%s", jce_editor_i18n_or(
            "toast.automationEntityUnnamed",
            "Give the entity a name first: that is how the scene file "
            "addresses it"));
        return false;
    }
    JceJson *extra = jce_json_object();
    if (!extra)
        return false;
    jce_json_set_string(extra, "entity", name);
    return apply_kept_plan("physics.ragdoll_plan", "physics.ragdoll_apply",
                           model, "editor: author a ragdoll from a plan",
                           extra);
}

extern "C" bool jce_editor_automation_pin_thresholds(void)
{
    /* REQ-ARCH-02's first WRITING call site, and REQ-PHY-02's other half made
     * reachable: the probe thresholds a project is judged by are defaults
     * until somebody pins them, and a default can move under a project that
     * never asked it to.  physics.save_thresholds writes them into
     * jce_project.json so they travel with the project.
     *
     * NO EDIT SCOPE, DELIBERATELY.  This writes a FILE.  The editor's undo
     * history owns the in-memory scene between saves; the changeset owns bytes
     * on disk between commits, and the undo for this is changeset.rollback.
     * jce_state_begin_batch_edit here would push an undo record for an
     * in-memory change that is not happening -- a Ctrl+Z that appears to
     * offer something it cannot deliver. */
    char args[512];
    snprintf(args, sizeof(args),
             "{\"thresholds\": {\"max_penetration_m\": %.5f, "
             "\"max_nan_events\": %d, \"settle_within_s\": %.2f, "
             "\"max_jitter_speed_m_per_s\": %.3f, "
             "\"max_energy_drift_ratio\": %.3f, \"max_tilt_deg\": %.1f}}",
             0.10, 0, 10.0, 0.25, 0.10, 60.0);
    return jce_editor_automation_write(
        "physics.save_thresholds", args,
        "pin physics probe thresholds to the project");
}

extern "C" bool jce_editor_automation_probe_current_scene(void)
{
    char rel[1024];
    if (!scene_relpath(rel, sizeof(rel)))
        return false;
    /* Built rather than printed: a scene path is user data and a format
     * string does not escape it.  See the note in _author_plan. */
    JceJson *args = jce_json_object();
    if (!args)
        return false;
    jce_json_set_string(args, "scene", rel);
    char *text = jce_json_print(args, false);
    jce_json_free(args);
    if (!text)
        return false;
    const bool started = jce_editor_automation_call("physics.probe", text);
    jce_json_free_string(text);
    return started;
}
