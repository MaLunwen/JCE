/*
 * jce_panel_console.cpp  Console panel (log output with filtering).
 * Extracted from jce_editor_panels.cpp.
 *
 * The console ring buffer lives in jce_editor_panels.cpp; this file
 * uses the iteration API (jce_editor_console_entry_count/get) to read it.
 */

#include "jce_editor_colors.h"
#include "jce_editor_i18n.h"
#include "jce_editor_panels.h"

#include <imgui.h>
#include <stdio.h>
#include <string.h>

#include <set>
#include <string>

/* ── Console UI state (filter / scroll / selection) ───────────────── */

struct ConsoleUiState {
    bool          auto_scroll  = true;
    bool          show_info    = true;
    bool          show_warning = true;
    bool          show_error   = true;
    bool          show_debug   = false;
    bool          initialized  = false;
    std::set<int> selected;   /* entry indices */
    int           anchor      = -1;
};

static ConsoleUiState s_ui;

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

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_console_content(void)
{
    ensure_init();

    /* Toolbar: Clear + filter checkboxes + auto-scroll */
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
    float right = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(ImGui::GetCursorPosX() + right - 100);
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###auto_scroll", jce_editor_i18n("console.autoScroll"));
        ImGui::Checkbox(_lbl, &s_ui.auto_scroll);
    }
    ImGui::Separator();

    /* Log output */
    ImGui::BeginChild("ConsoleScroll", ImVec2(0, 0), ImGuiChildFlags_None,
                       ImGuiWindowFlags_HorizontalScrollbar);

    int count = jce_editor_console_entry_count();
    int last_visible = -1;
    for (int i = 0; i < count; i++) {
        JceConsoleEntry entry;
        if (!jce_editor_console_entry_get(i, &entry))
            continue;

        /* Filter by level */
        bool show = false;
        switch (entry.level) {
        case JCE_CONSOLE_INFO:    show = s_ui.show_info;    break;
        case JCE_CONSOLE_WARNING: show = s_ui.show_warning; break;
        case JCE_CONSOLE_ERROR:   show = s_ui.show_error;   break;
        case JCE_CONSOLE_DEBUG:   show = s_ui.show_debug;   break;
        }
        if (!show) continue;
        last_visible = i;

        /* Color by level */
        ImVec4 color;
        const char *prefix;
        switch (entry.level) {
        case JCE_CONSOLE_WARNING: color = JCE_COLOR_CONSOLE_WARN;  prefix = "[WARN]  "; break;
        case JCE_CONSOLE_ERROR:   color = JCE_COLOR_CONSOLE_ERROR; prefix = "[ERROR] "; break;
        case JCE_CONSOLE_DEBUG:   color = JCE_COLOR_CONSOLE_DEBUG; prefix = "[DEBUG] "; break;
        default:                  color = JCE_COLOR_CONSOLE_INFO;  prefix = "[INFO]  "; break;
        }

        ImGui::PushID(i);
        bool selected = s_ui.selected.count(i) != 0;
        ImVec2 row_start = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", selected,
                              ImGuiSelectableFlags_AllowOverlap
                              | ImGuiSelectableFlags_SpanAllColumns,
                              ImVec2(0, ImGui::GetTextLineHeight())))
        {
            bool ctrl  = ImGui::GetIO().KeyCtrl;
            bool shift = ImGui::GetIO().KeyShift;
            if (shift && s_ui.anchor >= 0) {
                int mn = s_ui.anchor < i ? s_ui.anchor : i;
                int mx = s_ui.anchor > i ? s_ui.anchor : i;
                if (!ctrl) s_ui.selected.clear();
                for (int k = mn; k <= mx; k++) s_ui.selected.insert(k);
            } else if (ctrl) {
                if (selected) s_ui.selected.erase(i);
                else          s_ui.selected.insert(i);
                s_ui.anchor = i;
            } else {
                s_ui.selected.clear();
                s_ui.selected.insert(i);
                s_ui.anchor = i;
            }
        }

        /* Right-click on row also selects it (if not already) so context
         * menu acts on the right-clicked entry. */
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            if (!selected) {
                s_ui.selected.clear();
                s_ui.selected.insert(i);
                s_ui.anchor = i;
            }
        }

        if (ImGui::BeginPopupContextItem("##ctx")) {
            if (ImGui::MenuItem(jce_editor_i18n("console.copy"), "Ctrl+C",
                                false, !s_ui.selected.empty()))
                copy_selection_to_clipboard();
            if (ImGui::MenuItem(jce_editor_i18n("console.copyAll")))
                copy_all_visible_to_clipboard();
            if (ImGui::MenuItem(jce_editor_i18n("console.selectAll"), "Ctrl+A"))
            {
                s_ui.selected.clear();
                int n = jce_editor_console_entry_count();
                for (int k = 0; k < n; k++) {
                    JceConsoleEntry e;
                    if (!jce_editor_console_entry_get(k, &e)) continue;
                    bool s = false;
                    switch (e.level) {
                    case JCE_CONSOLE_INFO:    s = s_ui.show_info;    break;
                    case JCE_CONSOLE_WARNING: s = s_ui.show_warning; break;
                    case JCE_CONSOLE_ERROR:   s = s_ui.show_error;   break;
                    case JCE_CONSOLE_DEBUG:   s = s_ui.show_debug;   break;
                    }
                    if (s) s_ui.selected.insert(k);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(jce_editor_i18n("console.clear"))) {
                jce_editor_console_clear();
                s_ui.selected.clear();
                s_ui.anchor = -1;
            }
            ImGui::EndPopup();
        }

        /* Overlay text on top of the selectable row. */
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

        ImGui::PopID();
    }

    /* Background context menu (for empty area). */
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

    /* Keyboard shortcuts (when console is focused). */
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && !ImGui::GetIO().WantTextInput)
    {
        bool ctrl = ImGui::GetIO().KeyCtrl;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
            copy_selection_to_clipboard();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            s_ui.selected.clear();
            int n = jce_editor_console_entry_count();
            for (int k = 0; k < n; k++) {
                JceConsoleEntry e;
                if (!jce_editor_console_entry_get(k, &e)) continue;
                bool s = false;
                switch (e.level) {
                case JCE_CONSOLE_INFO:    s = s_ui.show_info;    break;
                case JCE_CONSOLE_WARNING: s = s_ui.show_warning; break;
                case JCE_CONSOLE_ERROR:   s = s_ui.show_error;   break;
                case JCE_CONSOLE_DEBUG:   s = s_ui.show_debug;   break;
                }
                if (s) s_ui.selected.insert(k);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            s_ui.selected.clear();
            s_ui.anchor = -1;
        }
    }

    (void)last_visible;
    if (s_ui.auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
}

/* ── Standalone wrapper ───────────────────────────────────────────── */

void jce_editor_panel_console(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_CONSOLE);
    if (!*vis) return;

    char title[256];
    snprintf(title, sizeof(title), "%s###Console", jce_editor_i18n("console.title"));
    if (ImGui::Begin(title, vis))
        jce_editor_panel_console_content();
    ImGui::End();
}
