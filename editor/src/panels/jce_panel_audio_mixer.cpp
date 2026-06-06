/*
 * jce_panel_audio_mixer.cpp  Audio Mixer window (Sprint 2 / 0.8.17)
 *
 * Editor-side authoring surface for the engine's hierarchical audio
 * mixer (jce_audio_mixer.h). Owns one JceAudioMixer instance per editor
 * session, persisted to .jce/audio_mixer.json, that the play-mode audio
 * engine can later consume to drive bus gains at runtime.
 *
 * UI mirrors Unity's "Audio Mixer" window:
 *   - Tree of buses (Master + arbitrary children) with name, volume,
 *     mute, solo per bus.
 *   - Add Group / Remove buttons.
 *   - Save / Reload buttons (auto-save on volume edits is intentional
 *     for ergonomics; tree mutations also auto-save).
 *
 * Default tree on first open:
 *     Master ─┬─ Music
 *             ├─ SFX
 *             ├─ Voice
 *             └─ UI
 */

#include "ui/jce_editor_colors.h"
#include "ui/jce_editor_tip.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_ui_state.h"
#include "io/jce_editor_file_util.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include <jce/middleware/audio/jce_audio_mixer.h>
#include <jce/middleware/scene/jce_scene.h>
}

#include "core/jce_editor_state.h"
#include "core/jce_editor_config.h"   /* jce_editor_dotjce_path (~/.jce) */

/* Per-user config dir (~/.jce); see jce_editor_dotjce_path. */
static const char *mixer_path(void) {
    static char p[1024]; static bool init = false;
    if (!init) { jce_editor_dotjce_path("audio_mixer.json", p, sizeof(p)); init = true; }
    return p;
}
#define MIXER_PATH mixer_path()

static JceAudioMixer *s_mixer            = nullptr;
static bool           s_initialized      = false;
static JceAudioBusId  s_selected_bus     = JCE_AUDIO_BUS_MASTER;
static char           s_rename_buf[64]   = {0};
static JceAudioBusId  s_renaming_bus     = JCE_AUDIO_BUS_INVALID;
static int            s_pending_focus_tab = -1; /* 0=mixer, 1=reverb, -1=none */
static int            s_current_tab       = 0;
static bool           s_tab_state_loaded  = false;

static const char *k_audio_tab_state_key = "panel.audio_mixer.current_tab";

static void ensure_tab_state_loaded(void)
{
    if (s_tab_state_loaded)
        return;
    s_current_tab =
        jce_editor_ui_state_load_int(k_audio_tab_state_key, 0, 0, 1);
    s_pending_focus_tab = s_current_tab;
    s_tab_state_loaded = true;
}

static void set_current_tab(int idx)
{
    if (idx < 0 || idx > 1 || s_current_tab == idx)
        return;
    s_current_tab = idx;
    if (s_tab_state_loaded)
        jce_editor_ui_state_save_int(k_audio_tab_state_key, idx);
}

/* ── Persistence ────────────────────────────────────────────────────── */

static void mixer_save(void)
{
    if (!s_mixer) return;
    JceAudioBusId ids[256] = {0};
    uint32_t      n        = jce_audio_mixer_list_buses(s_mixer, ids, 256);

    size_t cap = 64 + n * 160;
    char  *buf = (char *)ED_MALLOC(cap);
    if (!buf) return;
    size_t off = 0;
    int w = std::snprintf(buf + off, cap - off, "{\n  \"buses\": [\n");
    if (w < 0) { ED_FREE(buf); return; }
    off += (size_t)w;
    for (uint32_t i = 0; i < n; ++i) {
        JceAudioBusId id     = ids[i];
        JceAudioBusId parent = jce_audio_mixer_get_parent(s_mixer, id);
        const char   *name   = jce_audio_mixer_get_name(s_mixer, id);
        if (!name) name = "";
        w = std::snprintf(buf + off, cap - off,
            "    { \"id\": %u, \"parent\": %u, \"name\": \"%s\","
            " \"volume\": %.4f, \"muted\": %d, \"solo\": %d }%s\n",
            (unsigned)id, (unsigned)parent, name,
            (double)jce_audio_mixer_get_volume(s_mixer, id),
            jce_audio_mixer_is_muted(s_mixer, id) ? 1 : 0,
            jce_audio_mixer_is_solo(s_mixer, id)  ? 1 : 0,
            (i + 1 < n) ? "," : "");
        if (w < 0 || (size_t)w >= cap - off) { ED_FREE(buf); return; }
        off += (size_t)w;
    }
    w = std::snprintf(buf + off, cap - off, "  ]\n}\n");
    if (w < 0) { ED_FREE(buf); return; }
    off += (size_t)w;
    ed_write_file(MIXER_PATH, buf, off);
    ED_FREE(buf);
}

static void mixer_seed_default(void)
{
    /* Master is auto-created with id=1; just add common children. */
    jce_audio_mixer_add_bus(s_mixer, JCE_AUDIO_BUS_MASTER, "Music", 0.8f);
    jce_audio_mixer_add_bus(s_mixer, JCE_AUDIO_BUS_MASTER, "SFX",   1.0f);
    jce_audio_mixer_add_bus(s_mixer, JCE_AUDIO_BUS_MASTER, "Voice", 1.0f);
    jce_audio_mixer_add_bus(s_mixer, JCE_AUDIO_BUS_MASTER, "UI",    1.0f);
}

static bool mixer_load(void)
{
    size_t len = 0;
    char  *raw = (char *)ed_read_file(MIXER_PATH, &len);
    if (!raw) return false;
    if (len > (1 << 20)) { ED_FREE(raw); return false; }

    /* Minimal scanner: id, parent, name, volume, muted, solo per row. */
    const char *p = raw;
    while (p && *p) {
        const char *id_key = std::strstr(p, "\"id\"");
        if (!id_key) break;
        unsigned id = 0, parent = 0, mutedv = 0, solov = 0;
        float    vol = 1.0f;
        char     name[64] = {0};
        std::sscanf(id_key, "\"id\" : %u",  &id);
        if (id == 0) std::sscanf(id_key, "\"id\":%u", &id);

        const char *par_key = std::strstr(id_key, "\"parent\"");
        if (par_key) {
            std::sscanf(par_key, "\"parent\" : %u", &parent);
            if (parent == 0)
                std::sscanf(par_key, "\"parent\":%u", &parent);
        }
        const char *name_key = std::strstr(id_key, "\"name\"");
        if (name_key) {
            const char *q1 = std::strchr(name_key + 6, '"');
            const char *q2 = q1 ? std::strchr(q1 + 1, '"') : nullptr;
            if (q1 && q2) {
                size_t nl = (size_t)(q2 - q1 - 1);
                if (nl >= sizeof(name)) nl = sizeof(name) - 1;
                std::memcpy(name, q1 + 1, nl);
                name[nl] = 0;
            }
        }
        const char *vol_key = std::strstr(id_key, "\"volume\"");
        if (vol_key) {
            std::sscanf(vol_key, "\"volume\" : %f", &vol);
        }
        const char *mut_key = std::strstr(id_key, "\"muted\"");
        if (mut_key) std::sscanf(mut_key, "\"muted\" : %u", &mutedv);
        const char *sol_key = std::strstr(id_key, "\"solo\"");
        if (sol_key) std::sscanf(sol_key, "\"solo\" : %u", &solov);

        if (id == JCE_AUDIO_BUS_MASTER) {
            jce_audio_mixer_set_volume(s_mixer, JCE_AUDIO_BUS_MASTER, vol);
            jce_audio_mixer_set_muted (s_mixer, JCE_AUDIO_BUS_MASTER, mutedv != 0);
            jce_audio_mixer_set_solo  (s_mixer, JCE_AUDIO_BUS_MASTER, solov  != 0);
        } else if (id != 0 && name[0]) {
            JceAudioBusId par_id =
                (parent != 0) ? (JceAudioBusId)parent : JCE_AUDIO_BUS_MASTER;
            JceAudioBusId added =
                jce_audio_mixer_add_bus(s_mixer, par_id, name, vol);
            if (added != JCE_AUDIO_BUS_INVALID) {
                jce_audio_mixer_set_muted(s_mixer, added, mutedv != 0);
                jce_audio_mixer_set_solo (s_mixer, added, solov  != 0);
            }
        }
        p = (sol_key ? sol_key : (vol_key ? vol_key : id_key)) + 1;
    }
    ED_FREE(raw);
    return true;
}

static void ensure_mixer(void)
{
    if (s_initialized) return;
    s_initialized = true;
    if (!s_mixer) s_mixer = jce_audio_mixer_create();
    if (!s_mixer) return;
    if (!mixer_load()) {
        mixer_seed_default();
        mixer_save();
    }
}

/* ── Tree drawing ───────────────────────────────────────────────────── */

static int bus_depth(JceAudioBusId id)
{
    int depth = 0;
    while (id != JCE_AUDIO_BUS_MASTER && id != JCE_AUDIO_BUS_INVALID) {
        id = jce_audio_mixer_get_parent(s_mixer, id);
        if (++depth > 16) break;
    }
    return depth;
}

static void draw_bus_row(JceAudioBusId id)
{
    const char *name = jce_audio_mixer_get_name(s_mixer, id);
    if (!name) name = "(unnamed)";
    int   depth   = bus_depth(id);
    float volume  = jce_audio_mixer_get_volume(s_mixer, id);
    bool  muted   = jce_audio_mixer_is_muted (s_mixer, id);
    bool  solo    = jce_audio_mixer_is_solo  (s_mixer, id);
    float effective = jce_audio_mixer_resolve_volume(s_mixer, id);

    ImGui::PushID((int)id);
    ImGui::Indent(depth * 14.0f);

    bool selected = (s_selected_bus == id);
    char label[96];
    std::snprintf(label, sizeof(label), "%s##bus_sel", name);
    if (ImGui::Selectable(label, selected, 0, ImVec2(180, 0))) {
        s_selected_bus = id;
    }
    ImGui::SameLine();

    ImGui::SetNextItemWidth(160.0f);
    char vlabel[64];
    std::snprintf(vlabel, sizeof(vlabel), "##vol_%u", (unsigned)id);
    if (ImGui::SliderFloat(vlabel, &volume, 0.0f, 1.5f, "%.2f")) {
        jce_audio_mixer_set_volume(s_mixer, id, volume);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) mixer_save();

    ImGui::SameLine();
    if (ImGui::Checkbox("M", &muted)) {
        jce_audio_mixer_set_muted(s_mixer, id, muted);
        mixer_save();
    }
    jce_editor::help_tip(jce_editor_i18n("audioMixer.mute"));
    ImGui::SameLine();
    if (ImGui::Checkbox("S", &solo)) {
        jce_audio_mixer_set_solo(s_mixer, id, solo);
        mixer_save();
    }
    jce_editor::help_tip(jce_editor_i18n("audioMixer.solo"));

    ImGui::SameLine();
    ImGui::TextDisabled("%s %.2f", jce_editor_i18n("audioMixer.effective"),
                        (double)effective);

    ImGui::Unindent(depth * 14.0f);
    ImGui::PopID();
}

static void collect_children(JceAudioBusId parent,
                             const JceAudioBusId *all, uint32_t n,
                             std::vector<JceAudioBusId> &out)
{
    for (uint32_t i = 0; i < n; ++i) {
        if (all[i] == JCE_AUDIO_BUS_MASTER) continue;
        if (jce_audio_mixer_get_parent(s_mixer, all[i]) == parent) {
            out.push_back(all[i]);
            collect_children(all[i], all, n, out);
        }
    }
}

static void draw_mixer_tab(void)
{
    if (ImGui::Button(jce_editor_i18n("audioMixer.addGroup"))) {
        JceAudioBusId par = (s_selected_bus != JCE_AUDIO_BUS_INVALID)
                          ? s_selected_bus : JCE_AUDIO_BUS_MASTER;
        char name[32];
        std::snprintf(name, sizeof(name), "Group %u",
                      (unsigned)(jce_audio_mixer_bus_count(s_mixer)));
        JceAudioBusId nb = jce_audio_mixer_add_bus(s_mixer, par, name, 1.0f);
        if (nb != JCE_AUDIO_BUS_INVALID) {
            s_selected_bus = nb;
            mixer_save();
        }
    }
    ImGui::SameLine();
    bool can_remove = (s_selected_bus != JCE_AUDIO_BUS_INVALID &&
                       s_selected_bus != JCE_AUDIO_BUS_MASTER);
    ImGui::BeginDisabled(!can_remove);
    if (ImGui::Button(jce_editor_i18n("audioMixer.removeGroup"))) {
        if (jce_audio_mixer_remove_bus(s_mixer, s_selected_bus)) {
            s_selected_bus = JCE_AUDIO_BUS_MASTER;
            mixer_save();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_remove);
    if (ImGui::Button(jce_editor_i18n("audioMixer.rename"))) {
        const char *cur = jce_audio_mixer_get_name(s_mixer, s_selected_bus);
        std::snprintf(s_rename_buf, sizeof(s_rename_buf), "%s",
                      cur ? cur : "");
        s_renaming_bus = s_selected_bus;
        ImGui::OpenPopup("##rename_bus");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("audioMixer.reload"))) {
        jce_audio_mixer_destroy(s_mixer);
        s_mixer       = jce_audio_mixer_create();
        s_initialized = false;
        ensure_mixer();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("audioMixer.save"))) {
        mixer_save();
    }

    /* Rename popup. NOTE: jce_audio_mixer has no rename API today, so
     * we work around it by removing+re-adding under the same parent
     * (children stay attached because we walk siblings); preserving
     * non-master IDs isn't possible. Fall back to a TextDisabled note. */
    if (ImGui::BeginPopup("##rename_bus")) {
        ImGui::Text("%s", jce_editor_i18n("audioMixer.renameTitle"));
        ImGui::InputText("##rename_input", s_rename_buf,
                         sizeof(s_rename_buf));
        ImGui::TextDisabled("%s",
            jce_editor_i18n("audioMixer.renameNotWired"));
        if (ImGui::Button(jce_editor_i18n("audioMixer.close"))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();

    /* ── Bus list (depth-first from Master) ──────────────────────── */
    JceAudioBusId all[256] = {0};
    uint32_t      n        = jce_audio_mixer_list_buses(s_mixer, all, 256);

    /* Master row first, then DFS. */
    draw_bus_row(JCE_AUDIO_BUS_MASTER);
    std::vector<JceAudioBusId> ordered;
    ordered.reserve(n);
    collect_children(JCE_AUDIO_BUS_MASTER, all, n, ordered);
    for (JceAudioBusId id : ordered) draw_bus_row(id);

    ImGui::Separator();
    ImGui::TextDisabled("%s",
        jce_editor_i18n("audioMixer.runtimeNote"));
}

/* ── Reverb Zones tab (merged from jce_panel_reverb_zones.cpp in P6-A.1).
 * Lists every entity carrying a JceAudioReverbZoneComponent so the user
 * can audit and tune all reverb zones in one place. */

struct RZRow {
    JceEntity                    e;
    JceAudioReverbZoneComponent *c;
    char                         name[64];
};

static const char *kReverbPresetNames[] = {
    "Off","Generic","Padded Cell","Room","Bathroom","Living Room","Stone Room",
    "Auditorium","Concert Hall","Cave","Arena","Hangar","Hallway",
    "Stone Corridor","Alley","Forest","City","Mountains","Quarry","Plain",
    "Parking Lot","Sewer Pipe","Underwater","-","-","-","User"
};

static void rz_collect_cb(JceScene *s, JceEntity e, void *ud)
{
    auto *out = (std::vector<RZRow> *)ud;
    if (!jce_scene_has_audio_reverb_zone(s, e)) return;
    RZRow r{};
    r.e = e;
    r.c = jce_scene_get_audio_reverb_zone(s, e);
    JceEditorMeta *m = jce_scene_get_editor_meta(s, e);
    std::snprintf(r.name, sizeof(r.name), "%s",
                  (m && m->name[0]) ? m->name : "(unnamed)");
    out->push_back(r);
}

static void draw_reverb_zones_tab(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) {
        ImGui::TextDisabled("%s", jce_editor_i18n("common.noSceneLoaded"));
        return;
    }

    std::vector<RZRow> rows;
    jce_scene_each_entity(scene, rz_collect_cb, &rows);

    ImGui::Text("%s %zu", jce_editor_i18n("reverbZones.count"), rows.size());
    ImGui::Separator();
    if (rows.empty()) {
        ImGui::TextDisabled("%s", jce_editor_i18n("reverbZones.empty"));
        return;
    }

    if (ImGui::BeginTable("##rz_tbl", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Owner",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Preset",  ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn("Min",     ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Max",     ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableHeadersRow();

        for (auto &r : rows) {
            if (!r.c) continue;
            ImGui::PushID((int)r.e);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.name);

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            int idx = r.c->preset;
            int n = (int)(sizeof(kReverbPresetNames) /
                          sizeof(kReverbPresetNames[0]));
            if (idx < 0 || idx >= n) idx = 0;
            if (ImGui::Combo("##pre", &idx, kReverbPresetNames, n))
                r.c->preset = idx;

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##mn", &r.c->min_distance,
                             0.1f, 0.0f, 100000.0f, "%.1f");

            ImGui::TableSetColumnIndex(3);
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##mx", &r.c->max_distance,
                             0.1f, 0.0f, 100000.0f, "%.1f");

            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton(jce_editor_i18n("common.ping")))
                jce_state_select_entity((uint32_t)r.e, false);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

extern "C" void jce_editor_panel_audio_mixer_content(void)
{
    ensure_mixer();
    if (!s_mixer) {
        ImGui::TextDisabled("%s", jce_editor_i18n("audioMixer.unavailable"));
        return;
    }

    ensure_tab_state_loaded();
    if (ImGui::BeginTabBar("##audio_mixer_tabs")) {
        ImGuiTabItemFlags mixer_flags = (s_pending_focus_tab == 0)
            ? ImGuiTabItemFlags_SetSelected : 0;
        ImGuiTabItemFlags reverb_flags = (s_pending_focus_tab == 1)
            ? ImGuiTabItemFlags_SetSelected : 0;
        s_pending_focus_tab = -1;
        if (ImGui::BeginTabItem(jce_editor_i18n("audioMixer.tab.mixer"),
                                nullptr, mixer_flags)) {
            set_current_tab(0);
            draw_mixer_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(jce_editor_i18n("audioMixer.tab.reverb"),
                                nullptr, reverb_flags)) {
            set_current_tab(1);
            draw_reverb_zones_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

extern "C" void jce_editor_audio_mixer_focus_reverb_tab(void)
{
    s_pending_focus_tab = 1;
    s_current_tab = 1;
    jce_editor_ui_state_save_int(k_audio_tab_state_key, 1);
}

extern "C" void jce_editor_panel_audio_mixer(void)
{
    /* Reserved for future toolbar/integration logic. */
}
