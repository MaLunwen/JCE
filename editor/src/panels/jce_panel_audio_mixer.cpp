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
#include <map>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <jce/middleware/audio/jce_audio_mixer.h>
#include <jce/middleware/audio/jce_audio_dsp.h>
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
/* Tabs: 0=mixer, 1=snapshots, 2=reverb. */
static int            s_pending_focus_tab = -1;
static int            s_current_tab       = 0;
static bool           s_tab_state_loaded  = false;
static char           s_snapshot_buf[JCE_AUDIO_SNAPSHOT_NAME] = {0};
static float          s_snapshot_fade     = 1.0f;

/* Insert-effect chains live on the live JceAudio device (attached by bus
 * NAME via jce_audio_bus_add_effect), not on the JceAudioMixer bookkeeping
 * struct.  So the editor keeps the authored chain here, keyed by bus id, and
 * serializes it into the same audio_mixer.json so the runtime can re-attach
 * each bus's chain by name at play start. */
static std::map<JceAudioBusId, std::vector<JceAudioEffectDesc>> s_bus_effects;

static const char *k_audio_tab_state_key = "panel.audio_mixer.current_tab";

static void ensure_tab_state_loaded(void)
{
    if (s_tab_state_loaded)
        return;
    s_current_tab =
        jce_editor_ui_state_load_int(k_audio_tab_state_key, 0, 0, 2);
    s_pending_focus_tab = s_current_tab;
    s_tab_state_loaded = true;
}

static void set_current_tab(int idx)
{
    if (idx < 0 || idx > 2 || s_current_tab == idx)
        return;
    s_current_tab = idx;
    if (s_tab_state_loaded)
        jce_editor_ui_state_save_int(k_audio_tab_state_key, idx);
}

/* ── Persistence ────────────────────────────────────────────────────── */

/* Append one effect-desc object to a std::string (compact JSON). */
static void effect_to_json(std::string &s, const JceAudioEffectDesc &d)
{
    char tmp[256];
    switch (d.type) {
    case JCE_AUDIO_EFFECT_EQ:
        std::snprintf(tmp, sizeof(tmp),
            "{ \"type\": \"eq\", \"shape\": %d, \"freq\": %.4f, "
            "\"gain_db\": %.4f, \"q\": %.4f }",
            (int)d.u.eq.shape, (double)d.u.eq.frequency_hz,
            (double)d.u.eq.gain_db, (double)d.u.eq.q);
        break;
    case JCE_AUDIO_EFFECT_COMPRESSOR:
        std::snprintf(tmp, sizeof(tmp),
            "{ \"type\": \"comp\", \"threshold_db\": %.4f, \"ratio\": %.4f, "
            "\"attack_ms\": %.4f, \"release_ms\": %.4f, \"makeup_db\": %.4f, "
            "\"knee_db\": %.4f }",
            (double)d.u.comp.threshold_db, (double)d.u.comp.ratio,
            (double)d.u.comp.attack_ms, (double)d.u.comp.release_ms,
            (double)d.u.comp.makeup_db, (double)d.u.comp.knee_db);
        break;
    case JCE_AUDIO_EFFECT_LIMITER:
        std::snprintf(tmp, sizeof(tmp),
            "{ \"type\": \"limiter\", \"ceiling_db\": %.4f, "
            "\"release_ms\": %.4f }",
            (double)d.u.limiter.ceiling_db, (double)d.u.limiter.release_ms);
        break;
    case JCE_AUDIO_EFFECT_DELAY:
        std::snprintf(tmp, sizeof(tmp),
            "{ \"type\": \"delay\", \"delay_ms\": %.4f, \"feedback\": %.4f, "
            "\"wet\": %.4f, \"dry\": %.4f }",
            (double)d.u.delay.delay_ms, (double)d.u.delay.feedback,
            (double)d.u.delay.wet, (double)d.u.delay.dry);
        break;
    default:
        std::snprintf(tmp, sizeof(tmp), "{ \"type\": \"none\" }");
        break;
    }
    s += tmp;
}

static void mixer_save(void)
{
    if (!s_mixer) return;
    JceAudioBusId ids[256] = {0};
    uint32_t      n        = jce_audio_mixer_list_buses(s_mixer, ids, 256);

    /* Built with std::string so the extended schema (sends / sidechain /
     * per-bus effects / snapshots) has no fixed-size buffer ceiling. */
    std::string out;
    out.reserve(1024 + n * 256);
    out += "{\n  \"buses\": [\n";

    char row[512];
    for (uint32_t i = 0; i < n; ++i) {
        JceAudioBusId id     = ids[i];
        JceAudioBusId parent = jce_audio_mixer_get_parent(s_mixer, id);
        const char   *name   = jce_audio_mixer_get_name(s_mixer, id);
        if (!name) name = "";
        std::snprintf(row, sizeof(row),
            "    { \"id\": %u, \"parent\": %u, \"name\": \"%s\","
            " \"volume\": %.4f, \"muted\": %d, \"solo\": %d",
            (unsigned)id, (unsigned)parent, name,
            (double)jce_audio_mixer_get_volume(s_mixer, id),
            jce_audio_mixer_is_muted(s_mixer, id) ? 1 : 0,
            jce_audio_mixer_is_solo(s_mixer, id)  ? 1 : 0);
        out += row;

        /* Aux sends from this bus. */
        std::string sends;
        for (uint32_t j = 0; j < n; ++j) {
            if (ids[j] == id) continue;
            float amt = jce_audio_mixer_get_send(s_mixer, id, ids[j]);
            if (amt <= 0.0f) continue;
            std::snprintf(row, sizeof(row),
                "%s{ \"dest\": %u, \"amount\": %.4f }",
                sends.empty() ? "" : ", ",
                (unsigned)ids[j], (double)amt);
            sends += row;
        }
        if (!sends.empty()) {
            out += ", \"sends\": [ ";
            out += sends;
            out += " ]";
        }

        /* Sidechain installed on this (target) bus. */
        JceAudioDuckParams dp;
        if (jce_audio_mixer_get_sidechain(s_mixer, id, &dp)) {
            std::snprintf(row, sizeof(row),
                ", \"sidechain\": { \"key\": %u, \"threshold_db\": %.4f, "
                "\"ratio\": %.4f, \"attack_ms\": %.4f, \"release_ms\": %.4f, "
                "\"floor_db\": %.4f }",
                (unsigned)dp.key, (double)dp.threshold_db, (double)dp.ratio,
                (double)dp.attack_ms, (double)dp.release_ms,
                (double)dp.max_attenuation_db);
            out += row;
        }

        /* Insert-effect chain (editor-side model). */
        auto it = s_bus_effects.find(id);
        if (it != s_bus_effects.end() && !it->second.empty()) {
            out += ", \"effects\": [ ";
            for (size_t k = 0; k < it->second.size(); ++k) {
                if (k) out += ", ";
                effect_to_json(out, it->second[k]);
            }
            out += " ]";
        }

        out += (i + 1 < n) ? " },\n" : " }\n";
    }
    out += "  ]";

    /* Top-level named snapshots. */
    uint32_t snap_n = jce_audio_mixer_snapshot_count(s_mixer);
    if (snap_n > 0) {
        out += ",\n  \"snapshots\": [\n";
        for (uint32_t si = 0; si < snap_n; ++si) {
            char sname[JCE_AUDIO_SNAPSHOT_NAME] = {0};
            if (!jce_audio_mixer_snapshot_name(s_mixer, si, sname, sizeof(sname)))
                continue;
            out += "    { \"name\": \"";
            out += sname;
            out += "\", \"volumes\": [ ";
            std::string vols;
            for (uint32_t i = 0; i < n; ++i) {
                float v = jce_audio_mixer_snapshot_get_volume(s_mixer, sname, ids[i]);
                if (v < 0.0f) continue;
                std::snprintf(row, sizeof(row), "%s{ \"id\": %u, \"volume\": %.4f }",
                              vols.empty() ? "" : ", ", (unsigned)ids[i], (double)v);
                vols += row;
            }
            out += vols;
            out += " ] }";
            out += (si + 1 < snap_n) ? ",\n" : "\n";
        }
        out += "  ]";
    }
    out += "\n}\n";

    ed_write_file(MIXER_PATH, out.data(), out.size());
}

static void mixer_seed_default(void)
{
    /* Master is auto-created with id=1; just add common children. */
    jce_audio_mixer_add_bus(s_mixer, JCE_AUDIO_BUS_MASTER, "Music", 0.8f);
    jce_audio_mixer_add_bus(s_mixer, JCE_AUDIO_BUS_MASTER, "SFX",   1.0f);
    jce_audio_mixer_add_bus(s_mixer, JCE_AUDIO_BUS_MASTER, "Voice", 1.0f);
    jce_audio_mixer_add_bus(s_mixer, JCE_AUDIO_BUS_MASTER, "UI",    1.0f);
}

/* Scan a key:number within a bounded region [beg,end). Tolerates both
 * "key" : v and "key":v spacing.  Returns true if found. */
static bool scan_uint(const char *beg, const char *end, const char *key,
                      unsigned *out)
{
    const char *k = std::strstr(beg, key);
    if (!k || k >= end) return false;
    const char *c = std::strchr(k, ':');
    if (!c || c >= end) return false;
    return std::sscanf(c + 1, " %u", out) == 1;
}
static bool scan_float(const char *beg, const char *end, const char *key,
                       float *out)
{
    const char *k = std::strstr(beg, key);
    if (!k || k >= end) return false;
    const char *c = std::strchr(k, ':');
    if (!c || c >= end) return false;
    return std::sscanf(c + 1, " %f", out) == 1;
}
static bool scan_str(const char *beg, const char *end, const char *key,
                     char *out, size_t cap)
{
    const char *k = std::strstr(beg, key);
    if (!k || k >= end) return false;
    const char *c = std::strchr(k, ':');
    if (!c || c >= end) return false;
    const char *q1 = std::strchr(c, '"');
    const char *q2 = q1 ? std::strchr(q1 + 1, '"') : nullptr;
    if (!q1 || !q2 || q1 >= end || q2 >= end) return false;
    size_t nl = (size_t)(q2 - q1 - 1);
    if (nl >= cap) nl = cap - 1;
    std::memcpy(out, q1 + 1, nl);
    out[nl] = 0;
    return true;
}

/* Find the matching closing brace for the '{' at `open` (handles nesting and
 * skips braces inside strings).  Returns pointer to the '}' or end. */
static const char *match_brace(const char *open, const char *end)
{
    int depth = 0;
    bool in_str = false;
    for (const char *p = open; p < end; ++p) {
        if (in_str) { if (*p == '"') in_str = false; continue; }
        if (*p == '"') in_str = true;
        else if (*p == '{') ++depth;
        else if (*p == '}') { if (--depth == 0) return p; }
    }
    return end;
}

/* Find the matching closing ']' for the '[' at `open` (handles nesting,
 * skips brackets inside strings).  Returns pointer to ']' or NULL. */
static const char *match_brace_arr_end(const char *open, const char *end)
{
    int depth = 0;
    bool in_str = false;
    for (const char *p = open; p < end; ++p) {
        if (in_str) { if (*p == '"') in_str = false; continue; }
        if (*p == '"') in_str = true;
        else if (*p == '[') ++depth;
        else if (*p == ']') { if (--depth == 0) return p; }
    }
    return nullptr;
}

/* Parse one effects array (between '[' and its ']') into `dst`. */
static void parse_effects(const char *beg, const char *end,
                          std::vector<JceAudioEffectDesc> &dst)
{
    const char *p = beg;
    while (p < end) {
        const char *o = std::strchr(p, '{');
        if (!o || o >= end) break;
        const char *c = match_brace(o, end);
        char type[16] = {0};
        if (scan_str(o, c, "\"type\"", type, sizeof(type))) {
            JceAudioEffectDesc d{};
            if (std::strcmp(type, "eq") == 0) {
                d = jce_audio_effect_default(JCE_AUDIO_EFFECT_EQ);
                unsigned shape = (unsigned)d.u.eq.shape;
                scan_uint (o, c, "\"shape\"",   &shape); d.u.eq.shape = (JceAudioEqShape)shape;
                scan_float(o, c, "\"freq\"",    &d.u.eq.frequency_hz);
                scan_float(o, c, "\"gain_db\"", &d.u.eq.gain_db);
                scan_float(o, c, "\"q\"",       &d.u.eq.q);
            } else if (std::strcmp(type, "comp") == 0) {
                d = jce_audio_effect_default(JCE_AUDIO_EFFECT_COMPRESSOR);
                scan_float(o, c, "\"threshold_db\"", &d.u.comp.threshold_db);
                scan_float(o, c, "\"ratio\"",        &d.u.comp.ratio);
                scan_float(o, c, "\"attack_ms\"",    &d.u.comp.attack_ms);
                scan_float(o, c, "\"release_ms\"",   &d.u.comp.release_ms);
                scan_float(o, c, "\"makeup_db\"",    &d.u.comp.makeup_db);
                scan_float(o, c, "\"knee_db\"",      &d.u.comp.knee_db);
            } else if (std::strcmp(type, "limiter") == 0) {
                d = jce_audio_effect_default(JCE_AUDIO_EFFECT_LIMITER);
                scan_float(o, c, "\"ceiling_db\"",  &d.u.limiter.ceiling_db);
                scan_float(o, c, "\"release_ms\"",  &d.u.limiter.release_ms);
            } else if (std::strcmp(type, "delay") == 0) {
                d = jce_audio_effect_default(JCE_AUDIO_EFFECT_DELAY);
                scan_float(o, c, "\"delay_ms\"",  &d.u.delay.delay_ms);
                scan_float(o, c, "\"feedback\"",  &d.u.delay.feedback);
                scan_float(o, c, "\"wet\"",       &d.u.delay.wet);
                scan_float(o, c, "\"dry\"",       &d.u.delay.dry);
            }
            if (d.type != JCE_AUDIO_EFFECT_NONE) dst.push_back(d);
        }
        p = c + 1;
    }
}

static bool mixer_load(void)
{
    size_t len = 0;
    char  *raw = (char *)ed_read_file(MIXER_PATH, &len);
    if (!raw) return false;
    if (len > (1 << 20)) { ED_FREE(raw); return false; }
    const char *file_end = raw + len;
    s_bus_effects.clear();

    /* Locate the buses array; bound bus parsing to it so snapshot/volume
     * "id" keys are never mistaken for bus rows. */
    const char *buses_key = std::strstr(raw, "\"buses\"");
    const char *buses_beg = buses_key ? std::strchr(buses_key, '[') : nullptr;
    if (!buses_beg) { ED_FREE(raw); return true; }

    /* Map a parsed JSON id -> the live bus id actually assigned. Buses are
     * created in file order, so deferred send/sidechain refs (which name
     * JSON ids) resolve through this table in a second pass. */
    std::map<unsigned, JceAudioBusId> id_map;
    id_map[JCE_AUDIO_BUS_MASTER] = JCE_AUDIO_BUS_MASTER;

    /* Record per-bus deferred refs for pass 2 (object byte range). */
    struct BusObj { JceAudioBusId live; const char *beg; const char *end; };
    std::vector<BusObj> objs;

    /* Pass 1: create every bus, capture its object span.  Bound it to the
     * matching ']' of the buses '[' so the snapshots array (which follows)
     * is never scanned for bus rows. */
    int bdepth = 0; bool bstr = false; const char *buses_end = file_end;
    for (const char *q = buses_beg; q < file_end; ++q) {
        if (bstr) { if (*q == '"') bstr = false; continue; }
        if (*q == '"') bstr = true;
        else if (*q == '[') ++bdepth;
        else if (*q == ']') { if (--bdepth == 0) { buses_end = q; break; } }
    }
    const char *p = buses_beg + 1;
    while (p < buses_end) {
        const char *o = std::strchr(p, '{');
        if (!o || o >= buses_end) break;
        const char *c = match_brace(o, buses_end);
        unsigned id = 0, parent = 0, mutedv = 0, solov = 0;
        float    vol = 1.0f;
        char     name[64] = {0};
        scan_uint (o, c, "\"id\"",     &id);
        scan_uint (o, c, "\"parent\"", &parent);
        scan_str  (o, c, "\"name\"",   name, sizeof(name));
        scan_float(o, c, "\"volume\"", &vol);
        scan_uint (o, c, "\"muted\"",  &mutedv);
        scan_uint (o, c, "\"solo\"",   &solov);

        JceAudioBusId live = JCE_AUDIO_BUS_INVALID;
        if (id == JCE_AUDIO_BUS_MASTER) {
            jce_audio_mixer_set_volume(s_mixer, JCE_AUDIO_BUS_MASTER, vol);
            jce_audio_mixer_set_muted (s_mixer, JCE_AUDIO_BUS_MASTER, mutedv != 0);
            jce_audio_mixer_set_solo  (s_mixer, JCE_AUDIO_BUS_MASTER, solov  != 0);
            live = JCE_AUDIO_BUS_MASTER;
        } else if (id != 0 && name[0]) {
            unsigned par = parent ? parent : JCE_AUDIO_BUS_MASTER;
            auto pit = id_map.find(par);
            JceAudioBusId par_id = (pit != id_map.end())
                ? pit->second : JCE_AUDIO_BUS_MASTER;
            live = jce_audio_mixer_add_bus(s_mixer, par_id, name, vol);
            if (live != JCE_AUDIO_BUS_INVALID) {
                jce_audio_mixer_set_muted(s_mixer, live, mutedv != 0);
                jce_audio_mixer_set_solo (s_mixer, live, solov  != 0);
            }
        }
        if (live != JCE_AUDIO_BUS_INVALID) {
            id_map[id] = live;
            objs.push_back({ live, o, c });

            /* Insert-effect chain (resolved immediately; no id refs). */
            const char *fx = std::strstr(o, "\"effects\"");
            if (fx && fx < c) {
                const char *fb = std::strchr(fx, '[');
                const char *fe = (fb && fb < c) ? match_brace_arr_end(fb, c)
                                                : nullptr;
                if (fb && fe)
                    parse_effects(fb + 1, fe, s_bus_effects[live]);
            }
        }
        p = c + 1;
    }

    /* Pass 2: sends + sidechain (need the full id_map resolved). */
    for (const BusObj &b : objs) {
        const char *o = b.beg, *c = b.end;
        /* Sends: iterate { dest, amount } objects inside "sends":[...]. */
        const char *sk = std::strstr(o, "\"sends\"");
        if (sk && sk < c) {
            const char *sb = std::strchr(sk, '[');
            const char *se = sb ? match_brace_arr_end(sb, c) : nullptr;
            if (sb && se) {
                const char *q = sb + 1;
                while (q < se) {
                    const char *so = std::strchr(q, '{');
                    if (!so || so >= se) break;
                    const char *sc = match_brace(so, se);
                    unsigned dest = 0; float amt = 0.0f;
                    scan_uint (so, sc, "\"dest\"",   &dest);
                    scan_float(so, sc, "\"amount\"", &amt);
                    auto dit = id_map.find(dest);
                    if (dit != id_map.end() && amt > 0.0f)
                        jce_audio_mixer_set_send(s_mixer, b.live, dit->second, amt);
                    q = sc + 1;
                }
            }
        }
        /* Sidechain object. */
        const char *xk = std::strstr(o, "\"sidechain\"");
        if (xk && xk < c) {
            const char *xo = std::strchr(xk, '{');
            const char *xc = xo ? match_brace(xo, c) : nullptr;
            if (xo && xc) {
                unsigned key = 0;
                JceAudioDuckParams dp = jce_audio_duck_default_params();
                scan_uint (xo, xc, "\"key\"",          &key);
                scan_float(xo, xc, "\"threshold_db\"", &dp.threshold_db);
                scan_float(xo, xc, "\"ratio\"",        &dp.ratio);
                scan_float(xo, xc, "\"attack_ms\"",    &dp.attack_ms);
                scan_float(xo, xc, "\"release_ms\"",   &dp.release_ms);
                scan_float(xo, xc, "\"floor_db\"",     &dp.max_attenuation_db);
                auto kit = id_map.find(key);
                if (kit != id_map.end()) {
                    dp.key = kit->second;
                    jce_audio_mixer_set_sidechain(s_mixer, b.live, &dp, 48000u);
                }
            }
        }
    }

    /* Snapshots array (top-level). */
    const char *snap_key = std::strstr(buses_end, "\"snapshots\"");
    const char *snap_beg = snap_key ? std::strchr(snap_key, '[') : nullptr;
    if (snap_beg) {
        const char *snap_end = match_brace_arr_end(snap_beg, file_end);
        const char *q = snap_beg ? snap_beg + 1 : nullptr;
        while (q && snap_end && q < snap_end) {
            const char *so = std::strchr(q, '{');
            if (!so || so >= snap_end) break;
            const char *sc = match_brace(so, snap_end);
            char sname[JCE_AUDIO_SNAPSHOT_NAME] = {0};
            if (scan_str(so, sc, "\"name\"", sname, sizeof(sname)) && sname[0]) {
                /* volumes:[ { id, volume } ] */
                const char *vb = std::strstr(so, "\"volumes\"");
                vb = vb ? std::strchr(vb, '[') : nullptr;
                if (vb && vb < sc) {
                    const char *ve = match_brace_arr_end(vb, sc);
                    const char *r = vb + 1;
                    while (ve && r < ve) {
                        const char *vo = std::strchr(r, '{');
                        if (!vo || vo >= ve) break;
                        const char *vc = match_brace(vo, ve);
                        unsigned vid = 0; float vv = 0.0f;
                        scan_uint (vo, vc, "\"id\"",     &vid);
                        scan_float(vo, vc, "\"volume\"", &vv);
                        auto vit = id_map.find(vid);
                        if (vit != id_map.end())
                            jce_audio_mixer_snapshot_set_volume(
                                s_mixer, sname, vit->second, vv);
                        r = vc + 1;
                    }
                }
            }
            q = sc + 1;
        }
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

/* ── Bus inspector: insert-effect chain + aux sends + sidechain ─────── */

static const char *kEffectTypeNames[] = { "EQ", "Compressor", "Limiter", "Delay" };
static const JceAudioEffectType kEffectTypeVals[] = {
    JCE_AUDIO_EFFECT_EQ, JCE_AUDIO_EFFECT_COMPRESSOR,
    JCE_AUDIO_EFFECT_LIMITER, JCE_AUDIO_EFFECT_DELAY
};
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
    default: break;
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
            draw_effect_params(chain[i], dirty);
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

/* Aux sends originating from the selected bus. */
static void draw_sends_section(JceAudioBusId bus,
                               const JceAudioBusId *all, uint32_t n)
{
    ImGui::SeparatorText(jce_editor_i18n("audioMixer.sends"));
    bool dirty = false;

    for (uint32_t i = 0; i < n; ++i) {
        JceAudioBusId dest = all[i];
        if (dest == bus) continue;
        float amt = jce_audio_mixer_get_send(s_mixer, bus, dest);
        if (amt <= 0.0f) continue;
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
        if (jce_audio_mixer_get_send(s_mixer, bus, all[i]) > 0.0f) continue;
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
            /* Re-install with the edited params.  Live edits persist on every
             * change frame; cheap because the sidechain table is per-bus. */
            jce_audio_mixer_set_sidechain(s_mixer, bus, &dp, 48000u);
            dirty = true;
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

    ensure_tab_state_loaded();
    if (ImGui::BeginTabBar("##audio_mixer_tabs")) {
        ImGuiTabItemFlags mixer_flags = (s_pending_focus_tab == 0)
            ? ImGuiTabItemFlags_SetSelected : 0;
        ImGuiTabItemFlags snap_flags = (s_pending_focus_tab == 1)
            ? ImGuiTabItemFlags_SetSelected : 0;
        ImGuiTabItemFlags reverb_flags = (s_pending_focus_tab == 2)
            ? ImGuiTabItemFlags_SetSelected : 0;
        s_pending_focus_tab = -1;
        if (ImGui::BeginTabItem(jce_editor_i18n("audioMixer.tab.mixer"),
                                nullptr, mixer_flags)) {
            set_current_tab(0);
            draw_mixer_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(jce_editor_i18n("audioMixer.tab.snapshots"),
                                nullptr, snap_flags)) {
            set_current_tab(1);
            draw_snapshots_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(jce_editor_i18n("audioMixer.tab.reverb"),
                                nullptr, reverb_flags)) {
            set_current_tab(2);
            draw_reverb_zones_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

extern "C" void jce_editor_audio_mixer_focus_reverb_tab(void)
{
    s_pending_focus_tab = 2;
    s_current_tab = 2;
    jce_editor_ui_state_save_int(k_audio_tab_state_key, 2);
}

extern "C" void jce_editor_audio_mixer_focus_mixer_tab(void)
{
    s_pending_focus_tab = 0;
    s_current_tab = 0;
    jce_editor_ui_state_save_int(k_audio_tab_state_key, 0);
}

extern "C" int jce_editor_audio_mixer_current_tab(void)
{
    ensure_tab_state_loaded();
    return s_current_tab;
}

extern "C" void jce_editor_panel_audio_mixer(void)
{
    /* Reserved for future toolbar/integration logic. */
}
