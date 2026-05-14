/*
 * jce_editor_command_palette.cpp  Fuzzy-search command launcher.
 *
 * Storage: fixed array of (id, display_name, group, fn, user_data)
 * tuples.  Fuzzy match scores each command against the search input
 * using a simple subsequence-with-bonus algorithm (similar to fzf).
 * Top 30 results display in a vertical list; Enter runs the
 * highlighted item, Esc cancels.
 */

#include "jce_editor_command_palette.h"

#include <jce/tools/jce_imgui.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct Command {
    char               id          [JCE_EDITOR_COMMAND_NAME_MAX];
    char               display_name[JCE_EDITOR_COMMAND_NAME_MAX];
    char               group       [JCE_EDITOR_COMMAND_GROUP_MAX];
    JceEditorCommandFn fn;
    void              *user_data;
    bool               active;
};

Command s_cmds[JCE_EDITOR_COMMAND_MAX_REGISTERED];
uint32_t s_cmd_count = 0;

bool s_open = false;
bool s_focus_input_next_frame = false;
char s_search[128] = {0};
int  s_selected = 0;

int find_by_id(const char *id)
{
    if (!id) return -1;
    for (uint32_t i = 0; i < JCE_EDITOR_COMMAND_MAX_REGISTERED; ++i)
        if (s_cmds[i].active && strncmp(s_cmds[i].id, id,
                                        JCE_EDITOR_COMMAND_NAME_MAX) == 0)
            return (int)i;
    return -1;
}

/* Simple fuzzy score: rewards consecutive matches + match at word
 * boundaries.  Returns 0 when `pattern` isn't a subsequence of
 * `text` (case-insensitive). */
int fuzzy_score(const char *text, const char *pattern)
{
    if (!pattern || !pattern[0]) return 1;
    int score = 0;
    int prev_match = -2;
    bool prev_word_boundary = true;
    int ti = 0;
    for (int pi = 0; pattern[pi]; ++pi) {
        char pc = (char)tolower((unsigned char)pattern[pi]);
        while (text[ti]) {
            char tc = (char)tolower((unsigned char)text[ti]);
            if (tc == pc) break;
            ti++;
            prev_word_boundary = (text[ti - 1] == ' ' || text[ti - 1] == '.'
                                  || text[ti - 1] == '_' || text[ti - 1] == '/');
        }
        if (!text[ti]) return 0;
        /* Base score for any hit. */
        score += 4;
        /* Consecutive hit bonus. */
        if (ti == prev_match + 1) score += 8;
        /* Word-boundary bonus. */
        if (prev_word_boundary) score += 12;
        prev_match = ti;
        prev_word_boundary = false;
        ti++;
    }
    return score;
}

struct Result {
    int cmd_index;
    int score;
};

int compare_results(const void *a, const void *b)
{
    const Result *ra = (const Result *)a;
    const Result *rb = (const Result *)b;
    return rb->score - ra->score; /* descending */
}

} /* namespace */

extern "C" bool jce_editor_command_register(const char *id,
                                             const char *display_name,
                                             const char *group,
                                             JceEditorCommandFn fn,
                                             void *ud)
{
    if (!id || !id[0] || !display_name || !fn) return false;
    /* Replace existing entry with the same id. */
    int slot = find_by_id(id);
    if (slot < 0) {
        for (uint32_t i = 0; i < JCE_EDITOR_COMMAND_MAX_REGISTERED; ++i) {
            if (!s_cmds[i].active) { slot = (int)i; break; }
        }
    }
    if (slot < 0) return false;

    Command &c = s_cmds[slot];
    c.active = true;
    strncpy(c.id,           id,           JCE_EDITOR_COMMAND_NAME_MAX - 1);
    strncpy(c.display_name, display_name, JCE_EDITOR_COMMAND_NAME_MAX - 1);
    strncpy(c.group,        group ? group : "", JCE_EDITOR_COMMAND_GROUP_MAX - 1);
    c.id[JCE_EDITOR_COMMAND_NAME_MAX - 1] = '\0';
    c.display_name[JCE_EDITOR_COMMAND_NAME_MAX - 1] = '\0';
    c.group[JCE_EDITOR_COMMAND_GROUP_MAX - 1] = '\0';
    c.fn = fn;
    c.user_data = ud;

    /* Recount active commands. */
    uint32_t n = 0;
    for (uint32_t i = 0; i < JCE_EDITOR_COMMAND_MAX_REGISTERED; ++i)
        if (s_cmds[i].active) n++;
    s_cmd_count = n;
    return true;
}

extern "C" bool jce_editor_command_unregister(const char *id)
{
    int slot = find_by_id(id);
    if (slot < 0) return false;
    s_cmds[slot].active = false;
    s_cmd_count--;
    return true;
}

extern "C" void jce_editor_command_palette_open(void)
{
    s_open = true;
    s_focus_input_next_frame = true;
    s_search[0] = '\0';
    s_selected = 0;
}

extern "C" void jce_editor_command_palette_close(void)
{
    s_open = false;
}

extern "C" bool jce_editor_command_palette_is_open(void)
{
    return s_open;
}

extern "C" uint32_t jce_editor_command_count(void)
{
    return s_cmd_count;
}

extern "C" void jce_editor_command_palette_render(void)
{
    if (!s_open) return;

    /* Centre on viewport. */
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImVec2 size(640, 420);
    ImVec2 pos(vp->WorkPos.x + (vp->WorkSize.x - size.x) * 0.5f,
               vp->WorkPos.y + (vp->WorkSize.y - size.y) * 0.3f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.96f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("##cmd_palette", &s_open, flags)) {
        ImGui::End();
        return;
    }

    /* Input. */
    if (s_focus_input_next_frame) {
        ImGui::SetKeyboardFocusHere();
        s_focus_input_next_frame = false;
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", "type a command…",
                              s_search, sizeof(s_search));

    /* Rank commands. */
    Result results[64];
    int    result_count = 0;
    for (uint32_t i = 0; i < JCE_EDITOR_COMMAND_MAX_REGISTERED
                       && result_count < 64; ++i) {
        if (!s_cmds[i].active) continue;
        int sc = fuzzy_score(s_cmds[i].display_name, s_search);
        if (sc <= 0) continue;
        results[result_count].cmd_index = (int)i;
        results[result_count].score     = sc;
        result_count++;
    }
    qsort(results, (size_t)result_count, sizeof(Result), compare_results);
    if (s_selected >= result_count) s_selected = result_count - 1;
    if (s_selected < 0)              s_selected = 0;

    /* Up/down navigation. */
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) s_selected++;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow,   true)) s_selected--;
    if (s_selected >= result_count) s_selected = 0;
    if (s_selected < 0)              s_selected = result_count - 1;

    /* List. */
    ImGui::BeginChild("##cmd_list", ImVec2(0, 0), false);
    for (int i = 0; i < result_count; ++i) {
        Command &c = s_cmds[results[i].cmd_index];
        bool sel = (i == s_selected);
        char lbl[160];
        snprintf(lbl, sizeof(lbl), "%s%s%s",
                 c.group[0] ? "[" : "", c.group, c.group[0] ? "] " : "");
        ImGui::PushID(c.id);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.85f, 0.3f, 1));
        bool clicked = ImGui::Selectable(lbl, sel,
                                          ImGuiSelectableFlags_SpanAllColumns);
        ImGui::SameLine();
        ImGui::TextUnformatted(c.display_name);
        if (sel) ImGui::PopStyleColor();
        ImGui::PopID();
        if (clicked) {
            s_selected = i;
            if (c.fn) c.fn(c.user_data);
            jce_editor_command_palette_close();
            break;
        }
    }
    ImGui::EndChild();

    /* Enter to run, Esc to cancel. */
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && result_count > 0) {
        Command &c = s_cmds[results[s_selected].cmd_index];
        if (c.fn) c.fn(c.user_data);
        jce_editor_command_palette_close();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        jce_editor_command_palette_close();
    }

    ImGui::End();
}
