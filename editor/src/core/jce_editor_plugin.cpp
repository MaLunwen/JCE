/*
 * jce_editor_plugin.cpp  Static plugin panel registry.
 *
 * Linear array of up to JCE_MAX_PLUGIN_PANELS entries (32 today).  Id
 * lookups are O(N) — fine for the expected handful of plugins.  No
 * dynamic memory beyond strdup-ed ids/display names so unregister is
 * cheap and the table can be cleared at editor shutdown.
 */

#include "jce_editor_plugin.h"

#include <jce/tools/jce_imgui.hpp>

#include <cstring>
#include <cstdlib>

#define JCE_MAX_PLUGIN_PANELS 32

namespace {

struct Slot {
    bool                  active;
    JceEditorPluginPanel  desc;
    bool                  visible;
    char                 *id_owned;
    char                 *name_owned;
};

Slot s_slots[JCE_MAX_PLUGIN_PANELS];

int find_by_id(const char *id)
{
    if (!id) return -1;
    for (int i = 0; i < JCE_MAX_PLUGIN_PANELS; ++i)
        if (s_slots[i].active && s_slots[i].id_owned &&
            strcmp(s_slots[i].id_owned, id) == 0) return i;
    return -1;
}

int find_free_slot(void)
{
    for (int i = 0; i < JCE_MAX_PLUGIN_PANELS; ++i)
        if (!s_slots[i].active) return i;
    return -1;
}

void free_slot(Slot *s)
{
    std::free(s->id_owned);
    std::free(s->name_owned);
    s->id_owned   = nullptr;
    s->name_owned = nullptr;
    s->active     = false;
}

} /* namespace */

extern "C" bool jce_editor_plugin_register_panel(const JceEditorPluginPanel *desc)
{
    if (!desc || !desc->id || !desc->id[0] || !desc->content_fn) return false;

    /* Replace existing entry with the same id (hot-reload friendly). */
    int idx = find_by_id(desc->id);
    if (idx < 0) idx = find_free_slot();
    if (idx < 0) return false;

    Slot &s = s_slots[idx];
    if (s.active) free_slot(&s);

    s.active     = true;
    s.desc       = *desc;
    s.visible    = desc->default_visible;
    s.id_owned   = strdup(desc->id);
    s.name_owned = desc->display_name ? strdup(desc->display_name) : nullptr;
    /* Repoint into our owned strings so callers can safely free their
     * source buffers after register returns. */
    s.desc.id           = s.id_owned;
    s.desc.display_name = s.name_owned ? s.name_owned : s.id_owned;
    return true;
}

extern "C" bool jce_editor_plugin_unregister_panel(const char *id)
{
    int idx = find_by_id(id);
    if (idx < 0) return false;
    free_slot(&s_slots[idx]);
    return true;
}

extern "C" uint32_t jce_editor_plugin_panel_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < JCE_MAX_PLUGIN_PANELS; ++i)
        if (s_slots[i].active) n++;
    return n;
}

extern "C" const JceEditorPluginPanel *jce_editor_plugin_panel_at(uint32_t idx)
{
    uint32_t cur = 0;
    for (int i = 0; i < JCE_MAX_PLUGIN_PANELS; ++i) {
        if (!s_slots[i].active) continue;
        if (cur == idx) return &s_slots[i].desc;
        cur++;
    }
    return nullptr;
}

extern "C" bool *jce_editor_plugin_panel_visible_ptr(const char *id)
{
    int idx = find_by_id(id);
    if (idx < 0) return nullptr;
    return &s_slots[idx].visible;
}

extern "C" void jce_editor_plugin_render_all(void)
{
    for (int i = 0; i < JCE_MAX_PLUGIN_PANELS; ++i) {
        Slot &s = s_slots[i];
        if (!s.active || !s.visible || !s.desc.content_fn) continue;
        const char *title = s.desc.display_name ? s.desc.display_name : s.desc.id;
        if (ImGui::Begin(title, &s.visible)) {
            s.desc.content_fn();
        }
        ImGui::End();
    }
}
