/* JCE optional scene -> RmlUi document bridge. C99/C++20 compatible.
 * Contract: a project opts in by providing ui/hud.rml and ui/hud.bindings.
 * Scene-owned UI components remain the sole game-state/UI-value source.
 * This helper is deliberately host-side, not in middleware/ui (which must
 * remain independent of scene/ECS). Document ownership is explicit. */
#ifndef JCE_RML_SCENE_OVERLAY_H
#define JCE_RML_SCENE_OVERLAY_H

#include <jce/middleware/ui/jce_ui.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_views.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_keys.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define JCE_RML_SCENE_BIND_MAX 64 /* CK_0225_RML_BINDINGS: supports 36 validated CK slots */
#define JCE_RML_SCENE_LINE_MAX 192

typedef struct JceRmlSceneBinding {
    char tag[64];
    JceUIElementHandle element;
    int kind; /* 0=text, 1=optional text, 2=progress fraction, 3=journal */
    uint64_t previous_hash;
    bool initialized;
} JceRmlSceneBinding;

typedef struct JceRmlSceneOverlay {
    JceUIContext *ui;
    JceUIDocHandle document;
    JceRmlSceneBinding binds[JCE_RML_SCENE_BIND_MAX];
    int bind_count;
    JceUIElementHandle menu;
    JceUIElementHandle pages[3];
    JceUIElementHandle buttons[7];
    int open_page;
    JceUIElementHandle help_button, help_close, help_panel;
    bool help_visible;
    uint32_t width, height;
} JceRmlSceneOverlay;

static uint64_t jce_rml_scene_text_hash(const char *s)
{
    uint64_t h = UINT64_C(14695981039346656037);
    if (!s) return h;
    for (; *s; ++s) { h ^= (unsigned char)*s; h *= UINT64_C(1099511628211); }
    return h;
}

/* CK_0223_RESPONSIVE_HELP: scale based on the smaller viewport axis. */
static float jce_rml_scene_overlay_dp_ratio(uint32_t w, uint32_t h)
{
    float rw = (float)w / 1600.0f;
    float rh = (float)h / 900.0f;
    float scale = rw < rh ? rw : rh;
    if (scale < 0.78f) scale = 0.78f;
    if (scale > 1.60f) scale = 1.60f;
    return scale;
}

static void jce_rml_scene_overlay_help(JceRmlSceneOverlay *o, bool visible)
{
    if (!o || !o->ui) return;
    o->help_visible = visible;
    if (jce_ui_elem_valid(o->help_panel))
        jce_ui_elem_set_property(o->ui, o->help_panel, "display", visible ? "block" : "none");
}

static void jce_rml_scene_overlay_page(JceRmlSceneOverlay *o, int page)
{
    if (!o || !o->ui) return;
    o->open_page = page;
    if (jce_ui_elem_valid(o->menu))
        jce_ui_elem_set_property(o->ui, o->menu, "display", page ? "block" : "none");
    for (int i = 0; i < 3; ++i) {
        if (jce_ui_elem_valid(o->pages[i]))
            jce_ui_elem_set_property(o->ui, o->pages[i], "display", page == i + 1 ? "block" : "none");
    }
    /* CK_0224_MENU_ACTIVE_TABS: a selected tab must be visually identifiable.
     * The selected state belongs to the existing document controller. */
    for (int i = 0; i < 3; ++i) {
        JceUIElementHandle tab = o->buttons[4 + i];
        if (!jce_ui_elem_valid(tab)) continue;
        bool selected = page == i + 1;
        jce_ui_elem_set_property(o->ui, tab, "color", selected ? "#f3eee5" : "#a5b6ad");
        jce_ui_elem_set_property(o->ui, tab, "border-bottom-color", selected ? "#daa968" : "transparent");
    }
}

static void jce_rml_scene_overlay_click(JceUIElementHandle e,
                                         const char *type, void *userdata)
{
    JceRmlSceneOverlay *o = (JceRmlSceneOverlay *)userdata;
    if (!o || !type || strcmp(type, "click") != 0) return;
    if (jce_ui_elem_valid(o->help_button) && o->help_button.idx == e.idx) {
        jce_rml_scene_overlay_help(o, !o->help_visible);
        return;
    }
    if (jce_ui_elem_valid(o->help_close) && o->help_close.idx == e.idx) {
        jce_rml_scene_overlay_help(o, false);
        return;
    }
    for (int i = 0; i < 7; ++i) {
        if (jce_ui_elem_valid(o->buttons[i]) && o->buttons[i].idx == e.idx) {
            jce_rml_scene_overlay_page(o, i == 3 ? 0 : i < 3 ? i + 1 : i - 3);
            return;
        }
    }
}

static void jce_rml_scene_overlay_close(JceRmlSceneOverlay *o)
{
    if (!o) return;
    if (o->ui) jce_ui_destroy(o->ui);
    memset(o, 0, sizeof *o);
}

/* Parses a small, UTF-8 ASCII-key manifest of `text|element_id|scene_tag`,
 * `optional|element_id|scene_tag`, `journal|element_id|scene_tag`,
 * or `progress|element_id|scene_tag` rows.
 * Validates all rows before publishing the overlay. No CK identifiers here. */
static bool jce_rml_scene_overlay_open(JceRmlSceneOverlay *o,
    JceRenderer *renderer, JcePakArchive *pak, const char *markup,
    size_t markup_bytes, const char *bindings, size_t bindings_bytes,
    uint32_t width, uint32_t height)
{
    if (!o || o->ui || !renderer || !markup || !bindings || !markup_bytes ||
        !bindings_bytes || markup_bytes > 1024u*1024u ||
        bindings_bytes > 8u*1024u || !width || !height) return false;
    /* Decode and validate all bindings with no partial UI lifetime. */
    JceRmlSceneBinding parsed[JCE_RML_SCENE_BIND_MAX];
    memset(parsed, 0, sizeof parsed);
    int parsed_count = 0;
    size_t p = 0;
    while (p < bindings_bytes) {
        const size_t start = p;
        while (p < bindings_bytes && bindings[p] != '\n') p++;
        const size_t span = p - start;
        if (p < bindings_bytes) p++;
        if (span == 0) continue;
        if (span >= JCE_RML_SCENE_LINE_MAX) return false;
        char row[JCE_RML_SCENE_LINE_MAX];
        memcpy(row, bindings + start, span); row[span] = 0;
        if (span && row[span - 1] == '\r') row[span - 1] = 0;
        if (!row[0] || row[0] == '#') continue;
        char *sep1 = strchr(row, '|');
        if (!sep1) return false;
        *sep1++ = 0;
        char *sep2 = strchr(sep1, '|');
        if (!sep2) return false;
        *sep2++ = 0;
        if (!*sep1 || !*sep2 || strchr(sep2, '|') ||
            strlen(sep1) >= 64 || strlen(sep2) >= 64 ||
            parsed_count >= JCE_RML_SCENE_BIND_MAX) return false;
        int kind = strcmp(row, "text") == 0 ? 0 :
                   strcmp(row, "optional") == 0 ? 1 :
                   strcmp(row, "progress") == 0 ? 2 :
                   strcmp(row, "journal") == 0 ? 3 : -1;
        if (kind < 0) return false;
        JceRmlSceneBinding *b = &parsed[parsed_count++];
        b->kind = kind;
        memcpy(b->tag, sep2, strlen(sep2) + 1);
        /* Temporary sentinel: resolved below, after UI context creation. */
        b->element.idx = UINT32_MAX;
        for (int j = 0; j < parsed_count - 1; ++j)
            if (strcmp(parsed[j].tag, b->tag) == 0) return false;
        /* RML id is kept in a separate stack table until handles resolve. */
    }
    if (parsed_count <= 0) return false;
    JceUIContextDesc d; memset(&d, 0, sizeof d);
    d.width = width; d.height = height; d.renderer = renderer; d.pak = pak;
    o->ui = jce_ui_create(&d, jce_allocator_default());
    if (!o->ui) return false;
    o->width = width; o->height = height;
    /* Explicit pixel ratio. CSS px is a device pixel in the Game View target;
     * do not silently scale it to 0.6 at 720p. */
    jce_ui_set_dpi_scale(o->ui, jce_rml_scene_overlay_dp_ratio(width,height));
    o->document = jce_ui_doc_load(o->ui, "scene-hud", markup, (uint32_t)markup_bytes);
    if (!jce_ui_doc_valid(o->document)) { jce_rml_scene_overlay_close(o); return false; }
    /* Parse element ids again: this avoids a second copy in our fixed POD. */
    p = 0; int idx = 0;
    while (p < bindings_bytes) {
        const size_t start = p;
        while (p < bindings_bytes && bindings[p] != '\n') p++;
        size_t span = p-start;
        if (p < bindings_bytes) p++;
        if (!span || bindings[start] == '#') continue;
        if (span >= JCE_RML_SCENE_LINE_MAX) { jce_rml_scene_overlay_close(o); return false; }
        char row[JCE_RML_SCENE_LINE_MAX];
        memcpy(row, bindings+start, span); row[span]=0;
        char *sep = strchr(row,'|');
        if (!sep || idx >= parsed_count) { jce_rml_scene_overlay_close(o); return false; }
        char *id = sep+1; char *end = strchr(id,'|');
        if (!end) { jce_rml_scene_overlay_close(o); return false; }
        *end=0;
        parsed[idx].element = jce_ui_find_element(o->ui,o->document,id);
        if (!jce_ui_elem_valid(parsed[idx].element)) {
            jce_rml_scene_overlay_close(o); return false;
        }
        idx++;
    }
    for (int i = 0; i < parsed_count; ++i) o->binds[i] = parsed[i];
    o->bind_count = parsed_count;
    o->menu = jce_ui_find_element(o->ui,o->document,"rml_overlay_panel");
    o->help_panel = jce_ui_find_element(o->ui,o->document,"rml_help_panel");
    o->help_button = jce_ui_find_element(o->ui,o->document,"rml_help_button");
    o->help_close = jce_ui_find_element(o->ui,o->document,"rml_help_close");
    if (jce_ui_elem_valid(o->help_button) &&
        !jce_ui_elem_on(o->ui,o->help_button,"click",jce_rml_scene_overlay_click,o)) {
        jce_rml_scene_overlay_close(o); return false;
    }
    if (jce_ui_elem_valid(o->help_close) &&
        !jce_ui_elem_on(o->ui,o->help_close,"click",jce_rml_scene_overlay_click,o)) {
        jce_rml_scene_overlay_close(o); return false;
    }
    const char *page_ids[3] = {"rml_page_journal","rml_page_map","rml_page_pack"};
    const char *button_ids[7] = {"rml_nav_journal","rml_nav_map","rml_nav_pack",
        "rml_nav_close","rml_tab_journal","rml_tab_map","rml_tab_pack"};
    for (int i = 0; i < 3; ++i)
        o->pages[i] = jce_ui_find_element(o->ui,o->document,page_ids[i]);
    for (int i = 0; i < 7; ++i) {
        o->buttons[i] = jce_ui_find_element(o->ui,o->document,button_ids[i]);
        if (jce_ui_elem_valid(o->buttons[i]) &&
            !jce_ui_elem_on(o->ui,o->buttons[i],"click",jce_rml_scene_overlay_click,o)) {
            jce_rml_scene_overlay_close(o); return false;
        }
    }
    jce_rml_scene_overlay_page(o,0);
    jce_rml_scene_overlay_help(o,false);
    jce_ui_doc_show(o->ui,o->document);
    return true;
}

static void jce_rml_scene_overlay_sync(JceRmlSceneOverlay *o, JceScene *scene)
{
    if (!o || !o->ui || !scene) return;
    for (int i = 0; i < o->bind_count; ++i) {
        JceRmlSceneBinding *b = &o->binds[i];
        JceEntity e = jce_scene_find_with_tag(scene,b->tag);
        if (b->kind == 2) {
            JceUIProgressBarComponent *p = e ? jce_scene_get_ui_progress_bar(scene,e) : NULL;
            if (!p) continue;
            float span = p->max_value-p->min_value;
            float ratio = span > 0.0f ? (p->value-p->min_value)/span : 0.0f;
            if (ratio < 0) ratio = 0;
            if (ratio > 1) ratio = 1;
            int integer_percent = (int)(ratio*100.0f + .5f);
            if (b->initialized && b->previous_hash == (uint64_t)integer_percent) continue;
            char value[16]; snprintf(value,sizeof value,"%d%%",integer_percent);
            jce_ui_elem_set_property(o->ui,b->element,"width",value);
            b->previous_hash = (uint64_t)integer_percent;
            b->initialized = true;
        } else {
            JceUITextComponent *p = e ? jce_scene_get_ui_text(scene,e) : NULL;
            if (!p) continue;
            const char *value = p->text;
            uint64_t hash = jce_rml_scene_text_hash(value);
            if (b->initialized && b->previous_hash == hash) continue;
            jce_ui_elem_set_text(o->ui,b->element,value);
            if (b->kind == 1 || b->kind == 3)
                jce_ui_elem_set_property(o->ui,b->element,"display",value[0] ? "block" : "none");
            b->previous_hash = hash;
            b->initialized = true;
            /* The project journal toggles by updating a scene text component.
             * Automatically expose it in document UI on nonempty content. */
            if (b->kind == 3) {
                if (value[0]) jce_rml_scene_overlay_page(o,1);
                else if (o->open_page == 1) jce_rml_scene_overlay_page(o,0);
            }
        }
    }
}

static void jce_rml_scene_overlay_frame(JceRmlSceneOverlay *o, JceScene *scene,
    uint16_t view_id, uint32_t width, uint32_t height, float dt,
    bool pointer_valid, float pointer_x, float pointer_y, bool pointer_down,
    const JceInput *input)
{
    if (!o || !o->ui || !scene || !width || !height) return;
    if (o->width != width || o->height != height) {
        jce_ui_resize(o->ui,width,height);
        jce_ui_set_dpi_scale(o->ui,jce_rml_scene_overlay_dp_ratio(width,height));
        o->width = width; o->height = height;
    }
    jce_rml_scene_overlay_sync(o,scene);
    /* These keys are routed only when the native host provides an input
     * owner. The editor's Game View still uses its existing script-driven
     * J journal and mouse buttons; do not invent access to host ImGui input. */
    if (input) {
        if (jce_input_key_pressed(input,JCE_KEY_F1))
            jce_rml_scene_overlay_help(o,!o->help_visible);
        if (jce_input_key_pressed(input,JCE_KEY_M))
            jce_rml_scene_overlay_page(o,o->open_page == 2 ? 0 : 2);
        if (jce_input_key_pressed(input,JCE_KEY_I))
            jce_rml_scene_overlay_page(o,o->open_page == 3 ? 0 : 3);
        if (jce_input_key_pressed(input,JCE_KEY_ESCAPE) && o->help_visible)
            jce_rml_scene_overlay_help(o,false);
        if (jce_input_key_pressed(input,JCE_KEY_ESCAPE) && o->open_page)
            jce_rml_scene_overlay_page(o,0);
    }
    if (input) jce_ui_process_input(o->ui,input);
    else jce_ui_process_pointer_state(o->ui,pointer_x,pointer_y,pointer_down,pointer_valid);
    jce_ui_update(o->ui,dt > 0 ? dt : 0.0f);
    jce_ui_render_view(o->ui,view_id);
}

/* Optional runtime-only convenience. Implemented here because default_main
 * is itself a one-translation-unit host template, not a reusable module.
 * Defined for the default host only; Editor uses the shared overlay API. */
#ifdef JCE_RML_SCENE_HOST_RUNTIME
static void jce_rml_scene_overlay_runtime_tick(
    JceRmlSceneOverlay *o, bool *checked, JceFileSystem *fs,
    JceRenderer *renderer, JcePakArchive *pak, JceScene *scene,
    uint32_t w, uint32_t h, float dt, bool ptr_valid,
    float px, float py, bool ptr_down, const JceInput *input)
{
    if (!o || !checked || !scene || !renderer || !w || !h) return;
    if (!*checked && fs) {
        *checked = true;
        uint64_t doc_size = 0, map_size = 0;
        void *doc = jce_fs_read_all(fs, "ui/hud.rml", &doc_size);
        void *map = jce_fs_read_all(fs, "ui/hud.bindings", &map_size);
        if (doc && map && doc_size <= 1024u*1024u && map_size <= 8192u &&
            jce_rml_scene_overlay_open(o, renderer, pak, (const char *)doc,
                (size_t)doc_size, (const char *)map, (size_t)map_size,w,h)) {
            LOG_INFO("app", "RmlUi document HUD active");
        } else if (doc || map) {
            LOG_WARN("app", "RmlUi HUD present but could not load");
        }
        jce_fs_buffer_free(doc);
        jce_fs_buffer_free(map);
    }
    if (o->ui)
        jce_rml_scene_overlay_frame(o,scene,JCE_VIEW_UI,w,h,dt,
            ptr_valid,px,py,ptr_down,input);
}
#endif /* JCE_RML_SCENE_HOST_RUNTIME */

#endif /* JCE_RML_SCENE_OVERLAY_H */
