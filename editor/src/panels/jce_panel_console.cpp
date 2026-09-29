/*
 * jce_panel_console.cpp  Console panel (log output with filtering).
 * Extracted from jce_editor_panels.cpp.
 *
 * The console shows two interleaved streams:
 *   - editor events, kept in the ring buffer in jce_editor_panels.cpp and
 *     read through the iteration API (jce_editor_console_entry_count/get);
 *   - the engine's own structured log, mirrored here through the jce_log
 *     sink (see the "engine log bridge" section below).
 */

#include "ui/jce_editor_colors.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_assert_bridge.h"
#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "ui/jce_editor_ui_state.h"
#include "viewers/jce_file_viewer.h"

#include <jce/tools/jce_imgui.hpp>
#include <jce/os/core/jce_console.h>
#include <jce/os/core/jce_console_session.h>   /* cvar + command registry */
#include <jce/os/core/jce_log.h>       /* engine log stream + sink hook */
#include <jce/os/core/jce_thread.h>    /* JceMutex for the sink hand-off */
#include <jce/os/core/jce_timer.h>     /* local-time formatting */
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>
#include <unordered_map>

/* ── Console UI state (filter / scroll / selection) ───────────────── */

struct ConsoleUiState {
    bool          auto_scroll    = true;
    bool          show_info      = true;
    bool          show_warning   = true;
    bool          show_error     = true;
    bool          show_debug     = false;
    bool          clear_on_play  = false;
    bool          collapse       = false;
    bool          initialized    = false;
    JcePlayState  last_play      = JCE_PLAY_STOPPED;
    char          search_buf[128] = {0};
    char          cmd_buf[256]    = {0};
    /* Line / history / completion, shared in KIND with the shipped game's
     * overlay -- both drive a jce_console_session, so a fix to Tab or to the
     * history walk lands in both.  Not shared in INSTANCE: history is per
     * surface.  It lives in this struct rather than beside it because a
     * second file-scope mutable global is a real cost the dedup audit counts,
     * and this panel already has exactly one. */
    JceConsoleSession *session = nullptr;
    std::set<int> selected;   /* entry indices */
    int           anchor      = -1;
};

static ConsoleUiState s_ui;

static bool ascii_contains_ci(const char *hay, const char *needle)
{
    if (!needle || !*needle) return true;
    if (!hay) return false;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            ++i;
        if (i == nl) return true;
    }
    return false;
}

static bool entry_passes_filter(const JceConsoleEntry &e)
{
    bool show = false;
    switch (e.level) {
    case JCE_CONSOLE_INFO:    show = s_ui.show_info;    break;
    case JCE_CONSOLE_WARNING: show = s_ui.show_warning; break;
    case JCE_CONSOLE_ERROR:   show = s_ui.show_error;   break;
    case JCE_CONSOLE_DEBUG:   show = s_ui.show_debug;   break;
    }
    if (!show) return false;
    if (s_ui.search_buf[0] && !ascii_contains_ci(e.text, s_ui.search_buf))
        return false;
    return true;
}

/* ── Engine log bridge (jce_log → this panel) ─────────────────────────
 *
 * jce_log is the engine's one structured logger.  This panel subscribes to it
 * so engine output lands in the Console next to the editor's own entries,
 * instead of only in stderr and the log file.
 *
 * The sink fires on jce_log's backend thread, so it must not touch ImGui or
 * the display ring.  All it does is copy the record into `pending` under a
 * mutex; console_engine_drain() moves that into the main-thread-only `view`
 * ring once per content pass, and console_build_rows() merges `view` with the
 * editor console ring by timestamp.
 *
 * Severity mapping — jce_log has six levels, the console four.  Nothing is
 * dropped; the two pairs that would share a colour are folded:
 *     JCE_LOG_LEVEL_TRACE   ─┐
 *     JCE_LOG_LEVEL_DEBUG   ─┴→ JCE_CONSOLE_DEBUG    (hidden unless "Debug")
 *     JCE_LOG_LEVEL_INFO    ─┐
 *     JCE_LOG_LEVEL_SUCCESS ─┴→ JCE_CONSOLE_INFO
 *     JCE_LOG_LEVEL_WARN     → JCE_CONSOLE_WARNING
 *     JCE_LOG_LEVEL_ERROR    → JCE_CONSOLE_ERROR
 *     JCE_LOG_LEVEL_OFF      → never emitted by jce_log
 * The console has no level of its own that jce_log lacks, so the mapping is
 * total in both directions.
 *
 * Engine records are deliberately NOT pushed through
 * jce_editor_console_log_level(): that entry point also raises a toast, and
 * one toast per engine warning would evict the editor's own notifications
 * (8 slots, 4 s each).  Editor events keep their existing path and this
 * bridge stays purely additive.
 */

#define CONSOLE_ENGINE_LINE_LEN 256
#define CONSOLE_ENGINE_PENDING  256   /* power of two: sink → main hand-off */
#define CONSOLE_ENGINE_VIEW     512   /* power of two: what the panel shows */

struct EngineLogLine {
    char            text[CONSOLE_ENGINE_LINE_LEN];
    char            timestamp[24];
    JceConsoleLevel level;
};

static struct {
    JceMutex     *mtx;      /* guards pending / head / tail / dropped   */
    EngineLogLine pending[CONSOLE_ENGINE_PENDING];
    unsigned      head;     /* advanced by the sink (log thread)        */
    unsigned      tail;     /* advanced by the drain (main thread)      */
    int           dropped;  /* pending overflows since the last drain   */

    EngineLogLine view[CONSOLE_ENGINE_VIEW];   /* main thread only */
    unsigned      view_head;
    int           view_count;
} s_engine;

/* Fill in a line's timestamp in the same format the editor console ring uses,
 * so the two streams sort against each other lexicographically. */
static void engine_line_stamp(EngineLogLine *l, int64_t epoch_s)
{
    if (jce_time_format_local(epoch_s, "%Y-%m-%d %H:%M:%S",
                              l->timestamp, sizeof(l->timestamp)) == 0)
        snprintf(l->timestamp, sizeof(l->timestamp), "----------  --:--:--");
}

/* jce_log sink.  Runs on the log backend thread: copy only, no ImGui, no
 * allocation, no jce_log_* re-entry. */
static void console_log_sink(const JceLogRecord *rec, void *user)
{
    (void)user;
    if (!rec || !s_engine.mtx) return;

    /* Entries that ORIGINATED in the editor Console (jce_editor_console_log*)
     * are mirrored into jce_log so they reach the log file, and they carry
     * this tag.  They are already in the editor ring, so taking them again
     * here would show every editor message twice. */
    if (rec->tag && strcmp(rec->tag, kEditorConsoleLogTag) == 0) return;

    JceConsoleLevel level;
    switch (rec->level) {
    case JCE_LOG_LEVEL_TRACE:
    case JCE_LOG_LEVEL_DEBUG: level = JCE_CONSOLE_DEBUG;   break;
    case JCE_LOG_LEVEL_WARN:  level = JCE_CONSOLE_WARNING; break;
    case JCE_LOG_LEVEL_ERROR: level = JCE_CONSOLE_ERROR;   break;
    default:                  level = JCE_CONSOLE_INFO;    break; /* INFO, SUCCESS */
    }

    jce_mutex_lock(s_engine.mtx);
    if (s_engine.head - s_engine.tail >= CONSOLE_ENGINE_PENDING) {
        s_engine.dropped++;   /* panel not drawn, or a burst between frames */
    } else {
        EngineLogLine *l = &s_engine.pending[s_engine.head & (CONSOLE_ENGINE_PENDING - 1)];
        l->level = level;
        /* The engine's own line shape minus the parts the console renders
         * itself (timestamp column, level prefix). */
        snprintf(l->text, sizeof(l->text), "[%s] %s: %s at %s:%d",
                 rec->thread_name, rec->tag, rec->message, rec->file, rec->line);
        engine_line_stamp(l, rec->wall_epoch_s);
        s_engine.head++;
    }
    jce_mutex_unlock(s_engine.mtx);
}

/* Append to the display ring.  Main thread only. */
static void console_engine_view_push(const EngineLogLine *l)
{
    s_engine.view[s_engine.view_head & (CONSOLE_ENGINE_VIEW - 1)] = *l;
    s_engine.view_head++;
    if (s_engine.view_count < CONSOLE_ENGINE_VIEW)
        s_engine.view_count++;
}

/* Move everything the sink queued into the display ring.  Main thread only. */
static void console_engine_drain(void)
{
    if (!s_engine.mtx) return;

    jce_mutex_lock(s_engine.mtx);
    while (s_engine.head != s_engine.tail) {
        console_engine_view_push(&s_engine.pending[s_engine.tail & (CONSOLE_ENGINE_PENDING - 1)]);
        s_engine.tail++;
    }
    int dropped = s_engine.dropped;
    s_engine.dropped = 0;
    jce_mutex_unlock(s_engine.mtx);

    if (dropped > 0) {
        EngineLogLine note;
        note.level = JCE_CONSOLE_WARNING;
        snprintf(note.text, sizeof(note.text),
                 "[console] %d engine log line(s) dropped (see the log file)",
                 dropped);
        engine_line_stamp(&note, jce_time_now_epoch_seconds());
        console_engine_view_push(&note);
    }
}

/* Clear both streams: "Clear" must not leave half the console behind. */
static void console_clear_all(void)
{
    jce_editor_console_clear();
    if (s_engine.mtx) {
        jce_mutex_lock(s_engine.mtx);
        s_engine.tail    = s_engine.head;
        s_engine.dropped = 0;
        jce_mutex_unlock(s_engine.mtx);
    }
    s_engine.view_head  = 0;
    s_engine.view_count = 0;
}

/* Build the merged display list.  Both inputs are already in timestamp order,
 * so an in-place merge is enough.  Entries point at storage owned by the two
 * rings, neither of which is touched again before the frame ends. */
static void console_build_rows(std::vector<JceConsoleEntry> &rows)
{
    int editor_count = jce_editor_console_entry_count();
    rows.clear();
    rows.reserve((size_t)editor_count + (size_t)s_engine.view_count);

    for (int i = 0; i < editor_count; i++) {
        JceConsoleEntry e;
        if (jce_editor_console_entry_get(i, &e))
            rows.push_back(e);
    }
    size_t editor_n = rows.size();

    int start = 0;
    if (s_engine.view_count >= CONSOLE_ENGINE_VIEW)
        start = (int)(s_engine.view_head & (CONSOLE_ENGINE_VIEW - 1));
    for (int i = 0; i < s_engine.view_count; i++) {
        const EngineLogLine &l = s_engine.view[(start + i) & (CONSOLE_ENGINE_VIEW - 1)];
        JceConsoleEntry e;
        e.text      = l.text;
        e.timestamp = l.timestamp;
        e.level     = l.level;
        rows.push_back(e);
    }

    if (editor_n == 0 || rows.size() == editor_n) return;   /* one stream only */
    std::inplace_merge(rows.begin(), rows.begin() + (ptrdiff_t)editor_n, rows.end(),
                       [](const JceConsoleEntry &a, const JceConsoleEntry &b) {
                           return strcmp(a.timestamp, b.timestamp) < 0;
                       });
}

/* The editor's own commands, REGISTERED rather than if-chained.
 *
 * They used to be a chain of strncmp() in this function, which had three
 * costs.  `help` printed a hardcoded list naming only the chain, so a
 * registered command could never appear in it and the string was stale the
 * moment anyone added one.  The chain matched on PREFIX -- strncmp(cmd,
 * "help", 4) also fired for "helpme" and strncmp(cmd, "clear", 5) for
 * "clearfoo".  And nothing could enumerate them, so a completion key could
 * not offer the five commands this panel actually has.
 *
 * In the registry they are ordinary commands: exec dispatches them on an
 * exact name, the engine's `help` lists them beside every cvar because it
 * walks the same table, and Tab completes them. */
static void ed_cmd_clear(int argc, const char **argv, void *user)
{
    (void)argc; (void)argv; (void)user;
    console_clear_all();
    s_ui.selected.clear();
    s_ui.anchor = -1;
}

static void ed_cmd_echo(int argc, const char **argv, void *user)
{
    (void)user;
    char line[512];
    line[0] = '\0';
    for (int i = 1; i < argc; ++i) {
        if (i > 1) strncat(line, " ", sizeof line - strlen(line) - 1);
        strncat(line, argv[i], sizeof line - strlen(line) - 1);
    }
    jce_editor_console_log("%s", line);
}

static void ed_cmd_play (int, const char **, void *) { jce_state_play();  }
static void ed_cmd_stop (int, const char **, void *) { jce_state_stop();  }
static void ed_cmd_pause(int, const char **, void *) { jce_state_pause(); }

static void execute_console_command(const char *cmd)
{
    if (!cmd || !*cmd) return;
    jce_editor_console_log("> %s", cmd);
    /* One dispatcher.  cvars echo on a bare name and set on "name value";
     * commands -- the engine's help/list and the five registered above --
     * dispatch by exact name.  Output routes back into this panel through the
     * sink installed in ensure_init(). */
    if (!jce_console_exec(cmd))
        jce_editor_console_log_level(JCE_CONSOLE_WARNING,
            "unknown command or cvar: %s (try 'help')", cmd);
}

/* Up / Down / Tab on the command line.
 *
 * The buffer is ImGui's and the meaning is the session's, so each event
 * re-syncs first -- but only when they actually differ.  set_line() ends a
 * history walk by design (an edit means the user left it), so calling it
 * unconditionally would make Up always return the newest entry and never
 * step past it. */
static void cmdline_sync(ImGuiInputTextCallbackData *data)
{
    if (strcmp(data->Buf, jce_console_session_line(s_ui.session)) != 0)
        jce_console_session_set_line(s_ui.session, data->Buf);
}

static void cmdline_replace(ImGuiInputTextCallbackData *data)
{
    data->DeleteChars(0, data->BufTextLen);
    data->InsertChars(0, jce_console_session_line(s_ui.session));
}

static int cmdline_callback(ImGuiInputTextCallbackData *data)
{
    if (!s_ui.session) return 0;

    switch (data->EventFlag) {
    case ImGuiInputTextFlags_CallbackEdit:
        jce_console_session_set_line(s_ui.session, data->Buf);
        break;

    case ImGuiInputTextFlags_CallbackHistory:
        cmdline_sync(data);
        if (data->EventKey == ImGuiKey_UpArrow
                ? jce_console_session_history_prev(s_ui.session)
                : jce_console_session_history_next(s_ui.session))
            cmdline_replace(data);
        break;

    case ImGuiInputTextFlags_CallbackCompletion: {
        cmdline_sync(data);
        uint32_t total = jce_console_session_complete(s_ui.session);
        if (total > 0) cmdline_replace(data);
        if (total > 1) {
            /* Ambiguous: print the candidates.  This is the whole reason the
             * registry has jce_cvar_at / jce_console_cmd_at -- and until now
             * nothing in the repository called either. */
            uint32_t shown = jce_console_session_match_count(s_ui.session);
            for (uint32_t i = 0; i < shown; ++i)
                jce_editor_console_log("  %s",
                    jce_console_session_match_at(s_ui.session, i));
            if (total > shown)
                jce_editor_console_log("  ... and %u more",
                                       (unsigned)(total - shown));
        }
        break;
    }
    default: break;
    }
    return 0;
}

/* Route jce_console output (cvar echoes, command results, 'list') into the
 * editor console log so it shows in this same panel. */
static void console_sink(const char *text, void *user)
{
    (void)user;
    jce_editor_console_log("%s", text ? text : "");
}

static void ensure_init(void)
{
    if (s_ui.initialized) return;
    s_ui = ConsoleUiState{};
    /* Toolbar toggles persist per user (editor-session.json), bools as 0/1. */
    s_ui.show_info     = jce_editor_ui_state_load_int("console.filter.info",    1, 0, 1) != 0;
    s_ui.show_warning  = jce_editor_ui_state_load_int("console.filter.warning", 1, 0, 1) != 0;
    s_ui.show_error    = jce_editor_ui_state_load_int("console.filter.error",   1, 0, 1) != 0;
    s_ui.show_debug    = jce_editor_ui_state_load_int("console.show_debug",     0, 0, 1) != 0;
    s_ui.clear_on_play = jce_editor_ui_state_load_int("console.clear_on_play",  0, 0, 1) != 0;
    s_ui.collapse      = jce_editor_ui_state_load_int("console.collapse",       0, 0, 1) != 0;
    s_ui.auto_scroll   = jce_editor_ui_state_load_int("console.autoscroll",     1, 0, 1) != 0;
    s_ui.initialized = true;
    jce_console_set_output(console_sink, nullptr);

    if (!s_ui.session) s_ui.session = jce_console_session_create();

    jce_console_register_cmd("clear", ed_cmd_clear, nullptr,
                             "clear the console log");
    jce_console_register_cmd("echo",  ed_cmd_echo,  nullptr,
                             "print the rest of the line");
    jce_console_register_cmd("play",  ed_cmd_play,  nullptr,
                             "enter Play mode");
    jce_console_register_cmd("stop",  ed_cmd_stop,  nullptr,
                             "leave Play mode");
    jce_console_register_cmd("pause", ed_cmd_pause, nullptr,
                             "pause Play");

    /* Subscribe to the engine log.  The mutex must exist before the sink can
     * fire, and the sink is never removed — the log backend thread is joined
     * by jce_log_shutdown() long before these statics go away. */
    if (!s_engine.mtx) {
        s_engine.mtx = jce_mutex_create();
        if (s_engine.mtx)
            jce_log_set_sink(console_log_sink, nullptr);
    }
}

static void copy_selection_to_clipboard(const std::vector<JceConsoleEntry> &rows)
{
    if (s_ui.selected.empty()) return;
    std::string out;
    for (int idx : s_ui.selected) {
        if (idx < 0 || (size_t)idx >= rows.size()) continue;
        const JceConsoleEntry &entry = rows[(size_t)idx];
        const char *prefix;
        switch (entry.level) {
        case JCE_CONSOLE_WARNING: prefix = "[WARN]  "; break;
        case JCE_CONSOLE_ERROR:   prefix = "[ERROR] "; break;
        case JCE_CONSOLE_DEBUG:   prefix = "[DEBUG] "; break;
        default:                  prefix = "[INFO]  "; break;
        }
        out += entry.timestamp;
        out += ' ';
        out += prefix;
        out += entry.text;
        out += '\n';
    }
    if (!out.empty())
        ImGui::SetClipboardText(out.c_str());
}

static void copy_all_visible_to_clipboard(const std::vector<JceConsoleEntry> &rows)
{
    std::string out;
    for (size_t i = 0; i < rows.size(); i++) {
        const JceConsoleEntry &entry = rows[i];
        bool show = false;
        switch (entry.level) {
        case JCE_CONSOLE_INFO:    show = s_ui.show_info;    break;
        case JCE_CONSOLE_WARNING: show = s_ui.show_warning; break;
        case JCE_CONSOLE_ERROR:   show = s_ui.show_error;   break;
        case JCE_CONSOLE_DEBUG:   show = s_ui.show_debug;   break;
        }
        if (!show) continue;
        const char *prefix;
        switch (entry.level) {
        case JCE_CONSOLE_WARNING: prefix = "[WARN]  "; break;
        case JCE_CONSOLE_ERROR:   prefix = "[ERROR] "; break;
        case JCE_CONSOLE_DEBUG:   prefix = "[DEBUG] "; break;
        default:                  prefix = "[INFO]  "; break;
        }
        out += entry.timestamp;
        out += ' ';
        out += prefix;
        out += entry.text;
        out += '\n';
    }
    if (!out.empty())
        ImGui::SetClipboardText(out.c_str());
}

/* ── path:line detection (best-effort) ────────────────────────────── */

static bool _looks_like_src_ext(const char *e, size_t n)
{
    static const char *exts[] = {
        "c","h","cc","cxx","cpp","hpp","hh","inl","ipp",
        "lua","py","js","ts","glsl","hlsl","sc","sh",
        "json","yaml","yml","toml","md","txt","ini","cfg"
    };
    for (size_t i = 0; i < sizeof(exts)/sizeof(exts[0]); i++) {
        size_t el = strlen(exts[i]);
        if (n == el) {
            size_t j;
            for (j = 0; j < n; j++)
                if (tolower((unsigned char)e[j]) != exts[i][j]) break;
            if (j == n) return true;
        }
    }
    return false;
}

/* Scan a text line for a "path:line" token; return a pointer past the
 * token along with the path and line number on success. */
static bool console_try_parse_path_line(const char *text,
                                        std::string &out_path, int &out_line)
{
    if (!text) return false;
    const char *p = text;
    while (*p) {
        const char *colon = strchr(p, ':');
        if (!colon) return false;

        const char *line_start = colon + 1;
        if (!isdigit((unsigned char)*line_start)) {
            p = colon + 1;
            continue;
        }
        int line = 0;
        const char *q = line_start;
        while (isdigit((unsigned char)*q)) { line = line*10 + (*q - '0'); ++q; }
        if (line <= 0) { p = colon + 1; continue; }

        const char *dot = NULL;
        for (const char *r = colon - 1; r >= text; --r) {
            char c = *r;
            if (c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '(' || c == '<')
                break;
            if (c == '.') { dot = r; break; }
        }
        if (!dot || dot == colon - 1) { p = q; continue; }
        size_t ext_len = (size_t)(colon - dot - 1);
        if (!_looks_like_src_ext(dot + 1, ext_len)) { p = q; continue; }

        const char *path_start = dot;
        for (; path_start > text; --path_start) {
            char c = *(path_start - 1);
            if (c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '(' || c == '<' || c == '[')
                break;
        }
        out_path.assign(path_start, (size_t)(colon - path_start));
        out_line = line;
        return true;
    }
    return false;
}

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_console_content(void)
{
    ensure_init();

    /* Pull whatever the engine log queued since the last frame. */
    console_engine_drain();

    /* Clear-on-play: detect transition to PLAYING. */
    JcePlayState ps_now = jce_state_get_play_state();
    if (s_ui.clear_on_play && ps_now == JCE_PLAY_PLAYING && s_ui.last_play != JCE_PLAY_PLAYING) {
        console_clear_all();
        s_ui.selected.clear();
        s_ui.anchor = -1;
    }
    s_ui.last_play = ps_now;

    /* Toolbar: Clear + filter checkboxes + auto-scroll + clear-on-play + collapse */
    if (ImGui::SmallButton(jce_editor_i18n("console.clear")))
        console_clear_all();
    ImGui::SameLine();
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###filter_info", jce_editor_i18n("console.showLog"));
        if (ImGui::Checkbox(_lbl, &s_ui.show_info))
            jce_editor_ui_state_save_int("console.filter.info", s_ui.show_info ? 1 : 0);
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_CONSOLE_WARN);
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###filter_warn", jce_editor_i18n("console.showWarning"));
        if (ImGui::Checkbox(_lbl, &s_ui.show_warning))
            jce_editor_ui_state_save_int("console.filter.warning", s_ui.show_warning ? 1 : 0);
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_CONSOLE_ERROR);
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###filter_error", jce_editor_i18n("console.showError"));
        if (ImGui::Checkbox(_lbl, &s_ui.show_error))
            jce_editor_ui_state_save_int("console.filter.error", s_ui.show_error ? 1 : 0);
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###filter_debug", jce_editor_i18n("console.showDebug"));
        if (ImGui::Checkbox(_lbl, &s_ui.show_debug))
            jce_editor_ui_state_save_int("console.show_debug", s_ui.show_debug ? 1 : 0);
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(jce_editor_i18n_id("console.toggle.collapse", "collapse"), &s_ui.collapse))
        jce_editor_ui_state_save_int("console.collapse", s_ui.collapse ? 1 : 0);
    ImGui::SameLine();
    if (ImGui::Checkbox(jce_editor_i18n_id("console.toggle.clearOnPlay", "cop"), &s_ui.clear_on_play))
        jce_editor_ui_state_save_int("console.clear_on_play", s_ui.clear_on_play ? 1 : 0);
    ImGui::SameLine();
    if (ImGui::Checkbox(jce_editor_i18n_id("console.toggle.autoScroll", "auto_scroll"), &s_ui.auto_scroll))
        jce_editor_ui_state_save_int("console.autoscroll", s_ui.auto_scroll ? 1 : 0);

    /* Broken-invariant readout.  SHOWN ONLY WHEN NON-ZERO, because the number
     * that matters is "not zero" and a permanent `0 / 0` teaches the eye to
     * skip the spot where the first one will appear.
     *
     * JCE_ENSURE is the reason this needs a readout at all: it reports once
     * per site and then goes quiet forever, so an invariant that broke early
     * leaves nothing on screen a minute later.  The count does not go quiet. */
    {
        const unsigned long long af = jce_editor_assert_failures();
        const unsigned long long es = jce_editor_assert_ensure_sites();
        if (af || es) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                               jce_editor_i18n("console.invariants"), af, es);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", jce_editor_i18n("console.invariantsTip"));
        }
    }

    /* Search row */
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", jce_editor_i18n("console.searchHint"),
                              s_ui.search_buf, sizeof(s_ui.search_buf));

    ImGui::Separator();

    /* Reserve space for command line at the bottom */
    const float cmd_h = ImGui::GetFrameHeightWithSpacing();

    /* Log output */
    ImGui::BeginChild("ConsoleScroll", ImVec2(0, -cmd_h), ImGuiChildFlags_None,
                       ImGuiWindowFlags_HorizontalScrollbar);

    /* Editor events and mirrored engine log, interleaved by timestamp. */
    std::vector<JceConsoleEntry> rows;
    console_build_rows(rows);
    int count = (int)rows.size();

    /* Pre-compute per-entry visibility + collapse counts.
     * collapse merges consecutive entries with identical (level,text). */
    std::vector<int> visible_idx;
    std::vector<int> collapse_count;
    visible_idx.reserve(count);
    collapse_count.reserve(count);

    int i = 0;
    while (i < count) {
        const JceConsoleEntry &e = rows[(size_t)i];
        if (!entry_passes_filter(e)) { i++; continue; }

        int run = 1;
        if (s_ui.collapse) {
            int j = i + 1;
            while (j < count) {
                const JceConsoleEntry &e2 = rows[(size_t)j];
                if (!entry_passes_filter(e2)) { j++; continue; }
                if (e2.level == e.level && strcmp(e2.text, e.text) == 0) {
                    run++;
                    j++;
                } else break;
            }
            visible_idx.push_back(i);
            collapse_count.push_back(run);
            i = j;
        } else {
            visible_idx.push_back(i);
            collapse_count.push_back(1);
            i++;
        }
    }

    for (size_t vi = 0; vi < visible_idx.size(); ++vi) {
        int idx = visible_idx[vi];
        int dup = collapse_count[vi];
        const JceConsoleEntry &entry = rows[(size_t)idx];

        ImVec4 color;
        const char *prefix;
        switch (entry.level) {
        case JCE_CONSOLE_WARNING: color = JCE_COLOR_CONSOLE_WARN;  prefix = "[WARN]  "; break;
        case JCE_CONSOLE_ERROR:   color = JCE_COLOR_CONSOLE_ERROR; prefix = "[ERROR] "; break;
        case JCE_CONSOLE_DEBUG:   color = JCE_COLOR_CONSOLE_DEBUG; prefix = "[DEBUG] "; break;
        default:                  color = JCE_COLOR_CONSOLE_INFO;  prefix = "[INFO]  "; break;
        }

        ImGui::PushID(idx);
        bool selected = s_ui.selected.count(idx) != 0;
        ImVec2 row_start = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", selected,
                              ImGuiSelectableFlags_AllowOverlap
                              | ImGuiSelectableFlags_SpanAllColumns,
                              ImVec2(0, ImGui::GetTextLineHeight())))
        {
            bool ctrl  = ImGui::GetIO().KeyCtrl;
            bool shift = ImGui::GetIO().KeyShift;
            if (shift && s_ui.anchor >= 0) {
                int mn = s_ui.anchor < idx ? s_ui.anchor : idx;
                int mx = s_ui.anchor > idx ? s_ui.anchor : idx;
                if (!ctrl) s_ui.selected.clear();
                for (int k = mn; k <= mx; k++) s_ui.selected.insert(k);
            } else if (ctrl) {
                if (selected) s_ui.selected.erase(idx);
                else          s_ui.selected.insert(idx);
                s_ui.anchor = idx;
            } else {
                s_ui.selected.clear();
                s_ui.selected.insert(idx);
                s_ui.anchor = idx;
            }
        }

        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            if (!selected) {
                s_ui.selected.clear();
                s_ui.selected.insert(idx);
                s_ui.anchor = idx;
            }
        }

        /* Double-click: try to jump to a "path:line" reference in the message. */
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            std::string jpath;
            int jline = 0;
            if (console_try_parse_path_line(entry.text, jpath, jline)) {
                /* OPEN AT THE LINE, not at the top.  The line was parsed out
                 * of the message and then used only to print it back and to
                 * fill the tooltip -- so a script error took you to the file
                 * and left you to find the line yourself.  The function that
                 * honours it already existed for the hierarchy right-click
                 * and the search results; this call site just was not using
                 * it.  line <= 0 still opens at the top, which is what a
                 * message with no line resolves to. */
                jce_file_viewer_open_text_at(jpath.c_str(), jline);
                jce_editor_console_log("jump → %s:%d", jpath.c_str(), jline);
            }
        }
        if (ImGui::IsItemHovered()) {
            std::string jpath;
            int jline = 0;
            if (console_try_parse_path_line(entry.text, jpath, jline))
                ImGui::SetTooltip("%s %s:%d",
                    jce_editor_i18n("console.doubleClickToOpen"),
                    jpath.c_str(), jline);
        }

        if (ImGui::BeginPopupContextItem("##ctx")) {
            if (ImGui::MenuItem(jce_editor_i18n("console.copy"), "Ctrl+C",
                                false, !s_ui.selected.empty()))
                copy_selection_to_clipboard(rows);
            if (ImGui::MenuItem(jce_editor_i18n("console.copyAll")))
                copy_all_visible_to_clipboard(rows);
            ImGui::Separator();
            if (ImGui::MenuItem(jce_editor_i18n("console.clear"))) {
                console_clear_all();
                s_ui.selected.clear();
                s_ui.anchor = -1;
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine(0, 0);
        ImGui::SetCursorScreenPos(row_start);
        ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_SECONDARY);
        ImGui::TextUnformatted(entry.timestamp);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(prefix);
        ImGui::SameLine();
        ImGui::TextUnformatted(entry.text);
        ImGui::PopStyleColor();
        if (dup > 1) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_SECONDARY);
            ImGui::Text("(x%d)", dup);
            ImGui::PopStyleColor();
        }

        ImGui::PopID();
    }

    if (ImGui::BeginPopupContextWindow("##console_bg_ctx",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        if (ImGui::MenuItem(jce_editor_i18n("console.copyAll")))
            copy_all_visible_to_clipboard(rows);
        if (ImGui::MenuItem(jce_editor_i18n("console.clear"))) {
            console_clear_all();
            s_ui.selected.clear();
            s_ui.anchor = -1;
        }
        ImGui::EndPopup();
    }

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && !ImGui::GetIO().WantTextInput)
    {
        bool ctrl = ImGui::GetIO().KeyCtrl;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
            copy_selection_to_clipboard(rows);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            s_ui.selected.clear();
            s_ui.anchor = -1;
        }
    }

    if (s_ui.auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();

    /* Command-line input at the bottom, backed by the session. */
    ImGui::SetNextItemWidth(-1);
    bool submit = ImGui::InputText(
        "##cmdline", s_ui.cmd_buf, sizeof(s_ui.cmd_buf),
        ImGuiInputTextFlags_EnterReturnsTrue
            | ImGuiInputTextFlags_CallbackHistory
            | ImGuiInputTextFlags_CallbackCompletion
            | ImGuiInputTextFlags_CallbackEdit,
        cmdline_callback);
    if (submit && s_ui.cmd_buf[0]) {
        char sent[sizeof s_ui.cmd_buf];
        snprintf(sent, sizeof sent, "%s", s_ui.cmd_buf);
        jce_console_session_set_line(s_ui.session, sent);
        jce_editor_console_log("> %s", sent);
        /* submit() records the line in history -- including a line the parser
         * rejects, because a typo is exactly what Up is for -- runs it, and
         * clears the session's copy. */
        if (!jce_console_session_submit(s_ui.session))
            jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                "unknown command or cvar: %s (try 'help')", sent);
        s_ui.cmd_buf[0] = '\0';
        ImGui::SetKeyboardFocusHere(-1);
    }
}

/* ── Standalone wrapper ───────────────────────────────────────────── */

void jce_editor_panel_console(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_CONSOLE);
    if (!*vis) return;

    char title[256];
    snprintf(title, sizeof(title), "%s###console", jce_editor_i18n("console.title"));
    if (ImGui::Begin(title, vis, ImGuiWindowFlags_NoFocusOnAppearing))
        jce_editor_panel_console_content();
    ImGui::End();
}
