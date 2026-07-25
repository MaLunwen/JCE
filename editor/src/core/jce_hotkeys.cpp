/*
 * jce_hotkeys.cpp  Implementation of the hotkey registry.
 */

#include "jce_hotkeys.h"
#include "jce_editor_config.h"
#include "jce_editor_i18n.h"   /* localize hotkey display names (hotkey.<id_string>) */
#include "jce_editor_alloc.h"
#include "io/jce_editor_file_util.h"

#include <jce/tools/jce_imgui.hpp>
#include <jce/os/core/jce_json.h>
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

    { "edit.snap_to_ground",       "Edit / Snap To Ground",       { ImGuiKey_End, JCE_HKM_NONE }, {} },
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
    /* Localized display name: key = "hotkey.<id_string>", English `display`
     * as the fallback. Localizes every consumer (Hotkeys tab, Preferences,
     * User Guide) without touching them. i18n_or returns a persistent pointer
     * (the resolved translation or the fallback), so `key` being local is fine. */
    char key[96];
    std::snprintf(key, sizeof(key), "hotkey.%s", s_table[id].id_string);
    return jce_editor_i18n_or(key, s_table[id].display);
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

/* ── Persistence (schema: { "hotkeys": [ {id,key,mods}, ... ] }) ────── */

extern "C" bool jce_hotkeys_save(void)
{
    if (!initialized) return false;
    char path[640];
    hotkeys_path(path, sizeof(path));

    JceJson *root = jce_json_object();
    JceJson *arr  = jce_json_array();
    if (!root || !arr) {
        jce_json_free(root);
        jce_json_free(arr);
        return false;
    }
    jce_json_set_child(root, "hotkeys", arr);   /* arr now owned by root */
    for (int i = 0; i < JCE_HK_COUNT; ++i) {
        JceJson *item = jce_json_object();
        if (!item) { jce_json_free(root); return false; }
        jce_json_set_string(item, "id",   s_table[i].id_string);
        jce_json_set_int   (item, "key",  s_table[i].cur.key);
        jce_json_set_int   (item, "mods", (int)s_table[i].cur.mods);
        jce_json_array_push(arr, item);
    }

    char *text = jce_json_print(root, /*pretty=*/true);
    jce_json_free(root);
    if (!text) return false;

    /* Atomic: a torn hotkeys.json parses as absent and silently resets
     * every binding to defaults.  (Hence printing to a buffer instead of
     * jce_json_write_file, which is a plain write.) */
    bool ok = jce_fs_host_write_all_atomic(path, text, std::strlen(text));
    jce_json_free_string(text);
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

    JceJson *root = jce_json_parse(buf, len);
    ED_FREE(buf);
    if (!root) return false;   /* malformed: keep the bindings already loaded */

    JceJson *arr = jce_json_get(root, "hotkeys");
    int n = jce_json_array_size(arr);   /* 0 when absent or not an array */
    for (int e = 0; e < n; ++e) {
        JceJson *item = jce_json_array_at(arr, e);
        const char *id = jce_json_string_value(jce_json_get(item, "id"), nullptr);
        /* A row without "key" says nothing about the binding, so leave it
         * alone; key 0 is a deliberate unbind and must round-trip. */
        if (!id || !jce_json_has(item, "key")) continue;

        /* Ids no longer in s_table (renamed / retired) find no match and are
         * silently skipped. */
        for (int i = 0; i < JCE_HK_COUNT; ++i) {
            if (std::strcmp(s_table[i].id_string, id) == 0) {
                s_table[i].cur.key  = jce_json_get_int(item, "key", 0);
                s_table[i].cur.mods = (uint8_t)(jce_json_get_int(item, "mods", 0) & 0xFF);
                break;
            }
        }
    }
    jce_json_free(root);
    return true;
}
