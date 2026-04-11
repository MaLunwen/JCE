/*
 * jce_panel_console.cpp  Console panel (log output with filtering).
 * Extracted from jce_editor_panels.cpp.
 *
 * The console ring buffer lives in jce_editor_panels.cpp; this file
 * uses the iteration API (jce_editor_console_entry_count/get) to read it.
 */

#include "jce_editor_panels.h"
#include "jce_editor_colors.h"
#include "jce_editor_i18n.h"

#include <imgui.h>
#include <stdio.h>
#include <string.h>

/* ── Console UI state (filter / scroll) ───────────────────────────── */

static struct {
    bool auto_scroll;
    bool show_info;
    bool show_warning;
    bool show_error;
    bool show_debug;
    bool initialized;
} s_ui;

static void ensure_init(void)
{
    if (s_ui.initialized) return;
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.auto_scroll  = true;
    s_ui.show_info    = true;
    s_ui.show_warning = true;
    s_ui.show_error   = true;
    s_ui.show_debug   = false;
    s_ui.initialized  = true;
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

        /* Color by level */
        ImVec4 color;
        const char *prefix;
        switch (entry.level) {
        case JCE_CONSOLE_WARNING: color = JCE_COLOR_CONSOLE_WARN;  prefix = "[WARN]  "; break;
        case JCE_CONSOLE_ERROR:   color = JCE_COLOR_CONSOLE_ERROR; prefix = "[ERROR] "; break;
        case JCE_CONSOLE_DEBUG:   color = JCE_COLOR_CONSOLE_DEBUG; prefix = "[DEBUG] "; break;
        default:                  color = JCE_COLOR_CONSOLE_INFO;  prefix = "[INFO]  "; break;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_SECONDARY);
        ImGui::TextUnformatted(entry.timestamp);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(prefix);
        ImGui::SameLine();
        ImGui::TextUnformatted(entry.text);
        ImGui::PopStyleColor();
    }

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
