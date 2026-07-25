/*
 * jce_panel_user_guide.cpp — Official operation guide panel.
 *
 * Renderer for the in-editor user guide: a chapter/topic tree on the
 * left, block-based content on the right.  See jce_panel_user_guide.h
 * for the data model.  Two LIVE elements ground the guide in the real
 * editor state:
 *
 *   - Hotkey chips read the CURRENT chord from the hotkey registry, so
 *     rebound keys render correctly (and the built-in final chapter is
 *     a full hotkey reference generated from the registry, never stale).
 *   - "Open panel" buttons toggle the documented panel visible and
 *     focus it, so the reader can follow every topic hands-on.
 */

#include "jce_panel_common.h"
#include "ui/jce_editor_colors.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "jce_panel_user_guide.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstring>

/* ── Chapter registry ───────────────────────────────────────────────── */

static const JceGuideChapter *const k_guide_chapters[] = {
    &g_jce_guide_ch_getting_started,
    &g_jce_guide_ch_scene_editing,
    &g_jce_guide_ch_inspector_components,
    &g_jce_guide_ch_assets,
    &g_jce_guide_ch_rendering,
    &g_jce_guide_ch_animation,
    &g_jce_guide_ch_physics,
    &g_jce_guide_ch_audio_media,
    &g_jce_guide_ch_world_systems,
    &g_jce_guide_ch_play_runtime,
    &g_jce_guide_ch_build_distribution,
    &g_jce_guide_ch_diagnostics_customization,
};
static const int k_guide_chapter_count =
    (int)(sizeof(k_guide_chapters) / sizeof(k_guide_chapters[0]));

/* The hotkey reference is a virtual chapter appended after the content
 * chapters; it renders straight from the registry. */
static const int k_hotkey_chapter_index = k_guide_chapter_count;

/* ── State ──────────────────────────────────────────────────────────── */

static int  s_sel_chapter = 0;
static int  s_sel_topic   = 0;
static char s_filter[64]  = "";
static char s_hk_filter[64] = "";

/* External jump request (Help menu / About / Welcome can deep-link). */
static int  s_req_chapter = -1;

extern "C" void jce_editor_panel_user_guide_select_chapter(int chapter_index)
{
    s_req_chapter = chapter_index;
}

/* ── Helpers ────────────────────────────────────────────────────────── */

static void draw_hotkey_chip(JceHotkeyId id)
{
    char chord[64];
    jce_hotkey_chord_label(jce_hotkey_get(id), chord, sizeof(chord));
    if (!chord[0])
        snprintf(chord, sizeof(chord), "%s",
                 jce_editor_i18n("guide.ui.unbound"));
    ImGui::SameLine();
    ImGui::TextColored(JCE_COLOR_ACCENT, "[%s]", chord);
}

static void draw_menu_path(const char *path)
{
    /* str = "menu.a>menu.a.b>..." → "A → B → ..." */
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", path ? path : "");
    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
                       jce_editor_i18n("guide.ui.menuPath"));
    char *tok = strtok(buf, ">");
    bool first = true;
    while (tok) {
        ImGui::SameLine(0.0f, first ? 6.0f : 4.0f);
        if (!first) {
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s", "\xE2\x86\x92");
            ImGui::SameLine(0.0f, 4.0f);
        }
        ImGui::TextColored(JCE_COLOR_ACCENT, "%s", jce_editor_i18n(tok));
        first = false;
        tok = strtok(nullptr, ">");
    }
}

static void draw_open_panel_button(const JceGuideBlock *b)
{
    char label[160];
    snprintf(label, sizeof(label), "%s %s###guide_open_%s",
             jce_editor_i18n("guide.ui.openPanel"),
             b->text ? jce_editor_i18n(b->text) : "?",
             b->str ? b->str : "x");
    if (ImGui::SmallButton(label)) {
        if (b->aux >= 0 && b->aux < JCE_PANEL_COUNT) {
            bool *vis = jce_editor_panel_visible_ptr((JceEditorPanel)b->aux);
            if (vis) *vis = true;
        }
        if (b->str && b->str[0])
            jce_editor_panel_request_focus(b->str);
    }
}

static void draw_topic_blocks(const JceGuideTopic *topic)
{
    int step_no = 0;
    for (int i = 0; i < topic->block_count; i++) {
        const JceGuideBlock *b = &topic->blocks[i];
        switch ((JceGuideBlockType)b->type) {
        case JCE_GB_H1:
            ImGui::Spacing();
            ImGui::TextColored(JCE_COLOR_ACCENT, "%s",
                               jce_editor_i18n(b->text));
            ImGui::Separator();
            step_no = 0;
            break;
        case JCE_GB_P:
            ImGui::TextWrapped("%s", jce_editor_i18n(b->text));
            ImGui::Spacing();
            break;
        case JCE_GB_BULLET:
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::TextWrapped("%s", jce_editor_i18n(b->text));
            break;
        case JCE_GB_STEP: {
            step_no++;
            char num[16];
            snprintf(num, sizeof(num), "%d.", step_no);
            ImGui::TextColored(JCE_COLOR_ACCENT, "%s", num);
            ImGui::SameLine();
            ImGui::TextWrapped("%s", jce_editor_i18n(b->text));
            break;
        }
        case JCE_GB_TIP:
            ImGui::TextColored(JCE_COLOR_TEXT_SUCCESS, "%s",
                               jce_editor_i18n("guide.ui.tip"));
            ImGui::SameLine();
            ImGui::TextWrapped("%s", jce_editor_i18n(b->text));
            break;
        case JCE_GB_WARN:
            ImGui::TextColored(JCE_COLOR_TEXT_WARNING, "%s",
                               jce_editor_i18n("guide.ui.warn"));
            ImGui::SameLine();
            ImGui::TextWrapped("%s", jce_editor_i18n(b->text));
            break;
        case JCE_GB_HOTKEY:
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::TextUnformatted(jce_editor_i18n(b->text));
            draw_hotkey_chip((JceHotkeyId)b->aux);
            break;
        case JCE_GB_OPEN_PANEL:
            draw_open_panel_button(b);
            break;
        case JCE_GB_MENU_PATH:
            draw_menu_path(b->str);
            break;
        case JCE_GB_SEP:
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            break;
        default:
            break;
        }
    }
}

/* ── Built-in hotkey reference chapter ──────────────────────────────── */

static void draw_hotkey_reference(void)
{
    ImGui::TextColored(JCE_COLOR_ACCENT, "%s",
                       jce_editor_i18n("guide.hotkeys.title"));
    ImGui::Separator();
    ImGui::TextWrapped("%s", jce_editor_i18n("guide.hotkeys.intro"));
    ImGui::Spacing();

    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##guide_hk_filter", jce_editor_i18n("guide.ui.filter"),
                             s_hk_filter, sizeof(s_hk_filter));
    ImGui::Spacing();

    if (!ImGui::BeginTable("##guide_hk_table", 3,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                           ImGuiTableFlags_SizingStretchProp))
        return;
    ImGui::TableSetupColumn(jce_editor_i18n("guide.hotkeys.col.action"), 0, 0.5f);
    ImGui::TableSetupColumn(jce_editor_i18n("guide.hotkeys.col.chord"),  0, 0.25f);
    ImGui::TableSetupColumn(jce_editor_i18n("guide.hotkeys.col.id"),     0, 0.25f);
    ImGui::TableHeadersRow();

    for (int i = 0; i < jce_hotkeys_count(); i++) {
        JceHotkeyId id = (JceHotkeyId)i;
        const char *name   = jce_hotkey_name(id);
        const char *id_str = jce_hotkey_id_string(id);
        char chord[64];
        jce_hotkey_chord_label(jce_hotkey_get(id), chord, sizeof(chord));

        if (s_hk_filter[0] &&
            !jce_panel_contains_ci(name, s_hk_filter) &&
            !jce_panel_contains_ci(id_str, s_hk_filter) &&
            !jce_panel_contains_ci(chord, s_hk_filter))
            continue;

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(name ? name : "");
        ImGui::TableSetColumnIndex(1);
        if (chord[0])
            ImGui::TextColored(JCE_COLOR_ACCENT, "%s", chord);
        else
            ImGui::TextDisabled("%s", jce_editor_i18n("guide.ui.unbound"));
        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("%s", id_str ? id_str : "");
    }
    ImGui::EndTable();

    ImGui::Spacing();
    ImGui::TextWrapped("%s", jce_editor_i18n("guide.hotkeys.rebind"));
}

/* ── Panel content ──────────────────────────────────────────────────── */

extern "C" void jce_editor_panel_user_guide_content(void)
{
    /* Apply deep-link request (from Help menu / About / Welcome). */
    if (s_req_chapter >= 0) {
        if (s_req_chapter <= k_hotkey_chapter_index) {
            s_sel_chapter = s_req_chapter;
            s_sel_topic   = 0;
        }
        s_req_chapter = -1;
    }

    /* Left: chapter / topic tree with filter. */
    ImGui::BeginChild("##guide_nav", ImVec2(250.0f, 0.0f),
                      ImGuiChildFlags_ResizeX);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##guide_filter", jce_editor_i18n("guide.ui.filter"),
                             s_filter, sizeof(s_filter));
    ImGui::Spacing();

    for (int c = 0; c < k_guide_chapter_count; c++) {
        const JceGuideChapter *ch = k_guide_chapters[c];
        const char *ch_title = jce_editor_i18n(ch->title_key);

        /* Filter: keep the chapter if its title or any topic title hits. */
        bool ch_hit = !s_filter[0] || jce_panel_contains_ci(ch_title, s_filter);
        bool any_topic_hit = false;
        if (s_filter[0]) {
            for (int t = 0; t < ch->topic_count && !any_topic_hit; t++)
                if (jce_panel_contains_ci(jce_editor_i18n(ch->topics[t].title_key),
                                s_filter))
                    any_topic_hit = true;
            if (!ch_hit && !any_topic_hit) continue;
        }

        ImGuiTreeNodeFlags tf = ImGuiTreeNodeFlags_SpanAvailWidth;
        if (c == s_sel_chapter || s_filter[0]) tf |= ImGuiTreeNodeFlags_DefaultOpen;
        bool open = ImGui::TreeNodeEx(ch_title, tf);
        if (!open) continue;
        for (int t = 0; t < ch->topic_count; t++) {
            const char *tt = jce_editor_i18n(ch->topics[t].title_key);
            if (s_filter[0] && !ch_hit && !jce_panel_contains_ci(tt, s_filter)) continue;
            bool selected = (c == s_sel_chapter && t == s_sel_topic);
            if (ImGui::Selectable(tt, selected)) {
                s_sel_chapter = c;
                s_sel_topic   = t;
            }
        }
        ImGui::TreePop();
    }

    /* Virtual hotkey-reference chapter. */
    {
        const char *hk_title = jce_editor_i18n("guide.hotkeys.title");
        if (!s_filter[0] || jce_panel_contains_ci(hk_title, s_filter)) {
            bool selected = (s_sel_chapter == k_hotkey_chapter_index);
            if (ImGui::Selectable(hk_title, selected)) {
                s_sel_chapter = k_hotkey_chapter_index;
                s_sel_topic   = 0;
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    /* Right: topic content. */
    ImGui::BeginChild("##guide_content", ImVec2(0.0f, 0.0f));
    if (s_sel_chapter == k_hotkey_chapter_index) {
        draw_hotkey_reference();
    } else if (s_sel_chapter >= 0 && s_sel_chapter < k_guide_chapter_count) {
        const JceGuideChapter *ch = k_guide_chapters[s_sel_chapter];
        if (s_sel_topic < 0 || s_sel_topic >= ch->topic_count)
            s_sel_topic = 0;
        if (ch->topic_count > 0) {
            const JceGuideTopic *topic = &ch->topics[s_sel_topic];
            ImGui::TextColored(JCE_COLOR_TEXT_PRIMARY, "%s",
                               jce_editor_i18n(topic->title_key));
            ImGui::TextDisabled("%s", jce_editor_i18n(ch->title_key));
            ImGui::Separator();
            ImGui::Spacing();
            draw_topic_blocks(topic);

            /* Prev / next topic navigation. */
            ImGui::Spacing();
            ImGui::Separator();
            if (s_sel_topic > 0) {
                char prev[128];
                snprintf(prev, sizeof(prev), "< %s###guide_prev",
                         jce_editor_i18n(ch->topics[s_sel_topic - 1].title_key));
                if (ImGui::SmallButton(prev)) s_sel_topic--;
                ImGui::SameLine();
            }
            if (s_sel_topic + 1 < ch->topic_count) {
                char next[128];
                snprintf(next, sizeof(next), "%s >###guide_next",
                         jce_editor_i18n(ch->topics[s_sel_topic + 1].title_key));
                if (ImGui::SmallButton(next)) s_sel_topic++;
            }
        }
    }
    ImGui::EndChild();
}
