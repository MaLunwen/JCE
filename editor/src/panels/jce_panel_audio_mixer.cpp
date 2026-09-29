/*
 * jce_panel_audio_mixer.cpp  Audio Mixer window (Sprint 2 / 0.8.17)
 *
 * Editor-side authoring surface for the engine's hierarchical audio
 * mixer (jce_audio_mixer.h). Owns one JceAudioMixer instance per open
 * project, persisted to <root>/Settings/audio_mixer.json (game content —
 * ships with the game, version-controllable), that the play-mode audio
 * engine and the standalone runtime consume to drive bus gains.
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

#include "jce_panel_common.h"
#include "ui/jce_editor_colors.h"
#include "ui/jce_editor_tip.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_ui_state.h"
#include "io/jce_editor_file_util.h"

#include <jce/tools/jce_imgui.hpp>
#include <jce/os/core/jce_json.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <jce/middleware/audio/jce_audio_mixer.h>
#include <jce/middleware/audio/jce_audio_dsp.h>
#include <jce/middleware/audio/jce_audio_mixer_config.h>
#include <jce/middleware/scene/jce_scene.h>
}

#include "core/jce_editor_state.h"
#include "core/jce_editor_config.h"   /* jce_editor_dotjce_path (legacy ~/.jce) */

/* Open-project root — owned by dialog_project.cpp (same explicit-root
 * pattern as jce_project_settings / jce_pak_key).  Declared at global
 * scope: inside an anonymous namespace the extern would acquire internal
 * linkage and never bind to the definition. */
extern char s_current_project_root[512];

/* Mixer routing is game CONTENT: it belongs to the project (version control,
 * shipped builds), so it lives beside Settings/RenderPipeline.rp.json under
 * the OPEN project's root, resolved through the explicit root — the editor
 * never chdirs.  Without a project we fall back to the legacy per-user
 * ~/.jce copy so behaviour there is unchanged. */
static const char *mixer_path(void) {
    static char p[1024];
    if (s_current_project_root[0])
        std::snprintf(p, sizeof(p), "%s/Settings/audio_mixer.json",
                      s_current_project_root);
    else
        jce_editor_dotjce_path("audio_mixer.json", p, sizeof(p));
    return p;
}
#define MIXER_PATH mixer_path()

static JceAudioMixer *s_mixer            = nullptr;
static bool           s_initialized      = false;
static JceAudioBusId  s_selected_bus     = JCE_AUDIO_BUS_MASTER;
static char           s_rename_buf[64]   = {0};
static JceAudioBusId  s_renaming_bus     = JCE_AUDIO_BUS_INVALID;
/* Tabs: 0=mixer, 1=snapshots, 2=reverb. */
static JcePanelTabState s_tabs{ "panel.audio_mixer.current_tab", /*max_tab=*/2 };
static char           s_snapshot_buf[JCE_AUDIO_SNAPSHOT_NAME] = {0};
static float          s_snapshot_fade     = 1.0f;

/* Insert-effect chains live on the live JceAudio device (attached by bus
 * NAME via jce_audio_bus_add_effect), not on the JceAudioMixer bookkeeping
 * struct.  So the editor keeps the authored chain here, keyed by bus id, and
 * serializes it into the same audio_mixer.json so the runtime can re-attach
 * each bus's chain by name at play start. */
static std::map<JceAudioBusId, std::vector<JceAudioEffectDesc>> s_bus_effects;

/* ── Persistence ────────────────────────────────────────────────────── */

/* Build one effect-desc object node.
 *
 * FORWARDS to the engine.  This used to be a switch with its own field list
 * and its own type spellings, beside a parse_effect that was a second copy of
 * the engine's -- and adding three effects to the palette is exactly the
 * change that leaves one of those behind.  The one that gets left behind is
 * usually the reader, so the editor saves an effect the runtime drops without
 * a word. */
static JceJson *effect_to_json(const JceAudioEffectDesc &d)
{
    return jce_audio_effect_to_json(&d);
}


static void mixer_save(void)
{
    if (!s_mixer) return;
    JceAudioBusId ids[256] = {0};
    uint32_t      n        = jce_audio_mixer_list_buses(s_mixer, ids, 256);

    /* Built as a JceJson tree, not printf'd text: bus and snapshot names are
     * user-typed, and a name carrying a '"' or '\\' used to be pasted raw into
     * the file (corrupting it / injecting keys).  The facade escapes. */
    JceJson *root = jce_json_object();
    if (!root) return;
    /* "version" gives future breaking schema changes something to gate on;
     * both readers (mixer_load + engine jce_audio_mixer_config.c) default
     * every field, so files with or without it load identically. */
    jce_json_set_int(root, "version", 1);

    JceJson *buses = jce_json_array();
    if (!buses) { jce_json_free(root); return; }
    jce_json_set_child(root, "buses", buses);   /* buses now owned by root */

    for (uint32_t i = 0; i < n; ++i) {
        JceAudioBusId id     = ids[i];
        JceAudioBusId parent = jce_audio_mixer_get_parent(s_mixer, id);
        const char   *name   = jce_audio_mixer_get_name(s_mixer, id);
        JceJson      *b      = jce_json_object();
        if (!b) { jce_json_free(root); return; }
        jce_json_array_push(buses, b);          /* b now owned by buses */
        jce_json_set_int   (b, "id",     (int)id);
        jce_json_set_int   (b, "parent", (int)parent);
        jce_json_set_string(b, "name",   name ? name : "");
        jce_json_set_number(b, "volume",
                            (double)jce_audio_mixer_get_volume(s_mixer, id));
        /* muted/solo stay 0/1 NUMBERS so files written here still load in
         * editors/runtimes that predate this rewrite. */
        jce_json_set_int   (b, "muted",
                            jce_audio_mixer_is_muted(s_mixer, id) ? 1 : 0);
        jce_json_set_int   (b, "solo",
                            jce_audio_mixer_is_solo(s_mixer, id)  ? 1 : 0);

        /* Aux sends from this bus.  Enumerated (send_at), not probed via
         * get_send: get_send returns 0 for both "no send" and "registered
         * but silent", so probing silently dropped amount-0 sends on save. */
        uint32_t send_n = jce_audio_mixer_send_count(s_mixer, id);
        JceJson *sends  = nullptr;
        for (uint32_t j = 0; j < send_n; ++j) {
            JceAudioBusId dest = JCE_AUDIO_BUS_INVALID;
            float amt = 0.0f;
            if (!jce_audio_mixer_send_at(s_mixer, id, j, &dest, &amt)) break;
            if (!sends) {
                sends = jce_json_array();
                if (!sends) break;
                jce_json_set_child(b, "sends", sends);
            }
            JceJson *s = jce_json_object();
            if (!s) break;
            jce_json_array_push(sends, s);
            jce_json_set_int   (s, "dest",   (int)dest);
            jce_json_set_number(s, "amount", (double)amt);
        }

        /* Sidechain installed on this (target) bus. */
        JceAudioDuckParams dp;
        if (jce_audio_mixer_get_sidechain(s_mixer, id, &dp)) {
            JceJson *sc = jce_json_object();
            if (sc) {
                jce_json_set_child (b,  "sidechain", sc);
                jce_json_set_int   (sc, "key",          (int)dp.key);
                jce_json_set_number(sc, "threshold_db", (double)dp.threshold_db);
                jce_json_set_number(sc, "ratio",        (double)dp.ratio);
                jce_json_set_number(sc, "attack_ms",    (double)dp.attack_ms);
                jce_json_set_number(sc, "release_ms",   (double)dp.release_ms);
                jce_json_set_number(sc, "floor_db",
                                    (double)dp.max_attenuation_db);
            }
        }

        /* Insert-effect chain (editor-side model). */
        auto it = s_bus_effects.find(id);
        if (it != s_bus_effects.end() && !it->second.empty()) {
            JceJson *fx = jce_json_array();
            if (fx) {
                jce_json_set_child(b, "effects", fx);
                for (const JceAudioEffectDesc &d : it->second)
                    jce_json_array_push(fx, effect_to_json(d));
            }
        }
    }

    /* Top-level named snapshots. */
    uint32_t snap_n = jce_audio_mixer_snapshot_count(s_mixer);
    if (snap_n > 0) {
        JceJson *snaps = jce_json_array();
        if (snaps) {
            jce_json_set_child(root, "snapshots", snaps);
            for (uint32_t si = 0; si < snap_n; ++si) {
                char sname[JCE_AUDIO_SNAPSHOT_NAME] = {0};
                if (!jce_audio_mixer_snapshot_name(s_mixer, si, sname,
                                                   sizeof(sname)))
                    continue;
                JceJson *sn = jce_json_object();
                if (!sn) break;
                jce_json_array_push(snaps, sn);
                jce_json_set_string(sn, "name", sname);
                JceJson *vols = jce_json_array();
                if (!vols) continue;
                jce_json_set_child(sn, "volumes", vols);
                for (uint32_t i = 0; i < n; ++i) {
                    float v = jce_audio_mixer_snapshot_get_volume(s_mixer,
                                                                  sname, ids[i]);
                    if (v < 0.0f) continue;
                    JceJson *e = jce_json_object();
                    if (!e) break;
                    jce_json_array_push(vols, e);
                    jce_json_set_int   (e, "id",     (int)ids[i]);
                    jce_json_set_number(e, "volume", (double)v);
                }
            }
        }
    }

    char *text = jce_json_print(root, /*pretty=*/true);
    jce_json_free(root);
    if (!text) return;

    /* Project Settings/ dir may not exist yet (fresh project). */
    if (s_current_project_root[0]) {
        char dir[1024];
        std::snprintf(dir, sizeof(dir), "%s/Settings", s_current_project_root);
        jce_fs_host_create_directory(dir);
    }
    /* Atomic (temp+rename): a mid-write crash must not corrupt the mixer
     * file — it seeds the runtime mixer at Play start and bundle builds. */
    jce_fs_host_write_all_atomic(MIXER_PATH, text, std::strlen(text));
    jce_json_free_string(text);
}

/* Deferred-save latch: FX-insert and sidechain DragFloats report a change on
 * EVERY drag frame; saving per change frame rewrote Settings/audio_mixer.json
 * continuously during a drag (full serialize + disk write per frame).  Latch
 * instead and flush ONCE when the drag releases (no active item).  Structural
 * edits (add/remove/reorder/enable) keep their immediate saves.  Live audio
 * behaviour is unchanged — engine-side jce_audio_mixer_set_* still runs per
 * change frame; only the disk write is deferred. */
static bool s_param_save_pending = false;

static void mixer_flush_param_saves(void)
{
    if (s_param_save_pending && !ImGui::IsAnyItemActive()) {
        s_param_save_pending = false;
        mixer_save();
    }
}

static void mixer_seed_default(void)
{
    /* Master is auto-created with id=1; just add common children. */
    jce_audio_mixer_add_bus(s_mixer, JCE_AUDIO_BUS_MASTER, "Music", 0.8f);
    jce_audio_mixer_add_bus(s_mixer, JCE_AUDIO_BUS_MASTER, "SFX",   1.0f);
    jce_audio_mixer_add_bus(s_mixer, JCE_AUDIO_BUS_MASTER, "Voice", 1.0f);
    jce_audio_mixer_add_bus(s_mixer, JCE_AUDIO_BUS_MASTER, "UI",    1.0f);
}

/* Parse one effect-desc object.  FORWARDS to the engine -- see
 * effect_to_json above for why there is no longer a copy here. */
static bool parse_effect(const JceJson *e, JceAudioEffectDesc &d)
{
    return jce_audio_effect_from_json(e, &d);
}


static bool mixer_load(void)
{
    size_t len = 0;
    char  *raw = (char *)ed_read_file(MIXER_PATH, &len);
    if (!raw) return false;
    if (len > (1 << 20)) { ED_FREE(raw); return false; }
    s_bus_effects.clear();

    JceJson *root = jce_json_parse(raw, len);
    ED_FREE(raw);
    /* The file EXISTS, so report loaded even when it is unparseable or carries
     * no buses array: returning false makes ensure_mixer seed defaults and
     * immediately mixer_save() over whatever the user had there. */
    if (!root) return true;
    JceJson *buses = jce_json_get(root, "buses");
    if (!jce_json_is_array(buses)) { jce_json_free(root); return true; }

    /* Map a parsed JSON id -> the live bus id actually assigned. Buses are
     * created in file order, so deferred send/sidechain refs (which name
     * JSON ids) resolve through this table in a second pass. */
    std::map<unsigned, JceAudioBusId> id_map;
    id_map[JCE_AUDIO_BUS_MASTER] = JCE_AUDIO_BUS_MASTER;

    /* Record per-bus deferred refs for pass 2 (the bus's JSON node). */
    struct BusObj { JceAudioBusId live; const JceJson *node; };
    std::vector<BusObj> objs;

    /* Pass 1: create every bus. */
    const int bus_n = jce_json_array_size(buses);
    for (int i = 0; i < bus_n; ++i) {
        const JceJson *b = jce_json_array_at(buses, i);
        if (!jce_json_is_object(b)) continue;
        unsigned    id     = (unsigned)jce_json_get_int(b, "id",     0);
        unsigned    parent = (unsigned)jce_json_get_int(b, "parent", 0);
        float       vol    = (float)jce_json_get_number(b, "volume", 1.0);
        /* muted/solo are written as 0/1 numbers; get_bool takes numbers and
         * real booleans alike, so old and future files both read back. */
        bool        muted  = jce_json_get_bool(b, "muted", false);
        bool        solo   = jce_json_get_bool(b, "solo",  false);
        const char *name   = jce_json_get_string(b, "name", "");

        JceAudioBusId live = JCE_AUDIO_BUS_INVALID;
        if (id == JCE_AUDIO_BUS_MASTER) {
            jce_audio_mixer_set_volume(s_mixer, JCE_AUDIO_BUS_MASTER, vol);
            jce_audio_mixer_set_muted (s_mixer, JCE_AUDIO_BUS_MASTER, muted);
            jce_audio_mixer_set_solo  (s_mixer, JCE_AUDIO_BUS_MASTER, solo);
            live = JCE_AUDIO_BUS_MASTER;
        } else if (id != 0 && name[0]) {
            unsigned par = parent ? parent : JCE_AUDIO_BUS_MASTER;
            auto pit = id_map.find(par);
            JceAudioBusId par_id = (pit != id_map.end())
                ? pit->second : JCE_AUDIO_BUS_MASTER;
            live = jce_audio_mixer_add_bus(s_mixer, par_id, name, vol);
            if (live != JCE_AUDIO_BUS_INVALID) {
                jce_audio_mixer_set_muted(s_mixer, live, muted);
                jce_audio_mixer_set_solo (s_mixer, live, solo);
            }
        }
        if (live != JCE_AUDIO_BUS_INVALID) {
            id_map[id] = live;
            objs.push_back({ live, b });

            /* Insert-effect chain (resolved immediately; no id refs). */
            const JceJson *fx = jce_json_get(b, "effects");
            if (jce_json_is_array(fx)) {
                std::vector<JceAudioEffectDesc> &dst = s_bus_effects[live];
                const int fx_n = jce_json_array_size(fx);
                for (int k = 0; k < fx_n; ++k) {
                    const JceJson *e = jce_json_array_at(fx, k);
                    if (!jce_json_is_object(e)) continue;
                    JceAudioEffectDesc d{};
                    if (parse_effect(e, d)) dst.push_back(d);
                }
            }
        }
    }

    /* Pass 2: sends + sidechain (need the full id_map resolved). */
    for (const BusObj &b : objs) {
        /* Sends: iterate { dest, amount } objects inside "sends":[...]. */
        const JceJson *sends  = jce_json_get(b.node, "sends");
        const int      send_n = jce_json_array_size(sends);
        for (int si = 0; si < send_n; ++si) {
            const JceJson *s = jce_json_array_at(sends, si);
            if (!jce_json_is_object(s)) continue;
            unsigned dest = (unsigned)jce_json_get_int(s, "dest",   0);
            float    amt  = (float)jce_json_get_number(s, "amount", 0.0);
            auto dit = id_map.find(dest);
            /* amount 0 = registered-but-silent (valid authored state);
             * reject only negative garbage. */
            if (dit != id_map.end() && amt >= 0.0f)
                jce_audio_mixer_set_send(s_mixer, b.live, dit->second, amt);
        }
        /* Sidechain object. */
        const JceJson *sc = jce_json_get(b.node, "sidechain");
        if (jce_json_is_object(sc)) {
            JceAudioDuckParams dp = jce_audio_duck_default_params();
            unsigned key = (unsigned)jce_json_get_int(sc, "key", 0);
            dp.threshold_db =
                (float)jce_json_get_number(sc, "threshold_db", dp.threshold_db);
            dp.ratio =
                (float)jce_json_get_number(sc, "ratio",        dp.ratio);
            dp.attack_ms =
                (float)jce_json_get_number(sc, "attack_ms",    dp.attack_ms);
            dp.release_ms =
                (float)jce_json_get_number(sc, "release_ms",   dp.release_ms);
            dp.max_attenuation_db =
                (float)jce_json_get_number(sc, "floor_db", dp.max_attenuation_db);
            auto kit = id_map.find(key);
            if (kit != id_map.end()) {
                dp.key = kit->second;
                jce_audio_mixer_set_sidechain(s_mixer, b.live, &dp, 48000u);
            }
        }
    }

    /* Snapshots array (top-level). */
    const JceJson *snaps  = jce_json_get(root, "snapshots");
    const int      snap_n = jce_json_array_size(snaps);
    for (int si = 0; si < snap_n; ++si) {
        const JceJson *s = jce_json_array_at(snaps, si);
        if (!jce_json_is_object(s)) continue;
        /* The mixer stores snapshot names in a fixed 32-byte field, so a
         * longer authored name must be truncated the same way here or the
         * volumes below would key a second, differently-named snapshot. */
        char sname[JCE_AUDIO_SNAPSHOT_NAME] = {0};
        std::snprintf(sname, sizeof(sname), "%s",
                      jce_json_get_string(s, "name", ""));
        if (!sname[0]) continue;
        /* volumes:[ { id, volume } ] */
        const JceJson *vols  = jce_json_get(s, "volumes");
        const int      vol_n = jce_json_array_size(vols);
        for (int vi = 0; vi < vol_n; ++vi) {
            const JceJson *v = jce_json_array_at(vols, vi);
            if (!jce_json_is_object(v)) continue;
            unsigned vid = (unsigned)jce_json_get_int(v, "id",     0);
            float    vv  = (float)jce_json_get_number(v, "volume", 0.0);
            auto vit = id_map.find(vid);
            if (vit != id_map.end())
                jce_audio_mixer_snapshot_set_volume(s_mixer, sname,
                                                    vit->second, vv);
        }
    }

    jce_json_free(root);
    return true;
}

/* One-time forward-migration: older editors stored the mixer per-user in
 * ~/.jce/audio_mixer.json, where two projects clobbered each other.  If the
 * open project has no copy yet but the legacy one exists, copy it forward,
 * then rename the old file *.migrated so it can never shadow project copies
 * (a second project migrating later starts from its own defaults instead of
 * inheriting the first project's routing). */
static void migrate_legacy_mixer(void)
{
    if (!s_current_project_root[0]) return;   /* no project: legacy IS the store */
    if (jce_fs_host_exists_file(MIXER_PATH)) return;   /* project copy wins */
    char legacy[1024];
    jce_editor_dotjce_path("audio_mixer.json", legacy, sizeof(legacy));
    if (!legacy[0] || !jce_fs_host_exists_file(legacy)) return;
    char dir[1024];
    std::snprintf(dir, sizeof(dir), "%s/Settings", s_current_project_root);
    jce_fs_host_create_directory(dir);
    if (!jce_fs_host_copy_file(legacy, MIXER_PATH)) return;
    char moved[1040];
    std::snprintf(moved, sizeof(moved), "%s.migrated", legacy);
    jce_fs_host_rename(legacy, moved);
}

/* Project root the current mixer tree was loaded for. */
static char s_mixer_root[512] = {0};

static void ensure_mixer(void)
{
    /* The store is project-scoped: follow the open project so a project
     * switch drops the previous project's tree instead of writing it into
     * the new project's file. */
    if (s_initialized &&
        std::strcmp(s_mixer_root, s_current_project_root) != 0) {
        s_initialized = false;
        if (s_mixer) { jce_audio_mixer_destroy(s_mixer); s_mixer = nullptr; }
        s_bus_effects.clear();
        s_selected_bus = JCE_AUDIO_BUS_MASTER;
    }
    if (s_initialized) return;
    s_initialized = true;
    std::snprintf(s_mixer_root, sizeof(s_mixer_root), "%s",
                  s_current_project_root);
    if (!s_mixer) s_mixer = jce_audio_mixer_create();
    if (!s_mixer) return;
    migrate_legacy_mixer();
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

/* ── Bus inspector: insert-effect chain + aux sends + sidechain ─────── */

/* THESE TWO ARRAYS ARE PARALLEL AND MUST STAY THAT WAY: the combo returns an
 * index into the names and the code reads the same index out of the values,
 * so a name added without its value silently authors the WRONG effect. */
static const char *kEffectTypeNames[] = {
    "EQ", "Compressor", "Limiter", "Delay", "Chorus", "Flanger", "Distortion"
};
static const JceAudioEffectType kEffectTypeVals[] = {
    JCE_AUDIO_EFFECT_EQ, JCE_AUDIO_EFFECT_COMPRESSOR,
    JCE_AUDIO_EFFECT_LIMITER, JCE_AUDIO_EFFECT_DELAY,
    JCE_AUDIO_EFFECT_CHORUS, JCE_AUDIO_EFFECT_FLANGER,
    JCE_AUDIO_EFFECT_DISTORTION
};
static_assert(IM_ARRAYSIZE(kEffectTypeNames) == IM_ARRAYSIZE(kEffectTypeVals),
              "effect name/value arrays must stay parallel -- a mismatched "
              "index authors a different effect than the one displayed");

static const char *kDistShapeNames[] = { "Soft Clip", "Hard Clip", "Foldback" };
static const char *kEqShapeNames[] = {
    "Peaking", "Low Shelf", "High Shelf", "Low Pass", "High Pass"
};

static void draw_effect_params(JceAudioEffectDesc &d, bool &changed)
{
    switch (d.type) {
    case JCE_AUDIO_EFFECT_EQ: {
        int shape = (int)d.u.eq.shape;
        ImGui::SetNextItemWidth(140);
        if (ImGui::Combo(jce_editor_i18n("audioMixer.fx.eqShape"), &shape,
                         kEqShapeNames, IM_ARRAYSIZE(kEqShapeNames))) {
            d.u.eq.shape = (JceAudioEqShape)shape; changed = true;
        }
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.freq"),
                             &d.u.eq.frequency_hz, 10.0f, 20.0f, 22000.0f, "%.0f Hz"))
            changed = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.gainDb"),
                             &d.u.eq.gain_db, 0.1f, -24.0f, 24.0f, "%.1f dB"))
            changed = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.q"),
                             &d.u.eq.q, 0.01f, 0.1f, 10.0f, "%.3f"))
            changed = true;
        break;
    }
    case JCE_AUDIO_EFFECT_COMPRESSOR:
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.threshold"),
                             &d.u.comp.threshold_db, 0.1f, -60.0f, 0.0f, "%.1f dB")) changed = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.ratio"),
                             &d.u.comp.ratio, 0.1f, 1.0f, 20.0f, "%.1f:1")) changed = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.attack"),
                             &d.u.comp.attack_ms, 0.1f, 0.0f, 500.0f, "%.1f ms")) changed = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.release"),
                             &d.u.comp.release_ms, 1.0f, 0.0f, 2000.0f, "%.0f ms")) changed = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.makeup"),
                             &d.u.comp.makeup_db, 0.1f, 0.0f, 24.0f, "%.1f dB")) changed = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.knee"),
                             &d.u.comp.knee_db, 0.1f, 0.0f, 24.0f, "%.1f dB")) changed = true;
        break;
    case JCE_AUDIO_EFFECT_LIMITER:
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.ceiling"),
                             &d.u.limiter.ceiling_db, 0.1f, -24.0f, 0.0f, "%.1f dB")) changed = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.release"),
                             &d.u.limiter.release_ms, 1.0f, 0.0f, 1000.0f, "%.0f ms")) changed = true;
        break;
    case JCE_AUDIO_EFFECT_DELAY:
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.delayMs"),
                             &d.u.delay.delay_ms, 1.0f, 0.0f, 2000.0f, "%.0f ms")) changed = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.feedback"),
                             &d.u.delay.feedback, 0.01f, 0.0f, 0.95f, "%.2f")) changed = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.wet"),
                             &d.u.delay.wet, 0.01f, 0.0f, 1.0f, "%.2f")) changed = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.dry"),
                             &d.u.delay.dry, 0.01f, 0.0f, 1.0f, "%.2f")) changed = true;
        break;
    /* CHORUS AND FLANGER share a parameter struct, so they share this editor.
     * The RANGES differ though: a flanger lives in single-digit milliseconds
     * and a chorus in the twenties, and one slider spanning both makes the
     * flanger's whole useful range about four pixels wide. */
    case JCE_AUDIO_EFFECT_CHORUS:
    case JCE_AUDIO_EFFECT_FLANGER: {
        const bool flanger = (d.type == JCE_AUDIO_EFFECT_FLANGER);
        const float max_delay = flanger ? 20.0f : 60.0f;
        const float max_depth = flanger ? 10.0f : 30.0f;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.delayMs"),
                             &d.u.mod_delay.delay_ms, 0.1f, 0.1f, max_delay, "%.2f ms")) changed = true;
        if (ImGui::DragFloat(jce_editor_i18n_id("audioMixer.fx.depthMs", "Depth"),
                             &d.u.mod_delay.depth_ms, 0.1f, 0.0f, max_depth, "%.2f ms")) changed = true;
        if (ImGui::DragFloat(jce_editor_i18n_id("audioMixer.fx.rateHz", "Rate"),
                             &d.u.mod_delay.rate_hz, 0.01f, 0.0f, 20.0f, "%.2f Hz")) changed = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.feedback"),
                             &d.u.mod_delay.feedback, 0.01f, 0.0f, 0.95f, "%.2f")) changed = true;
        if (ImGui::DragFloat(jce_editor_i18n_id("audioMixer.fx.stereoPhase", "Stereo Phase"),
                             &d.u.mod_delay.stereo_phase, 0.01f, 0.0f, 1.0f, "%.2f")) changed = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.wet"),
                             &d.u.mod_delay.wet, 0.01f, 0.0f, 1.0f, "%.2f")) changed = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.dry"),
                             &d.u.mod_delay.dry, 0.01f, 0.0f, 1.0f, "%.2f")) changed = true;
        break;
    }
    case JCE_AUDIO_EFFECT_DISTORTION: {
        int shape = (int)d.u.distortion.shape;
        ImGui::SetNextItemWidth(140);
        if (ImGui::Combo(jce_editor_i18n_id("audioMixer.fx.distShape", "Shape"),
                         &shape, kDistShapeNames, IM_ARRAYSIZE(kDistShapeNames))) {
            d.u.distortion.shape = (JceAudioDistortionShape)shape; changed = true;
        }
        if (ImGui::DragFloat(jce_editor_i18n_id("audioMixer.fx.drive", "Drive"),
                             &d.u.distortion.drive, 0.1f, 1.0f, 50.0f, "%.1f")) changed = true;
        if (ImGui::DragFloat(jce_editor_i18n_id("audioMixer.fx.distCeiling", "Ceiling"),
                             &d.u.distortion.ceiling, 0.01f, 0.05f, 2.0f, "%.2f")) changed = true;
        if (ImGui::DragFloat(jce_editor_i18n_id("audioMixer.fx.outputGain", "Output"),
                             &d.u.distortion.output_gain, 0.01f, 0.0f, 2.0f, "%.2f")) changed = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.wet"),
                             &d.u.distortion.wet, 0.01f, 0.0f, 1.0f, "%.2f")) changed = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.fx.dry"),
                             &d.u.distortion.dry, 0.01f, 0.0f, 1.0f, "%.2f")) changed = true;
        break;
    }
    default:
        /* A REGISTERED effect: the engine knows its name and nothing about
         * its parameters, so there is no honest editor to draw.  Said, rather
         * than an empty pane that reads like a broken panel. */
        if ((int)d.type >= JCE_AUDIO_EFFECT_CUSTOM_BASE) {
            const char *nm = jce_audio_dsp_effect_name((int)d.type);
            ImGui::TextDisabled("%s", jce_editor_i18n_id(
                "audioMixer.fx.customNoParams",
                "registered by the project; its parameters are not editable here"));
            if (nm) ImGui::TextDisabled("(%s)", nm);
        }
        break;
    }
}

/* Insert-effect chain for the selected bus. */
static void draw_inserts_section(JceAudioBusId bus)
{
    ImGui::SeparatorText(jce_editor_i18n("audioMixer.inserts"));
    auto &chain = s_bus_effects[bus];
    bool dirty = false;

    for (size_t i = 0; i < chain.size(); ++i) {
        ImGui::PushID((int)i);
        const char *tn = "?";
        for (int t = 0; t < IM_ARRAYSIZE(kEffectTypeVals); ++t)
            if (kEffectTypeVals[t] == chain[i].type) tn = kEffectTypeNames[t];
        char hdr[64];
        std::snprintf(hdr, sizeof(hdr), "%u. %s", (unsigned)i, tn);
        if (ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_DefaultOpen)) {
            bool param_edit = false;
            draw_effect_params(chain[i], param_edit);
            if (param_edit) s_param_save_pending = true;
            ImGui::BeginDisabled(i == 0);
            if (ImGui::SmallButton(jce_editor_i18n("audioMixer.fx.up"))) {
                std::swap(chain[i], chain[i - 1]); dirty = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(i + 1 >= chain.size());
            if (ImGui::SmallButton(jce_editor_i18n("audioMixer.fx.down"))) {
                std::swap(chain[i], chain[i + 1]); dirty = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton(jce_editor_i18n("audioMixer.fx.remove"))) {
                chain.erase(chain.begin() + (long)i);
                dirty = true;
                ImGui::TreePop();
                ImGui::PopID();
                break;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    static int s_add_fx_type = 0;
    ImGui::SetNextItemWidth(140);
    ImGui::Combo("##add_fx_type", &s_add_fx_type, kEffectTypeNames,
                 IM_ARRAYSIZE(kEffectTypeNames));
    ImGui::SameLine();
    bool full = chain.size() >= JCE_AUDIO_DSP_MAX_EFFECTS;
    ImGui::BeginDisabled(full);
    if (ImGui::Button(jce_editor_i18n("audioMixer.fx.add"))) {
        chain.push_back(jce_audio_effect_default(kEffectTypeVals[s_add_fx_type]));
        dirty = true;
    }
    ImGui::EndDisabled();
    if (full) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", jce_editor_i18n("audioMixer.fx.full"));
    }

    if (dirty) mixer_save();
}

/* True when `src` has a REGISTERED send to `dest`.  get_send can't answer
 * this: it returns 0 for both "no send" and "registered but silent". */
static bool bus_has_send(JceAudioBusId src, JceAudioBusId dest)
{
    uint32_t sn = jce_audio_mixer_send_count(s_mixer, src);
    for (uint32_t i = 0; i < sn; ++i) {
        JceAudioBusId d = JCE_AUDIO_BUS_INVALID;
        if (jce_audio_mixer_send_at(s_mixer, src, i, &d, nullptr) && d == dest)
            return true;
    }
    return false;
}

/* Aux sends originating from the selected bus. */
static void draw_sends_section(JceAudioBusId bus,
                               const JceAudioBusId *all, uint32_t n)
{
    ImGui::SeparatorText(jce_editor_i18n("audioMixer.sends"));
    bool dirty = false;

    for (uint32_t i = 0; i < n; ++i) {
        JceAudioBusId dest = all[i];
        if (dest == bus) continue;
        /* Registration, not amount, decides visibility: a send dialled to 0
         * stays editable instead of vanishing (and being lost on save). */
        if (!bus_has_send(bus, dest)) continue;
        float amt = jce_audio_mixer_get_send(s_mixer, bus, dest);
        ImGui::PushID((int)dest);
        const char *dn = jce_audio_mixer_get_name(s_mixer, dest);
        ImGui::TextUnformatted(dn ? dn : "?");
        ImGui::SameLine(160);
        ImGui::SetNextItemWidth(160);
        if (ImGui::SliderFloat("##send_amt", &amt, 0.0f, 1.0f, "%.2f")) {
            jce_audio_mixer_set_send(s_mixer, bus, dest, amt);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) dirty = true;
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n("audioMixer.fx.remove"))) {
            jce_audio_mixer_remove_send(s_mixer, bus, dest);
            dirty = true;
        }
        ImGui::PopID();
    }

    /* Add-send row: pick a target bus (excluding self / existing). */
    static int s_send_target_idx = 0;
    std::vector<JceAudioBusId> targets;
    std::vector<const char *>  target_names;
    for (uint32_t i = 0; i < n; ++i) {
        if (all[i] == bus) continue;
        if (bus_has_send(bus, all[i])) continue;
        targets.push_back(all[i]);
        const char *tn = jce_audio_mixer_get_name(s_mixer, all[i]);
        target_names.push_back(tn ? tn : "?");
    }
    if (!targets.empty()) {
        if (s_send_target_idx >= (int)targets.size()) s_send_target_idx = 0;
        ImGui::SetNextItemWidth(160);
        ImGui::Combo("##send_target", &s_send_target_idx,
                     target_names.data(), (int)target_names.size());
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("audioMixer.sends.add"))) {
            if (jce_audio_mixer_set_send(s_mixer, bus,
                    targets[(size_t)s_send_target_idx], 0.5f))
                dirty = true;
        }
    }

    if (dirty) mixer_save();
}

/* Sidechain (ducking) config on the selected (target) bus. */
static void draw_sidechain_section(JceAudioBusId bus,
                                   const JceAudioBusId *all, uint32_t n)
{
    ImGui::SeparatorText(jce_editor_i18n("audioMixer.sidechain"));
    bool dirty = false;
    bool has = jce_audio_mixer_has_sidechain(s_mixer, bus);

    bool enabled = has;
    if (ImGui::Checkbox(jce_editor_i18n("audioMixer.sc.enable"), &enabled)) {
        if (enabled && !has) {
            JceAudioDuckParams dp = jce_audio_duck_default_params();
            /* Default the key to the first other bus. */
            for (uint32_t i = 0; i < n; ++i)
                if (all[i] != bus) { dp.key = all[i]; break; }
            if (dp.key != JCE_AUDIO_BUS_INVALID)
                jce_audio_mixer_set_sidechain(s_mixer, bus, &dp, 48000u);
        } else if (!enabled && has) {
            jce_audio_mixer_clear_sidechain(s_mixer, bus);
        }
        dirty = true;
        has = jce_audio_mixer_has_sidechain(s_mixer, bus);
    }

    JceAudioDuckParams dp;
    if (has && jce_audio_mixer_get_sidechain(s_mixer, bus, &dp)) {
        /* Key bus combo. */
        std::vector<JceAudioBusId> keys;
        std::vector<const char *>  key_names;
        int cur = 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (all[i] == bus) continue;
            if (all[i] == dp.key) cur = (int)keys.size();
            keys.push_back(all[i]);
            const char *kn = jce_audio_mixer_get_name(s_mixer, all[i]);
            key_names.push_back(kn ? kn : "?");
        }
        bool ch = false;
        if (!keys.empty()) {
            ImGui::SetNextItemWidth(160);
            if (ImGui::Combo(jce_editor_i18n("audioMixer.sc.key"), &cur,
                             key_names.data(), (int)key_names.size())) {
                dp.key = keys[(size_t)cur]; ch = true;
            }
        }
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.sc.threshold"),
                             &dp.threshold_db, 0.1f, -60.0f, 0.0f, "%.1f dB")) ch = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.sc.ratio"),
                             &dp.ratio, 0.1f, 1.0f, 20.0f, "%.1f:1")) ch = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.sc.attack"),
                             &dp.attack_ms, 0.1f, 0.0f, 200.0f, "%.1f ms")) ch = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.sc.release"),
                             &dp.release_ms, 1.0f, 0.0f, 2000.0f, "%.0f ms")) ch = true;
        if (ImGui::DragFloat(jce_editor_i18n("audioMixer.sc.floor"),
                             &dp.max_attenuation_db, 0.1f, -60.0f, 0.0f, "%.1f dB")) ch = true;
        if (ch) {
            /* Re-install with the edited params every change frame (cheap,
             * in-memory) but defer the disk write to the drag-release latch. */
            jce_audio_mixer_set_sidechain(s_mixer, bus, &dp, 48000u);
            s_param_save_pending = true;
        }
    }

    if (dirty) mixer_save();
}

static void draw_bus_inspector(const JceAudioBusId *all, uint32_t n)
{
    JceAudioBusId bus = s_selected_bus;
    if (bus == JCE_AUDIO_BUS_INVALID ||
        !jce_audio_mixer_get_name(s_mixer, bus)) {
        ImGui::TextDisabled("%s", jce_editor_i18n("audioMixer.selectBus"));
        return;
    }
    const char *bn = jce_audio_mixer_get_name(s_mixer, bus);
    ImGui::Text("%s %s", jce_editor_i18n("audioMixer.busLabel"), bn ? bn : "?");
    ImGui::PushID((int)bus);
    draw_inserts_section(bus);
    draw_sends_section(bus, all, n);
    draw_sidechain_section(bus, all, n);
    ImGui::PopID();
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
        JceAudioBusId removed = s_selected_bus;
        if (jce_audio_mixer_remove_bus(s_mixer, s_selected_bus)) {
            s_bus_effects.erase(removed);
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

    /* ── Selected-bus inspector: inserts + sends + sidechain ───────── */
    ImGui::Separator();
    draw_bus_inspector(all, n);

    ImGui::Separator();
    ImGui::TextDisabled("%s",
        jce_editor_i18n("audioMixer.runtimeNote"));
}

/* ── Snapshots tab: capture current bus volumes, apply with a fade ──── */
static void draw_snapshots_tab(void)
{
    ImGui::TextDisabled("%s", jce_editor_i18n("audioMixer.snap.help"));
    ImGui::Separator();

    ImGui::SetNextItemWidth(180);
    ImGui::InputTextWithHint("##snap_name",
        jce_editor_i18n("audioMixer.snap.namehint"),
        s_snapshot_buf, sizeof(s_snapshot_buf));
    ImGui::SameLine();
    bool can_capture = s_snapshot_buf[0] != 0;
    ImGui::BeginDisabled(!can_capture);
    if (ImGui::Button(jce_editor_i18n("audioMixer.snap.capture"))) {
        if (jce_audio_mixer_capture_snapshot(s_mixer, s_snapshot_buf)) {
            mixer_save();
            s_snapshot_buf[0] = 0;
        }
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::SetNextItemWidth(160);
    ImGui::DragFloat(jce_editor_i18n("audioMixer.snap.fade"),
                     &s_snapshot_fade, 0.05f, 0.0f, 10.0f, "%.2f s");

    uint32_t cnt = jce_audio_mixer_snapshot_count(s_mixer);
    if (cnt == 0) {
        ImGui::TextDisabled("%s", jce_editor_i18n("audioMixer.snap.empty"));
        return;
    }
    if (ImGui::BeginTable("##snap_tbl", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn(jce_editor_i18n("audioMixer.snap.col.name"),
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(jce_editor_i18n("audioMixer.snap.col.apply"),
                                ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableHeadersRow();

        for (uint32_t i = 0; i < cnt; ++i) {
            char sname[JCE_AUDIO_SNAPSHOT_NAME] = {0};
            if (!jce_audio_mixer_snapshot_name(s_mixer, i, sname, sizeof(sname)))
                continue;
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(sname);
            ImGui::TableSetColumnIndex(1);
            if (ImGui::SmallButton(jce_editor_i18n("audioMixer.snap.apply")))
                jce_audio_mixer_apply_snapshot(s_mixer, sname, s_snapshot_fade);
            ImGui::TableSetColumnIndex(2);
            if (ImGui::SmallButton(jce_editor_i18n("audioMixer.snap.remove"))) {
                jce_audio_mixer_remove_snapshot(s_mixer, sname);
                mixer_save();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (jce_audio_mixer_snapshot_fading(s_mixer)) {
        ImGui::ProgressBar(jce_audio_mixer_snapshot_progress(s_mixer));
    }
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
        ImGui::TableSetupColumn(jce_editor_i18n("audioMixer.col.owner"),   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(jce_editor_i18n("audioMixer.col.preset"),  ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn(jce_editor_i18n("audioMixer.col.min"),     ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn(jce_editor_i18n("audioMixer.col.max"),     ImGuiTableColumnFlags_WidthFixed, 90);
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

    /* Drive any in-flight snapshot crossfade so applied snapshots actually
     * ramp the live bus volumes while this panel is open. */
    jce_audio_mixer_update(s_mixer, ImGui::GetIO().DeltaTime);

    jce_panel_tab_ensure_loaded(s_tabs);
    if (ImGui::BeginTabBar("##audio_mixer_tabs")) {
        ImGuiTabItemFlags mixer_flags  = jce_panel_tab_flags(s_tabs, 0);
        ImGuiTabItemFlags snap_flags   = jce_panel_tab_flags(s_tabs, 1);
        ImGuiTabItemFlags reverb_flags = jce_panel_tab_flags(s_tabs, 2);
        s_tabs.request = -1;
        if (ImGui::BeginTabItem(jce_editor_i18n("audioMixer.tab.mixer"),
                                nullptr, mixer_flags)) {
            jce_panel_tab_set_current(s_tabs, 0);
            draw_mixer_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(jce_editor_i18n("audioMixer.tab.snapshots"),
                                nullptr, snap_flags)) {
            jce_panel_tab_set_current(s_tabs, 1);
            draw_snapshots_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(jce_editor_i18n("audioMixer.tab.reverb"),
                                nullptr, reverb_flags)) {
            jce_panel_tab_set_current(s_tabs, 2);
            draw_reverb_zones_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    /* Flush any parameter-drag save once the drag has released. */
    mixer_flush_param_saves();
}

extern "C" void jce_editor_audio_mixer_focus_reverb_tab(void)
{
    jce_panel_tab_request(s_tabs, 2);
}

extern "C" void jce_editor_audio_mixer_focus_mixer_tab(void)
{
    jce_panel_tab_request(s_tabs, 0);
}

extern "C" int jce_editor_audio_mixer_current_tab(void)
{
    return jce_panel_tab_current(s_tabs);
}

extern "C" void jce_editor_panel_audio_mixer(void)
{
    /* Reserved for future toolbar/integration logic. */
}
