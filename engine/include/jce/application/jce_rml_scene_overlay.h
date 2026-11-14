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
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_timer.h>
#include <jce/renderer/jce_views.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_actions.h>
#include <jce/os/platform/jce_keys.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define JCE_RML_SCENE_BIND_MAX 96 /* Bounded scene values, controls and visibility. */
#define JCE_RML_SCENE_LINE_MAX 192

typedef struct JceRmlSceneBinding {
    char tag[64];
    JceUIElementHandle element;
    int kind; /* 0=text, 1=optional, 2=progress, 3=journal, 4=visible, 5=modal, 6=command, 7=key, 8=modal-key */
    uint64_t previous_hash;
    bool initialized;
    char command[64]; /* A bounded command for the target UIText mailbox. */
    int key_code;
    bool key_held;
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
    JceScene *scene; /* Borrowed only while the host owns this overlay and scene. */
    bool wait_release; /* Quarantine held controls across modal dismissal. */
    bool capture_owned, capture_saved; /* Runtime-host pointer handoff only. */
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

static bool jce_rml_scene_overlay_has_modal_controls(const JceRmlSceneOverlay *o)
{
    if (!o || !o->ui) return false;
    for (int i = 0; i < o->bind_count; ++i)
        if (o->binds[i].kind == 5) return true;
    return false;
}

/* A snapshot belongs to one sync/input operation, never to a frame or scene.
 * Resolve all requested tags in the same entity order as find_with_tag; a
 * first match without the requested UI component must not select a later one.
 * Tags and full entity handles can change between any two operations. */
typedef struct JceRmlSceneTargets {
    uint16_t tags[JCE_RML_SCENE_BIND_MAX];
    JceEntity entities[JCE_RML_SCENE_BIND_MAX];
    int bind_target[JCE_RML_SCENE_BIND_MAX];
    int count, remaining;
} JceRmlSceneTargets;

static void jce_rml_scene_overlay_resolve_entity(JceScene *scene, JceEntity e,
    void *userdata)
{
    JceRmlSceneTargets *targets = (JceRmlSceneTargets *)userdata;
    if (!targets->remaining) return;
    JceTagComponent *tag = jce_scene_get_tag_component(scene,e);
    const uint16_t tag_id = tag ? tag->tag_id : 0u;
    for (int i = 0; i < targets->count; ++i) {
        if (targets->tags[i] == tag_id && !targets->entities[i]) {
            targets->entities[i] = e;
            --targets->remaining;
            return;
        }
    }
}

static void jce_rml_scene_overlay_resolve(JceRmlSceneTargets *targets,
    const JceRmlSceneOverlay *o, JceScene *scene, uint32_t kind_mask)
{
    memset(targets,0,sizeof *targets);
    for (int i = 0; i < o->bind_count; ++i) {
        const JceRmlSceneBinding *b = &o->binds[i];
        targets->bind_target[i] = -1;
        if ((unsigned)b->kind > 8u || !(kind_mask & (1u << b->kind))) continue;
        const uint16_t tag_id = jce_tag_intern(b->tag);
        /* Match find_with_tag: an invalid/failed intern is not Untagged. */
        if (!b->tag[0] || (!tag_id && strcmp(b->tag,"Untagged") != 0)) continue;
        int target = 0;
        while (target < targets->count && targets->tags[target] != tag_id) ++target;
        if (target == targets->count) targets->tags[targets->count++] = tag_id;
        targets->bind_target[i] = target;
    }
    targets->remaining = targets->count;
    if (targets->count) jce_scene_each_entity(scene,jce_rml_scene_overlay_resolve_entity,targets);
}

static JceEntity jce_rml_scene_overlay_target(const JceRmlSceneTargets *targets,
    int binding)
{
    const int target = targets->bind_target[binding];
    return target >= 0 ? targets->entities[target] : JCE_ENTITY_INVALID;
}

static bool jce_rml_scene_overlay_modal_targets(const JceRmlSceneOverlay *o,
    JceScene *scene, const JceRmlSceneTargets *targets)
{
    for (int i = 0; i < o->bind_count; ++i) {
        if (o->binds[i].kind != 5) continue;
        JceEntity e = jce_rml_scene_overlay_target(targets,i);
        JceUIToggleComponent *toggle = e ? jce_scene_get_ui_toggle(scene,e) : NULL;
        if (toggle && toggle->is_on) return true;
    }
    return false;
}

static bool jce_rml_scene_overlay_modal_active(JceRmlSceneOverlay *o, JceScene *scene)
{
    if (!o || !o->ui || !scene) return false;
    JceRmlSceneTargets targets;
    jce_rml_scene_overlay_resolve(&targets,o,scene,1u << 5);
    return jce_rml_scene_overlay_modal_targets(o,scene,&targets);
}

/* Query before sampling gameplay. A host keeps raw UI input available while
 * it suppresses its game/controller sample and releases pointer capture. */
static bool jce_rml_scene_overlay_wants_input(JceRmlSceneOverlay *o, JceScene *scene)
{
    if (!o || !o->ui || !scene) return false;
    if (o->open_page || o->help_visible) return true;
    JceRmlSceneTargets targets;
    bool checked[JCE_RML_SCENE_BIND_MAX] = {false};
    jce_rml_scene_overlay_resolve(&targets,o,scene,
        (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8));
    for (int i = 0; i < o->bind_count; ++i) {
        JceRmlSceneBinding *b = &o->binds[i];
        if (b->kind != 6 && b->kind != 7 && b->kind != 8) continue;
        const int target = targets.bind_target[i];
        if (target < 0 || checked[target]) continue;
        checked[target] = true;
        JceEntity e = targets.entities[target];
        JceUITextComponent *text = e ? jce_scene_get_ui_text(scene,e) : NULL;
        if (text && text->text[0]) return true;
    }
    return jce_rml_scene_overlay_modal_targets(o,scene,&targets);
}

/* Preserve ownership until every pre-existing control has been released.
 * The neutral release frame is consumed too, preventing an old action edge
 * from becoming the first frame of resumed gameplay. */
static bool jce_rml_scene_overlay_blocks_gameplay(JceRmlSceneOverlay *o,
    JceScene *scene, const JceInput *input, const JceInputActions *actions)
{
    if (!o || !o->ui || !scene) return false;
    if (jce_rml_scene_overlay_wants_input(o,scene)) {
        o->wait_release = true;
        return true;
    }
    if (!o->wait_release) return false;
    if (input) {
        for (int key = 4; key < JCE_KEY_COUNT; ++key)
            if (jce_input_key_down(input,(JceKey)key)) return true;
        for (int button = 1; button <= 5; ++button)
            if (jce_input_mouse_button(input,button)) return true;
        if (jce_input_touch_count(input) > 0) return true;
    }
    if (actions) {
        int count = jce_actions_count(actions);
        for (int i = 0; i < count; ++i) {
            float value = jce_action_value(actions,i);
            if (value != value || value > 0.001f || value < -0.001f) return true;
        }
    }
    o->wait_release = false;
    return true;
}

/* Feed raw UI keys before the host masks the gameplay keyboard. Keys are
 * edge-triggered and share the same bounded first-event mailbox as buttons. */
static void jce_rml_scene_overlay_prepare_input(JceRmlSceneOverlay *o,
    JceScene *scene, const JceInput *input)
{
    if (!o || !o->ui || !scene) return;
    o->scene = scene;
    bool modal_checked = false, modal = false;
    bool targets_ready = false;
    JceRmlSceneTargets targets;
    for (int i = 0; i < o->bind_count; ++i) {
        JceRmlSceneBinding *b = &o->binds[i];
        if (b->kind != 7 && b->kind != 8) continue;
        bool down = input && jce_input_key_down(input,(JceKey)b->key_code);
        bool edge = down && !b->key_held;
        if (edge && b->kind == 8 && !modal_checked) {
            modal = jce_rml_scene_overlay_modal_active(o,scene);
            modal_checked = true;
        }
        if (edge && (b->kind == 7 || modal)) {
            if (!targets_ready) {
                jce_rml_scene_overlay_resolve(&targets,o,scene,(1u << 7) | (1u << 8));
                targets_ready = true;
            }
            JceEntity e = jce_rml_scene_overlay_target(&targets,i);
            JceUITextComponent *text = e ? jce_scene_get_ui_text(scene,e) : NULL;
            if (text && !text->text[0])
                memcpy(text->text,b->command,strlen(b->command) + 1u);
        }
        b->key_held = down;
    }
}

static void jce_rml_scene_overlay_click(JceUIElementHandle e,
                                         const char *type, void *userdata)
{
    JceRmlSceneOverlay *o = (JceRmlSceneOverlay *)userdata;
    if (!o || !type || strcmp(type, "click") != 0) return;
    for (int i = 0; i < o->bind_count; ++i) {
        JceRmlSceneBinding *b = &o->binds[i];
        if (b->kind != 6 || b->element.idx != e.idx || !o->scene) continue;
        JceEntity target = jce_scene_find_with_tag(o->scene,b->tag);
        JceUITextComponent *text = target ? jce_scene_get_ui_text(o->scene,target) : NULL;
        if (text && !text->text[0]) {
            /* A one-event mailbox: do not overwrite an unconsumed click. */
            memcpy(text->text,b->command,strlen(b->command) + 1u);
        }
        return;
    }
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
 * `progress|element_id|scene_tag`, `visible|element_id|scene_tag`,
 * `modal|element_id|scene_tag`, `command|button_id|scene_tag`, or
 * `key|decimal_scancode|scene_tag` or `modal-key|decimal_scancode|scene_tag`
 * rows (0 <= key < JCE_KEY_COUNT). Modal keys only enqueue while a modal is on.
 * Visible/modal read UIToggle.is_on without replacing element children.
 * Commands write their button id to a UIText mailbox; the consumer clears it.
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
                   strcmp(row, "journal") == 0 ? 3 :
                   strcmp(row, "visible") == 0 ? 4 :
                   strcmp(row, "modal") == 0 ? 5 :
                   strcmp(row, "command") == 0 ? 6 :
                   strcmp(row, "key") == 0 ? 7 :
                   strcmp(row, "modal-key") == 0 ? 8 : -1;
        if (kind < 0) return false;
        JceRmlSceneBinding *b = &parsed[parsed_count++];
        b->kind = kind;
        if (kind == 6) memcpy(b->command,sep1,strlen(sep1) + 1u);
        if (kind == 7 || kind == 8) {
            size_t key_len = strlen(sep1);
            if (!key_len || key_len > 3u) return false;
            int key = 0;
            for (size_t k = 0; k < key_len; ++k) {
                if (sep1[k] < '0' || sep1[k] > '9') return false;
                key = key*10 + (sep1[k] - '0');
            }
            if (key >= JCE_KEY_COUNT) return false;
            b->key_code = key;
            snprintf(b->command,sizeof b->command,"key:%d",key);
            for (int j = 0; j < parsed_count - 1; ++j)
                if ((parsed[j].kind == 7 || parsed[j].kind == 8) && parsed[j].key_code == key) return false;
        }
        memcpy(b->tag, sep2, strlen(sep2) + 1);
        /* Temporary sentinel: resolved below, after UI context creation. */
        b->element.idx = UINT32_MAX;
        for (int j = 0; j < parsed_count - 1; ++j)
            if (strcmp(parsed[j].tag, b->tag) == 0 &&
                !((parsed[j].kind >= 6 && parsed[j].kind <= 8) &&
                  (b->kind >= 6 && b->kind <= 8))) return false;
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
        if (parsed[idx].kind == 7 || parsed[idx].kind == 8) { ++idx; continue; }
        parsed[idx].element = jce_ui_find_element(o->ui,o->document,id);
        if (!jce_ui_elem_valid(parsed[idx].element)) {
            jce_rml_scene_overlay_close(o); return false;
        }
        idx++;
    }
    for (int i = 0; i < parsed_count; ++i) o->binds[i] = parsed[i];
    o->bind_count = parsed_count;
    for (int i = 0; i < parsed_count; ++i) {
        if (o->binds[i].kind == 6 &&
            !jce_ui_elem_on(o->ui,o->binds[i].element,"click",jce_rml_scene_overlay_click,o)) {
            jce_rml_scene_overlay_close(o); return false;
        }
    }
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
    o->scene = scene;
    JceRmlSceneTargets targets;
    jce_rml_scene_overlay_resolve(&targets,o,scene,(1u << 6) - 1u);
    for (int i = 0; i < o->bind_count; ++i) {
        JceRmlSceneBinding *b = &o->binds[i];
        if (b->kind >= 6 && b->kind <= 8) continue;
        JceEntity e = jce_rml_scene_overlay_target(&targets,i);
        if (b->kind == 4 || b->kind == 5) {
            JceUIToggleComponent *p = e ? jce_scene_get_ui_toggle(scene,e) : NULL;
            bool visible = p && p->is_on;
            if (b->initialized && b->previous_hash == (uint64_t)visible) continue;
            jce_ui_elem_set_property(o->ui,b->element,"display",visible ? "block" : "none");
            b->previous_hash = (uint64_t)visible;
            b->initialized = true;
        } else if (b->kind == 2) {
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
    const bool profile = jce_perf_phase_enabled() != 0;
    uint64_t phase_start = profile ? jce_time_perf_counter() : 0u;
    jce_rml_scene_overlay_sync(o,scene);
    if (profile) {
        const uint64_t now = jce_time_perf_counter();
        jce_perf_phase_add("rml_sync",jce_time_perf_to_ms(phase_start,now));
        phase_start = now;
    }
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
    /* Full-window hosts can retain ordinary focus/wheel handling. Embedded
     * views provide transformed pointer coordinates, which stay authoritative:
     * never feed both routes or a click can fire twice at different positions. */
    float raw_x = 0.0f, raw_y = 0.0f;
    if (input) jce_input_mouse_pos(input,&raw_x,&raw_y);
    bool matching_pointer = input && pointer_valid && raw_x == pointer_x && raw_y == pointer_y &&
        jce_input_mouse_button(input,1) == pointer_down;
    if (matching_pointer) jce_ui_process_input(o->ui,input);
    else jce_ui_process_pointer_state(o->ui,pointer_x,pointer_y,pointer_down,pointer_valid);
    jce_ui_update(o->ui,dt > 0 ? dt : 0.0f);
    jce_ui_render_view(o->ui,view_id);
    if (profile)
        jce_perf_phase_add("rml_render",jce_time_perf_to_ms(phase_start,jce_time_perf_counter()));
}

/* Optional runtime-only convenience. Implemented here because default_main
 * is itself a one-translation-unit host template, not a reusable module.
 * Defined for the default host only; Editor uses the shared overlay API. */
#ifdef JCE_RML_SCENE_HOST_RUNTIME
#include <jce/os/platform/jce_window.h>
static bool jce_rml_scene_overlay_runtime_input(JceRmlSceneOverlay *o,
    JceScene *scene, const JceInput *input, const JceInputActions *actions,
    JceWindow *window, bool *captured, bool allow_capture)
{
    jce_rml_scene_overlay_prepare_input(o,scene,input);
    bool blocked = jce_rml_scene_overlay_blocks_gameplay(o,scene,input,actions);
    if (!o || !captured) return blocked;
    if (blocked) {
        if (!o->capture_owned) {
            o->capture_saved = *captured;
            o->capture_owned = true;
        }
        if (*captured && window) {
            jce_window_set_relative_mouse_mode(window,false);
            jce_window_set_mouse_grab(window,false);
        }
        *captured = false;
    } else if (o->capture_owned && (!o->capture_saved || allow_capture)) {
        *captured = o->capture_saved && allow_capture;
        if (window) {
            jce_window_set_relative_mouse_mode(window,*captured);
            jce_window_set_mouse_grab(window,*captured);
        }
        o->capture_owned = false;
    }
    return blocked;
}

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
