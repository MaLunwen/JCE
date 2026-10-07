#include "jce_ui_canvas_storage.h"
#include <jce/os/core/jce_hash.h>
#include <jce/os/core/jce_hashmap.h>
#include "os/core/jce_memory.h"
#include <string.h>

bool uc_interaction_reserve_hits(UCInteractionStore *store, uint32_t needed)
{
    if (needed <= store->hit_capacity) return true;
    uint32_t capacity = jce_hashmap_cap_pow2(needed, 64u);
    if (capacity < needed || (size_t)capacity > SIZE_MAX / sizeof(*store->hits)) {
        store->allocation_failed = true;
        return false;
    }
    UCHit *hits = JCE_REALLOC(store->hits, (size_t)capacity * sizeof(*hits));
    if (!hits) { store->allocation_failed = true; return false; }
    store->hits = hits;
    store->hit_capacity = capacity;
    return true;
}

static uint32_t bucket(uint64_t entity, uint32_t capacity)
{
    return (uint32_t)jce_hash_fmix64(entity) & (capacity - 1u);
}

static bool grow_buttons(UCInteractionStore *store)
{
    uint32_t capacity = store->button_capacity ? store->button_capacity * 2u : 16u;
    if (capacity < store->button_capacity || (size_t)capacity > SIZE_MAX / sizeof(*store->buttons))
        return false;
    UCButtonState *buttons = JCE_CALLOC(capacity, sizeof(*buttons));
    if (!buttons) return false;
    for (uint32_t i = 0; i < store->button_capacity; ++i) {
        UCButtonState state = store->buttons[i];
        if (!state.entity) continue;
        uint32_t slot = bucket(state.entity, capacity);
        while (buttons[slot].entity) slot = (slot + 1u) & (capacity - 1u);
        buttons[slot] = state;
    }
    JCE_FREE(store->buttons);
    store->buttons = buttons;
    store->button_capacity = capacity;
    return true;
}

UCButtonState *uc_interaction_button(UCInteractionStore *store, uint64_t entity)
{
    if (!entity) return NULL;
    uint32_t slot = store->button_capacity ? bucket(entity, store->button_capacity) : 0;
    if (store->button_capacity) {
        while (store->buttons[slot].entity) {
            if (store->buttons[slot].entity == entity) return &store->buttons[slot];
            slot = (slot + 1u) & (store->button_capacity - 1u);
        }
    }
    if (store->button_count + 1u > store->button_capacity / 2u) {
        if (!grow_buttons(store)) { store->allocation_failed = true; return NULL; }
        slot = bucket(entity, store->button_capacity);
        while (store->buttons[slot].entity) slot = (slot + 1u) & (store->button_capacity - 1u);
    }
    UCButtonState *state = &store->buttons[slot];
    memset(state, 0, sizeof(*state));
    state->entity = entity;
    state->fade = 1.0f;
    state->dd_press = -1;
    ++store->button_count;
    return state;
}

static void remove_button(UCInteractionStore *store, uint32_t hole)
{
    uint32_t mask = store->button_capacity - 1u;
    uint32_t probe = (hole + 1u) & mask;
    while (store->buttons[probe].entity) {
        uint32_t home = bucket(store->buttons[probe].entity, store->button_capacity);
        if (jce_hashmap_shift_back(home, hole, probe, store->button_capacity)) {
            store->buttons[hole] = store->buttons[probe];
            hole = probe;
        }
        probe = (probe + 1u) & mask;
    }
    memset(&store->buttons[hole], 0, sizeof(store->buttons[hole]));
    --store->button_count;
}

void uc_interaction_begin(UCInteractionStore *store, JceScene *scene)
{
    store->allocation_failed = false;
    if (store->scene != scene) {
        if (store->buttons) memset(store->buttons, 0, (size_t)store->button_capacity * sizeof(*store->buttons));
        store->button_count = 0;
        store->scene = scene;
    }
    for (uint32_t i = 0; i < store->button_capacity;) {
        JceEntity entity = store->buttons[i].entity;
        if (entity && !jce_scene_has_ui_button(scene, entity) &&
                !jce_scene_has_ui_slider(scene, entity) && !jce_scene_has_ui_toggle(scene, entity) &&
                !jce_scene_has_ui_input_field(scene, entity) && !jce_scene_has_ui_dropdown(scene, entity))
            remove_button(store, i);
        else ++i;
    }
}

void uc_interaction_cancel_presses(UCInteractionStore *store)
{
    for (uint32_t i = 0; i < store->button_capacity; ++i)
        store->buttons[i].press_inside = false;
}

void uc_interaction_destroy(UCInteractionStore *store)
{
    JCE_FREE(store->buttons);
    JCE_FREE(store->hits);
    memset(store, 0, sizeof(*store));
}
