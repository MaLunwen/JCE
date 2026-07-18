/*
 * jce_hotkeys.cpp  Implementation of the hotkey registry.
 */

#include "jce_hotkeys.h"
#include "jce_editor_config.h"
#include "jce_editor_alloc.h"
#include "io/jce_editor_file_util.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace {

struct HotkeyEntry {
    const char    *id_string;   /* stable, used in JSON */
    const char    *display;     /* shown in Settings UI */
    JceHotkeyChord def;
    JceHotkeyChord cur;
};

/* clang-format off */
HotkeyEntry s_table[JCE_HK_COUNT] = {
    /* file */
    { "file.save",                 "File / Save",                 { ImGuiKey_S, JCE_HKM_CTRL }, {} },
    { "file.save_as",              "File / Save As",              { ImGuiKey_S, (uint8_t)(JCE_HKM_CTRL | JCE_HKM_SHIFT) }, {} },
    { "file.open",                 "File / Open",                 { ImGuiKey_O, JCE_HKM_CTRL }, {} },
    { "file.new",                  "File / New",                  { ImGuiKey_N, JCE_HKM_CTRL }, {} },

    /* edit */
    { "edit.undo",                 "Edit / Undo",                 { ImGuiKey_Z, JCE_HKM_CTRL }, {} },
    { "edit.redo",                 "Edit / Redo",                 { ImGuiKey_Y, JCE_HKM_CTRL }, {} },
    { "edit.redo_alt",             "Edit / Redo (Alt)",           { ImGuiKey_Z, (uint8_t)(JCE_HKM_CTRL | JCE_HKM_SHIFT) }, {} },
    { "edit.cut",                  "Edit / Cut",                  { ImGuiKey_X, JCE_HKM_CTRL }, {} },
    { "edit.copy",                 "Edit / Copy",                 { ImGuiKey_C, JCE_HKM_CTRL }, {} },
    { "edit.paste",                "Edit / Paste",                { ImGuiKey_V, JCE_HKM_CTRL }, {} },
    { "edit.duplicate",            "Edit / Duplicate",            { ImGuiKey_D, JCE_HKM_CTRL }, {} },
    { "edit.delete",               "Edit / Delete",               { ImGuiKey_Delete, JCE_HKM_NONE }, {} },
    { "edit.delete_alt",           "Edit / Delete (Alt)",         { ImGuiKey_Backspace, JCE_HKM_NONE }, {} },
    { "edit.rename",               "Edit / Rename",               { ImGuiKey_F2, JCE_HKM_NONE }, {} },
    { "edit.select_all",           "Edit / Select All",           { ImGuiKey_A, JCE_HKM_CTRL }, {} },
    { "edit.find",                 "Edit / Find",                 { ImGuiKey_F, JCE_HKM_CTRL }, {} },

    /* gizmos */
    { "gizmo.translate",           "Gizmo / Translate",           { ImGuiKey_W, JCE_HKM_NONE }, {} },
    { "gizmo.rotate",              "Gizmo / Rotate",              { ImGuiKey_E, JCE_HKM_NONE }, {} },
    { "gizmo.scale",               "Gizmo / Scale",               { ImGuiKey_R, JCE_HKM_NONE }, {} },
    { "gizmo.none",                "Gizmo / None",                { ImGuiKey_Q, JCE_HKM_NONE }, {} },
    { "gizmo.toggle_space",        "Gizmo / Toggle Local/World",  { ImGuiKey_X, JCE_HKM_NONE }, {} },
    { "gizmo.toggle_snap",         "Gizmo / Toggle Snap",         { ImGuiKey_S, JCE_HKM_NONE }, {} },
    { "gizmo.toggle_pivot",        "Gizmo / Toggle Pivot/Center", { ImGuiKey_Z, JCE_HKM_NONE }, {} },
    { "gizmo.pivot_edit",          "Gizmo / Edit Pivot",          { ImGuiKey_D, JCE_HKM_NONE }, {} },

    /* view */
    { "view.frame_selected",       "View / Frame Selected",       { ImGuiKey_F, JCE_HKM_NONE }, {} },
    { "view.frame_all",            "View / Frame All",            { ImGuiKey_F, JCE_HKM_SHIFT }, {} },
    { "view.focus_persp",          "View / Perspective",          { ImGuiKey_Keypad5, JCE_HKM_NONE }, {} },
    { "view.focus_top",            "View / Top",                  { ImGuiKey_Keypad7, JCE_HKM_NONE }, {} },
    { "view.focus_front",          "View / Front",                { ImGuiKey_Keypad1, JCE_HKM_NONE }, {} },
    { "view.focus_right",          "View / Right",                { ImGuiKey_Keypad3, JCE_HKM_NONE }, {} },
    { "view.toggle_grid",          "View / Toggle Grid",          { ImGuiKey_G, JCE_HKM_NONE }, {} },
    { "view.toggle_gizmos",        "View / Toggle Gizmos",        { ImGuiKey_G, JCE_HKM_SHIFT }, {} },

    /* play */
    { "play.toggle",               "Play / Toggle",               { ImGuiKey_F5, JCE_HKM_NONE }, {} },
    { "play.step",                 "Play / Step",                 { ImGuiKey_F10, JCE_HKM_NONE }, {} },
    { "play.pause",                "Play / Pause",                { ImGuiKey_Pause, JCE_HKM_NONE }, {} },

    /* ui */
    { "ui.command_palette",        "UI / Command Palette",        { ImGuiKey_P, JCE_HKM_CTRL }, {} },
    { "ui.find_in_hierarchy",      "UI / Find in Hierarchy",      { ImGuiKey_F, JCE_HKM_CTRL }, {} },
    { "ui.find_in_assets",         "UI / Find in Assets",         { ImGuiKey_F, (uint8_t)(JCE_HKM_CTRL | JCE_HKM_ALT) }, {} },
    { "ui.toggle_fullscreen_view", "UI / Toggle Fullscreen Panel",{ ImGuiKey_F11, JCE_HKM_NONE }, {} },
    { "ui.screenshot",             "UI / Screenshot (PNG)",       { ImGuiKey_F12, JCE_HKM_NONE }, {} },
    { "ui.record",                 "UI / Record toggle (frames)", { ImGuiKey_F9, JCE_HKM_NONE }, {} },
    { "file.build_settings",       "File / Build Settings",       { ImGuiKey_B, JCE_HKM_CTRL }, {} },
    { "file.pack_current_scene",   "File / Pack Current Scene",   { ImGuiKey_B, (uint8_t)(JCE_HKM_CTRL | JCE_HKM_SHIFT) }, {} },

    /* panel toggles */
    { "panel.console",             "Panel / Toggle Console",      { ImGuiKey_F4,  JCE_HKM_NONE }, {} },
    { "panel.profiler",            "Panel / Toggle Profiler",     { ImGuiKey_F7,  JCE_HKM_NONE }, {} },
    { "panel.hierarchy",           "Panel / Toggle Hierarchy",    { ImGuiKey_F6,  JCE_HKM_NONE }, {} },
    { "panel.inspector",           "Panel / Toggle Inspector",    { ImGuiKey_F8,  JCE_HKM_NONE }, {} },
    { "panel.assets",              "Panel / Toggle Assets",       { ImGuiKey_F3,  JCE_HKM_NONE }, {} },
    { "panel.search",              "Panel / Toggle Search",       { ImGuiKey_F,   (uint8_t)(JCE_HKM_CTRL | JCE_HKM_SHIFT) }, {} },
    { "panel.project_settings",    "Panel / Project Settings",    { ImGuiKey_P,   (uint8_t)(JCE_HKM_CTRL | JCE_HKM_SHIFT) }, {} },
    { "edit.preferences",          "Edit / Preferences",          { ImGuiKey_Comma, JCE_HKM_CTRL }, {} },

    /* Maya-style workspaces (Ctrl+F1..F7). */
    { "workspace.default",         "Workspace / Default",         { ImGuiKey_F1, JCE_HKM_CTRL }, {} },
    { "workspace.modeling",        "Workspace / Modeling",        { ImGuiKey_F2, JCE_HKM_CTRL }, {} },
    { "workspace.rigging",         "Workspace / Rigging",         { ImGuiKey_F3, JCE_HKM_CTRL }, {} },
    { "workspace.animation",       "Workspace / Animation",       { ImGuiKey_F4, JCE_HKM_CTRL }, {} },
    { "workspace.fx",              "Workspace / FX",              { ImGuiKey_F5, JCE_HKM_CTRL }, {} },
    { "workspace.rendering",       "Workspace / Rendering",       { ImGuiKey_F6, JCE_HKM_CTRL }, {} },
    { "workspace.uv_editing",      "Workspace / UV Editing",      { ImGuiKey_F7, JCE_HKM_CTRL }, {} },
};
/* clang-format on */

void hotkeys_path(char *out, size_t n)
{
    /* Sit alongside editor-config.json in the per-user config dir (~/.jce). */
    jce_editor_dotjce_path("hotkeys.json", out, n);
}

int chord_imgui(JceHotkeyChord c)
{
    int chord = 0;
    if (c.mods & JCE_HKM_CTRL)  chord |= ImGuiMod_Ctrl;
    if (c.mods & JCE_HKM_SHIFT) chord |= ImGuiMod_Shift;
    if (c.mods & JCE_HKM_ALT)   chord |= ImGuiMod_Alt;
    if (c.mods & JCE_HKM_SUPER) chord |= ImGuiMod_Super;
    chord |= c.key;
    return chord;
}

const char *imgui_key_name(int key)
{
    if (key <= 0) return "";
    return ImGui::GetKeyName((ImGuiKey)key);
}

bool initialized = false;
int consumed_frame = -1;
ImGuiKeyChord consumed_chords[JCE_HK_COUNT] = {};
int consumed_chord_count = 0;

void reset_consumed_if_needed(void)
{
    int frame = ImGui::GetFrameCount();
    if (frame == consumed_frame)
        return;
    consumed_frame = frame;
    consumed_chord_count = 0;
}

bool chord_consumed(ImGuiKeyChord chord)
{
    reset_consumed_if_needed();
    for (int i = 0; i < consumed_chord_count; i++)
        if (consumed_chords[i] == chord)
            return true;
    return false;
}

void consume_chord(ImGuiKeyChord chord)
{
    reset_consumed_if_needed();
    if (consumed_chord_count >= (int)(sizeof(consumed_chords) /
                                      sizeof(consumed_chords[0])))
        return;
    consumed_chords[consumed_chord_count++] = chord;
}

} /* namespace */

extern "C" void jce_hotkeys_init(void)
{
    if (initialized) return;
    for (int i = 0; i < JCE_HK_COUNT; ++i) {
        s_table[i].cur = s_table[i].def;
    }
    initialized = true;
    jce_hotkeys_load();
}

extern "C" void jce_hotkeys_shutdown(void)
{
    initialized = false;
}

extern "C" void jce_hotkeys_reset_all(void)
{
    for (int i = 0; i < JCE_HK_COUNT; ++i) {
        s_table[i].cur = s_table[i].def;
    }
}

extern "C" void jce_hotkey_reset(JceHotkeyId id)
{
    if (id < 0 || id >= JCE_HK_COUNT) return;
    s_table[id].cur = s_table[id].def;
}

extern "C" int jce_hotkeys_count(void)
{
    return (int)JCE_HK_COUNT;
}

extern "C" bool jce_hotkey_chord_equal(JceHotkeyChord a, JceHotkeyChord b)
{
    if (a.key <= 0 || b.key <= 0) return false;
    return a.key == b.key && a.mods == b.mods;
}

extern "C" JceHotkeyId jce_hotkey_find_conflict(JceHotkeyId for_id,
                                                 JceHotkeyChord chord)
{
    if (chord.key <= 0) return JCE_HK_COUNT;
    for (int i = 0; i < JCE_HK_COUNT; ++i) {
        if (i == (int)for_id) continue;
        if (jce_hotkey_chord_equal(s_table[i].cur, chord))
            return (JceHotkeyId)i;
    }
    return JCE_HK_COUNT;
}

extern "C" const char *jce_hotkey_name(JceHotkeyId id)
{
    if (id < 0 || id >= JCE_HK_COUNT) return "";
    return s_table[id].display;
}
extern "C" const char *jce_hotkey_id_string(JceHotkeyId id)
{
    if (id < 0 || id >= JCE_HK_COUNT) return "";
    return s_table[id].id_string;
}
extern "C" JceHotkeyChord jce_hotkey_get(JceHotkeyId id)
{
    if (id < 0 || id >= JCE_HK_COUNT) return JceHotkeyChord{0, 0};
    return s_table[id].cur;
}
extern "C" JceHotkeyChord jce_hotkey_get_default(JceHotkeyId id)
{
    if (id < 0 || id >= JCE_HK_COUNT) return JceHotkeyChord{0, 0};
    return s_table[id].def;
}
extern "C" void jce_hotkey_set(JceHotkeyId id, JceHotkeyChord chord)
{
    if (id < 0 || id >= JCE_HK_COUNT) return;
    s_table[id].cur = chord;
}

extern "C" bool jce_hotkey_pressed(JceHotkeyId id)
{
    if (id < 0 || id >= JCE_HK_COUNT) return false;
    JceHotkeyChord c = s_table[id].cur;
    if (c.key <= 0) return false;
    ImGuiKeyChord chord = (ImGuiKeyChord)chord_imgui(c);
    if (chord_consumed(chord))
        return false;
    if (!ImGui::IsKeyPressed((ImGuiKey)c.key, false))
        return false;
    if (!ImGui::IsKeyChordPressed(chord))
        return false;
    consume_chord(chord);
    return true;
}

extern "C" char *jce_hotkey_chord_label(JceHotkeyChord c, char *out, size_t n)
{
    if (!out || n == 0) return out;
    out[0] = '\0';
    char tmp[96] = {0};
    size_t off = 0;
    auto append = [&](const char *s) {
        size_t l = std::strlen(s);
        if (off + l + 1 < sizeof(tmp)) {
            std::memcpy(tmp + off, s, l);
            off += l;
            tmp[off] = '\0';
        }
    };
    if (c.mods & JCE_HKM_CTRL)  append("Ctrl+");
    if (c.mods & JCE_HKM_SHIFT) append("Shift+");
    if (c.mods & JCE_HKM_ALT)   append("Alt+");
    if (c.mods & JCE_HKM_SUPER) append("Super+");
    if (c.key > 0) append(imgui_key_name(c.key));
    if (off == 0) std::snprintf(tmp, sizeof(tmp), "(unbound)");
    std::strncpy(out, tmp, n - 1);
    out[n - 1] = '\0';
    return out;
}

/* ── Persistence (very small JSON; no library) ─────────────────────── */

extern "C" bool jce_hotkeys_save(void)
{
    if (!initialized) return false;
    char path[640];
    hotkeys_path(path, sizeof(path));

    /* Build JSON in a heap buffer; bound estimate per row ≈ 120 bytes. */
    size_t cap = 64 + (size_t)JCE_HK_COUNT * 128;
    char *buf = (char *)ED_MALLOC(cap);
    if (!buf) return false;
    size_t off = 0;
    int n = std::snprintf(buf + off, cap - off, "{\n  \"hotkeys\": [\n");
    if (n < 0 || (size_t)n >= cap - off) { ED_FREE(buf); return false; }
    off += (size_t)n;
    for (int i = 0; i < JCE_HK_COUNT; ++i) {
        n = std::snprintf(buf + off, cap - off,
            "    { \"id\": \"%s\", \"key\": %d, \"mods\": %u }%s\n",
            s_table[i].id_string,
            (int)s_table[i].cur.key,
            (unsigned)s_table[i].cur.mods,
            (i + 1 < JCE_HK_COUNT) ? "," : "");
        if (n < 0 || (size_t)n >= cap - off) { ED_FREE(buf); return false; }
        off += (size_t)n;
    }
    n = std::snprintf(buf + off, cap - off, "  ]\n}\n");
    if (n < 0 || (size_t)n >= cap - off) { ED_FREE(buf); return false; }
    off += (size_t)n;

    /* Atomic: a torn hotkeys.json parses as absent and silently resets
     * every binding to defaults. */
    bool ok = jce_fs_host_write_all_atomic(path, buf, off);
    ED_FREE(buf);
    return ok;
}

extern "C" bool jce_hotkeys_load(void)
{
    char path[640];
    hotkeys_path(path, sizeof(path));
    size_t len = 0;
    char *buf = (char *)ed_read_file(path, &len);
    if (!buf) return false;
    if (len > (1 << 20)) { ED_FREE(buf); return false; }

    /* Tiny scanner: look for "id": "...", "key": N, "mods": N triples. */
    const char *p = buf;
    while (p && *p) {
        const char *id_key = std::strstr(p, "\"id\"");
        if (!id_key) break;
        const char *q1 = std::strchr(id_key + 4, '"'); if (!q1) break;
        const char *q2 = std::strchr(q1 + 1,    '"'); if (!q2) break;
        char idbuf[64] = {0};
        size_t idlen = (size_t)(q2 - q1 - 1);
        if (idlen >= sizeof(idbuf)) idlen = sizeof(idbuf) - 1;
        std::memcpy(idbuf, q1 + 1, idlen);

        const char *key_key  = std::strstr(q2, "\"key\"");
        const char *mods_key = std::strstr(q2, "\"mods\"");
        if (!key_key || !mods_key) break;
        int keyv = 0; unsigned modsv = 0;
        std::sscanf(key_key,  "\"key\" : %d", &keyv);
        if (keyv == 0) std::sscanf(key_key,  "\"key\":%d",  &keyv);
        std::sscanf(mods_key, "\"mods\" : %u", &modsv);
        if (modsv == 0 && std::strstr(mods_key, "0") == nullptr)
            std::sscanf(mods_key, "\"mods\":%u", &modsv);

        for (int i = 0; i < JCE_HK_COUNT; ++i) {
            if (std::strcmp(s_table[i].id_string, idbuf) == 0) {
                s_table[i].cur.key  = keyv;
                s_table[i].cur.mods = (uint8_t)modsv;
                break;
            }
        }
        p = mods_key + 1;
    }
    ED_FREE(buf);
    return true;
}
