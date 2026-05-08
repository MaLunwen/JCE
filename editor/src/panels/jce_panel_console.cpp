/*
 * jce_panel_console.cpp  Console panel (log output with filtering).
 * Extracted from jce_editor_panels.cpp.
 *
 * The console ring buffer lives in jce_editor_panels.cpp; this file
 * uses the iteration API (jce_editor_console_entry_count/get) to read it.
 */

#include "ui/jce_editor_colors.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "viewers/jce_file_viewer.h"

#include <jce/tools/jce_imgui.hpp>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

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

static void execute_console_command(const char *cmd)
{
    if (!cmd || !*cmd) return;
    jce_editor_console_log("> %s", cmd);

    if (strncmp(cmd, "clear", 5) == 0) {
        jce_editor_console_clear();
        s_ui.selected.clear();
        s_ui.anchor = -1;
    } else if (strncmp(cmd, "help", 4) == 0) {
        jce_editor_console_log("Commands: clear, help, echo <msg>, play, stop, pause");
    } else if (strncmp(cmd, "echo ", 5) == 0) {
        jce_editor_console_log("%s", cmd + 5);
    } else if (strcmp(cmd, "play") == 0) {
        jce_state_play();
    } else if (strcmp(cmd, "stop") == 0) {
        jce_state_stop();
    } else if (strcmp(cmd, "pause") == 0) {
        jce_state_pause();
    } else {
        jce_editor_console_log_level(JCE_CONSOLE_WARNING,
            "unknown command: %s (try 'help')", cmd);
    }
}

static void ensure_init(void)
{
    if (s_ui.initialized) return;
    s_ui = ConsoleUiState{};
    s_ui.initialized = true;
}

static void copy_selection_to_clipboard(void)
{
    if (s_ui.selected.empty()) return;
    std::string out;
    for (int idx : s_ui.selected) {
        JceConsoleEntry entry;
        if (!jce_editor_console_entry_get(idx, &entry)) continue;
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

static void copy_all_visible_to_clipboard(void)
{
    std::string out;
    int count = jce_editor_console_entry_count();
    for (int i = 0; i < count; i++) {
        JceConsoleEntry entry;
        if (!jce_editor_console_entry_get(i, &entry)) continue;
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

    /* Clear-on-play: detect transition to PLAYING. */
    JcePlayState ps_now = jce_state_get_play_state();
    if (s_ui.clear_on_play && ps_now == JCE_PLAY_PLAYING && s_ui.last_play != JCE_PLAY_PLAYING) {
        jce_editor_console_clear();
        s_ui.selected.clear();
        s_ui.anchor = -1;
    }
    s_ui.last_play = ps_now;

    /* Toolbar: Clear + filter checkboxes + auto-scroll + clear-on-play + collapse */
    if (ImGui::SmallButton(jce_editor_i18n("console.clear")))
        jce_editor_console_clear();
    ImGui::SameLine();
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###filter_info", jce_editor_i18n("console.showLog"));
        ImGui::Checkbox(_lbl, &s_ui.show_info);
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_CONSOLE_WARN);
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###filter_warn", jce_editor_i18n("console.showWarning"));
        ImGui::Checkbox(_lbl, &s_ui.show_warning);
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_CONSOLE_ERROR);
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###filter_error", jce_editor_i18n("console.showError"));
        ImGui::Checkbox(_lbl, &s_ui.show_error);
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###filter_debug", jce_editor_i18n("console.showDebug"));
        ImGui::Checkbox(_lbl, &s_ui.show_debug);
    }
    ImGui::SameLine();
    ImGui::Checkbox(jce_editor_i18n_id("console.toggle.collapse", "collapse"), &s_ui.collapse);
    ImGui::SameLine();
    ImGui::Checkbox(jce_editor_i18n_id("console.toggle.clearOnPlay", "cop"), &s_ui.clear_on_play);
    ImGui::SameLine();
    ImGui::Checkbox(jce_editor_i18n_id("console.toggle.autoScroll", "auto_scroll"), &s_ui.auto_scroll);

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

    int count = jce_editor_console_entry_count();

    /* Pre-compute per-entry visibility + collapse counts.
     * collapse merges consecutive entries with identical (level,text). */
    std::vector<int> visible_idx;
    std::vector<int> collapse_count;
    visible_idx.reserve(count);
    collapse_count.reserve(count);

    int i = 0;
    while (i < count) {
        JceConsoleEntry e;
        if (!jce_editor_console_entry_get(i, &e)) { i++; continue; }
        if (!entry_passes_filter(e)) { i++; continue; }

        int run = 1;
        if (s_ui.collapse) {
            int j = i + 1;
            while (j < count) {
                JceConsoleEntry e2;
                if (!jce_editor_console_entry_get(j, &e2)) break;
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
        JceConsoleEntry entry;
        if (!jce_editor_console_entry_get(idx, &entry)) continue;

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
                jce_file_viewer_open(jpath.c_str());
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
                copy_selection_to_clipboard();
            if (ImGui::MenuItem(jce_editor_i18n("console.copyAll")))
                copy_all_visible_to_clipboard();
            ImGui::Separator();
            if (ImGui::MenuItem(jce_editor_i18n("console.clear"))) {
                jce_editor_console_clear();
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
            copy_all_visible_to_clipboard();
        if (ImGui::MenuItem(jce_editor_i18n("console.clear"))) {
            jce_editor_console_clear();
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
            copy_selection_to_clipboard();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            s_ui.selected.clear();
            s_ui.anchor = -1;
        }
    }

    if (s_ui.auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();

    /* Command-line input at the bottom. */
    ImGui::SetNextItemWidth(-1);
    bool submit = ImGui::InputText("##cmdline", s_ui.cmd_buf, sizeof(s_ui.cmd_buf),
                                    ImGuiInputTextFlags_EnterReturnsTrue);
    if (submit && s_ui.cmd_buf[0]) {
        execute_console_command(s_ui.cmd_buf);
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
    if (ImGui::Begin(title, vis))
        jce_editor_panel_console_content();
    ImGui::End();
}
