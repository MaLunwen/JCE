/*
 * jce_sr_anim.c  Scene-renderer animation module (split from
 * jce_scene_renderer.c).
 *
 * Per-instance morph (blendshape) vertex buffers, the 2D sprite animator,
 * avatar bone masks, frame events, IK passes (two-bone / foot / full-body),
 * the ragdoll override pass, animation retargeting, and the per-frame skinned
 * animation evaluation (sr_update_skinned_anims).  Pure move from the
 * monolithic renderer: cross-module entry points are declared in
 * jce_sr_internal.h, everything else stays file-static here.  No behaviour
 * change.
 */

#include "jce_sr_internal.h"

#include <jce/resource/jce_pak_loader.h>   /* PAK-first anim-SM / avatar-mask */

/* ── FEATURE 3.1 GPU morph deform: per-instance dynamic-VB lifecycle ──
 *
 * sr_init_morph_vbs marks every morph_vb slot invalid (a plain memset leaves
 * idx==0, a VALID bgfx handle — so this MUST run after any memset of the
 * instance).  sr_free_morph_vbs destroys all live VBs and resets the mapping;
 * it is called at EVERY instance-lifecycle site (model swap, slot reclaim,
 * renderer destroy) BEFORE the shared model is touched, mirroring the
 * retarget_map teardown, so a morph VB never outlives its instance and the
 * bgfx dynamic-VB handle pool can't leak (same failure class as the v0.9.8
 * pick-pass model leak). */
static void sr_init_morph_vbs(SrAnimInstance *a)
{
    if (!a) return;
    for (int i = 0; i < SR_MORPH_PRIM_MAX; i++) {
        a->morph_vb[i].idx   = UINT16_MAX;
        a->morph_vb_node[i]  = UINT32_MAX;
        a->morph_vb_prim[i]  = UINT32_MAX;
    }
    a->morph_vb_count    = 0;
    a->morph_last_count  = -1;   /* force first deform/upload */
}

void sr_free_morph_vbs(SrAnimInstance *a)
{
    if (!a) return;
    for (int i = 0; i < SR_MORPH_PRIM_MAX; i++) {
        if (a->morph_vb[i].idx != UINT16_MAX) {
            bgfx_destroy_dynamic_vertex_buffer(a->morph_vb[i]);
            a->morph_vb[i].idx = UINT16_MAX;
        }
        a->morph_vb_node[i] = UINT32_MAX;
        a->morph_vb_prim[i] = UINT32_MAX;
    }
    a->morph_vb_count   = 0;
    a->morph_last_count = -1;
}

/* ── O(1) entity -> anim slot index ──────────────────────────────────
 * Open-addressing hash over sr->anim_idx/anim_idx_keys (capacity 2x the
 * slot cap, power of two; value = slot+1, 0 = empty).  Replaces the linear
 * SR_ANIM_INSTANCE_MAX scan that per-entity per-pass hot paths (bind-pose
 * gates in color/shadow/velocity + the anim update) paid on every call —
 * ~6M comparisons/frame at 4000 skinned chars after the 256-slot raise.
 * Slots never free individually at runtime (wholesale reset at renderer
 * destroy), so the index only ever inserts / rebinds. */
#define SR_ANIM_IDX_CAP  ((uint32_t)SR_ANIM_INSTANCE_MAX * 2u)
#define SR_ANIM_IDX_MASK (SR_ANIM_IDX_CAP - 1u)

static uint32_t sr_anim_idx_hash(uint32_t entity)
{
    return (entity * 2654435761u) & SR_ANIM_IDX_MASK;
}

/* Insert or update entity -> slot.
 * Termination invariant: slots are never individually released at runtime
 * (only the wholesale renderer destroy), so a slot binds at most ONE entity
 * for the renderer's lifetime => live index entries <= SR_ANIM_INSTANCE_MAX
 * < capacity, and an empty cell always exists.  The probe caps at capacity
 * anyway so a future slot-recycling change degrades to a warn, not a hang. */
static void sr_anim_idx_put(JceSceneRenderer *sr, uint32_t entity, int slot)
{
    uint32_t h = sr_anim_idx_hash(entity);
    for (uint32_t n = 0; n < SR_ANIM_IDX_CAP; n++) {
        if (sr->anim_idx[h] == 0 || sr->anim_idx_keys[h] == entity) {
            sr->anim_idx[h]      = (uint16_t)(slot + 1);
            sr->anim_idx_keys[h] = entity;
            return;
        }
        h = (h + 1u) & SR_ANIM_IDX_MASK;
    }
    LOG_WARN(LOG_TAG, "anim slot index full — entity %u falls back to the "
             "slot record scan (index invariant violated?)", entity);
}

static int sr_anim_idx_get(const JceSceneRenderer *sr, uint32_t entity)
{
    uint32_t h = sr_anim_idx_hash(entity);
    for (uint32_t n = 0; n < SR_ANIM_IDX_CAP; n++) {
        if (sr->anim_idx[h] == 0) return -1;
        if (sr->anim_idx_keys[h] == entity) return (int)sr->anim_idx[h] - 1;
        h = (h + 1u) & SR_ANIM_IDX_MASK;
    }
    return -1;
}

/* Find an existing per-entity animation instance (no creation).  O(1). */
SrAnimInstance *sr_find_anim_instance(JceSceneRenderer *sr, uint32_t entity)
{
    int slot = sr_anim_idx_get(sr, entity);
    if (slot < 0) return NULL;
    SrAnimInstance *a = &sr->anim_inst[slot];
    /* A reclaimed slot may now belong to another entity (stale index entry
     * for the OLD key): the slot's own record is the source of truth. */
    return (a->used && a->entity == entity) ? a : NULL;
}

/* Get-or-create the per-entity animation instance for `entity` bound to the
   shared `model`. Creates a fresh player (from the model's read-only skeleton)
   and rebuilds it if the entity's model changed (skeleton_path reassigned). */
/* Release a frame-event pool and reset the dispatch state so the sidecar is
 * re-loaded lazily on the next frame (used on slot reclaim / model swap). */
static void sr_anim_events_reset(SrAnimInstance *a)
{
    if (!a) return;
    if (a->ev_pool) { JCE_FREE(a->ev_pool); a->ev_pool = NULL; }
    a->ev_pool_count  = 0;
    a->ev_track_count = 0;
    a->ev_loaded      = false;
    a->ev_clip        = -1;
    a->ev_prev_time   = 0.0f;
    memset(a->ev_tracks, 0, sizeof(a->ev_tracks));
}

/* The entity's model changed (skeleton_path reassigned): tear down every
 * piece of per-instance state that was resolved against the OLD model's
 * clips/skeleton/prims and rebuild the player against the new one. */
static void sr_anim_rebind_model(SrAnimInstance *a, JceModel *model)
{
    if (a->player) jce_anim_player_destroy(a->player);
    a->player      = NULL;
    a->model       = model;
    a->active_clip = -1;
    /* Clip set changed — force the blend tree to rebuild against the new
       model's clips. */
    if (a->blend_tree) {
        jce_anim_blend_tree_destroy(a->blend_tree);
        a->blend_tree = NULL;
    }
    a->bt_count = 0;
    /* Any in-flight SM crossfade state belongs to the OLD model's clips. */
    a->sm_trans_idx = -1;
    a->sm_seed_time = -1.0f;
    /* New model = new clip set: drop the event pool so the new skeleton's
       sidecar is re-loaded against the new clips. */
    sr_anim_events_reset(a);
    /* Bone mask was resolved against the OLD skeleton's joint indices. */
    if (a->avatar_mask) {
        jce_avatar_mask_unload(a->avatar_mask);
        a->avatar_mask = NULL;
        a->avatar_mask_path[0] = '\0';
    }
    for (int li = 0; li < SR_AVATAR_MAX_LAYERS; li++)
        if (a->layer_mask[li]) {
            jce_avatar_mask_unload(a->layer_mask[li]);
            a->layer_mask[li] = NULL;
            a->layer_mask_path[li][0] = '\0';
        }
    /* Retarget map borrows the OLD dst skeleton — rebuild against the new. */
    if (a->retarget_map) {
        jce_anim_retarget_map_destroy(a->retarget_map);
        a->retarget_map      = NULL;
        a->retarget_src_skel = NULL;
        a->retarget_dst_skel = NULL;
        a->retarget_src[0]   = '\0';
    }
    /* Morph VBs were sized + (node,prim)-keyed against the OLD model's prims
       — destroy them BEFORE the new model binds so they re-create lazily
       against the new geometry (handle-leak guard). */
    sr_free_morph_vbs(a);
    JceSkeleton *sk = jce_model_get_skeleton(model);
    if (sk && jce_model_anim_count(model) > 0)
        a->player = jce_anim_player_create(sk);
}

static SrAnimInstance *sr_get_anim_instance(JceSceneRenderer *sr,
                                            uint32_t entity, JceModel *model)
{
    int free_slot = -1;
    /* Hot path: O(1) index hit (the overwhelming steady-state case). */
    {
        SrAnimInstance *a = sr_find_anim_instance(sr, entity);
        if (a) {
            if (a->model != model)
                sr_anim_rebind_model(a, model);
            return a;
        }
    }
    /* Cold path (index miss => this entity has no slot).  The linear pass
     * still verifies that (correctness backstop for the index-full warn
     * path) while finding the first free slot — it runs once per entity
     * lifetime, not per frame. */
    for (int i = 0; i < SR_ANIM_INSTANCE_MAX; i++) {
        SrAnimInstance *a = &sr->anim_inst[i];
        if (a->used && a->entity == entity) {
            if (a->model != model)
                sr_anim_rebind_model(a, model);
            sr_anim_idx_put(sr, entity, i);   /* heal the missing entry */
            return a;
        }
        if (!a->used && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) {
        /* Once per renderer, not per entity per frame: a 1000-char crowd past
         * the cap would otherwise emit (N-cap) lines EVERY frame (~187k lines
         * over a 200-frame run) — real logging cost + drowns the log. */
        if (!sr->anim_cache_full_warned) {
            sr->anim_cache_full_warned = true;
            LOG_WARN(LOG_TAG, "anim instance cache full (%d) — additional "
                     "skinned entities render at bind pose (warned once)",
                     SR_ANIM_INSTANCE_MAX);
        }
        return NULL;
    }
    SrAnimInstance *a = &sr->anim_inst[free_slot];
    if (a->sm_binding) jce_anim_sm_binding_destroy(a->sm_binding); /* reclaimed slot */
    if (a->blend_tree) jce_anim_blend_tree_destroy(a->blend_tree);
    if (a->ev_pool)    JCE_FREE(a->ev_pool);                       /* reclaimed slot */
    if (a->avatar_mask) jce_avatar_mask_unload(a->avatar_mask);    /* reclaimed slot */
    for (int li = 0; li < SR_AVATAR_MAX_LAYERS; li++)             /* reclaimed slot */
        if (a->layer_mask[li]) jce_avatar_mask_unload(a->layer_mask[li]);
    if (a->retarget_map) jce_anim_retarget_map_destroy(a->retarget_map); /* reclaimed slot */
    /* Reclaimed slot may hold a previous entity's live morph VBs — destroy
       them BEFORE the memset (which would orphan the handles).  ONLY for a
       slot that was actually used: a FIRST-TIME slot is zero-initialized and
       handle idx==0 is a VALID bgfx handle (someone else's dynamic VB) —
       freeing it here destroyed foreign buffer 0 eight times per newly
       spawned animated entity, silently corrupting bgfx's handle table at
       RUNTIME (root of the exit-crash corruption chain). */
    if (a->used)
        sr_free_morph_vbs(a);                                      /* reclaimed slot */
    memset(a, 0, sizeof(*a));
    /* memset left morph_vb[*].idx == 0 (a VALID handle) — re-mark invalid. */
    sr_init_morph_vbs(a);
    a->entity      = entity;
    a->model       = model;
    a->active_clip = -1;
    a->ev_clip     = -1;
    a->speed       = 1.0f;
    a->paused      = true;
    a->used        = true;
    a->sm_trans_idx = -1;
    a->sm_seed_time = -1.0f;
    a->sm_prev_state = SR_SM_STATE_SEED;   /* first poll fires initial on_state_enter */
    JceSkeleton *sk = jce_model_get_skeleton(model);
    if (sk && jce_model_anim_count(model) > 0)
        a->player = jce_anim_player_create(sk);
    /* Register in the O(1) entity->slot index (a reclaimed slot's OLD key may
     * still point here; sr_find_anim_instance double-checks the slot record,
     * so the stale entry is harmless and gets overwritten on that entity's
     * next allocation). */
    sr_anim_idx_put(sr, entity, free_slot);
    return a;
}

/* ── 2D sprite animator (P1 #16) ───────────────────────────────────── */

/* Same-pattern O(1) entity -> slot index as the skeletal anim instances
 * above (identical lifecycle: slots release only wholesale at renderer
 * destroy, so live entries <= slot cap < capacity and probes terminate). */
#define SR_SPRITE_IDX_CAP  ((uint32_t)SR_SPRITE_ANIM_MAX * 2u)
#define SR_SPRITE_IDX_MASK (SR_SPRITE_IDX_CAP - 1u)

static void sr_sprite_idx_put(JceSceneRenderer *sr, uint32_t entity, int slot)
{
    uint32_t h = (entity * 2654435761u) & SR_SPRITE_IDX_MASK;
    for (uint32_t n = 0; n < SR_SPRITE_IDX_CAP; n++) {
        if (sr->sprite_idx[h] == 0 || sr->sprite_idx_keys[h] == entity) {
            sr->sprite_idx[h]      = (uint16_t)(slot + 1);
            sr->sprite_idx_keys[h] = entity;
            return;
        }
        h = (h + 1u) & SR_SPRITE_IDX_MASK;
    }
}

/* Find the per-entity sprite-animator slot (no creation).  O(1). */
int sr_find_sprite_anim(JceSceneRenderer *sr, uint32_t entity)
{
    uint32_t h = (entity * 2654435761u) & SR_SPRITE_IDX_MASK;
    for (uint32_t n = 0; n < SR_SPRITE_IDX_CAP; n++) {
        if (sr->sprite_idx[h] == 0) return -1;
        if (sr->sprite_idx_keys[h] == entity) {
            int slot = (int)sr->sprite_idx[h] - 1;
            SrSpriteAnim *s = &sr->sprite_anim[slot];
            /* Slot record is the source of truth (mirrors the anim index). */
            return (s->used && s->entity == entity) ? slot : -1;
        }
        h = (h + 1u) & SR_SPRITE_IDX_MASK;
    }
    return -1;
}

/* Build (or rebuild) the sprite sheet + player for `sa` on this slot. The
 * sheet comes from the JSON atlas when atlas_path is set, otherwise a uniform
 * grid sized from the texture (sheet_path is the image) and frame_w/frame_h. */
static void sr_sprite_anim_build(JceSceneRenderer *sr, int slot,
                                 const JceSpriteAnimatorComponent *sa)
{
    SrSpriteAnim *s = &sr->sprite_anim[slot];

    if (s->player) { jce_sprite_player_destroy(s->player); s->player = NULL; }
    if (s->sheet)  { jce_sprite_sheet_destroy(s->sheet);   s->sheet  = NULL; }

    if (sa->atlas_path[0]) {
        const char *image_path = sa->sheet_path[0] ? sa->sheet_path : NULL;
        if (sr->has_cbs && sr->cbs.resolve_path) {
            char resolved[1024];
            if (sr->cbs.resolve_path(sa->atlas_path, resolved,
                                     (int)sizeof(resolved), sr->cbs.userdata))
                s->sheet = jce_sprite_sheet_load_json(resolved, image_path);
        }
        if (!s->sheet && sr->pak)
            s->sheet = jce_sprite_sheet_load_json_pak(sr->pak,
                                                       sa->atlas_path,
                                                       image_path);
        if (!s->sheet)
            s->sheet = jce_sprite_sheet_load_json(sa->atlas_path, image_path);
    } else if (sa->sheet_path[0] && sa->frame_width > 0 && sa->frame_height > 0) {
        JceTexture tex = sr_resolve_texture(sr, sa->sheet_path);
        uint32_t iw = 0, ih = 0;
        if (jce_texture_valid(tex)) jce_texture_get_size(tex, &iw, &ih);
        if (iw > 0 && ih > 0) {
            s->sheet = jce_sprite_sheet_create_grid(
                sa->sheet_path, iw, ih,
                (uint32_t)sa->frame_width, (uint32_t)sa->frame_height, 100.0f);
        }
    }
    if (s->sheet) {
        s->player = jce_sprite_player_create(s->sheet);
        if (s->player && sa->current_anim[0])
            jce_sprite_player_set_anim(s->player, sa->current_anim);
    }

    snprintf(s->sheet_path, sizeof(s->sheet_path), "%s", sa->sheet_path);
    snprintf(s->atlas_path, sizeof(s->atlas_path), "%s", sa->atlas_path);
    snprintf(s->cur_anim,   sizeof(s->cur_anim),   "%s", sa->current_anim);
    s->frame_w = sa->frame_width;
    s->frame_h = sa->frame_height;
}

/* Advance every SpriteAnimator entity's frame time once per frame (mirrors the
 * skeletal "tick once" rule). The cached player is consumed in the entity draw
 * loop where the current frame's UV sub-rect is submitted to the sprite batch. */
void sr_update_sprite_anims(JceSceneRenderer *sr, JceScene *scene,
                                   EntityList *list, float dt_sec)
{
    /* O(1) empty-scene early-out (mirrors sr_update_skinned_anims): no
     * SpriteAnimator components + no live playback slots => the whole
     * per-entity probe walk below is provably a no-op. */
    if (jce_scene_count_sprite_animators(scene) == 0) {
        bool live = false;
        for (int li = 0; li < SR_SPRITE_ANIM_MAX; li++)
            if (sr->sprite_anim[li].used) { live = true; break; }
        if (!live) return;
    }

    for (int i = 0; i < list->count; i++) {
        JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;
        if (!jce_scene_has_sprite_animator(scene, e)) continue;
        if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SPRITE_ANIMATOR)) continue;

        JceSpriteAnimatorComponent *sa = jce_scene_get_sprite_animator(scene, e);
        if (!sa) continue;

        int slot = sr_find_sprite_anim(sr, (uint32_t)e);
        if (slot < 0) {
            for (int k = 0; k < SR_SPRITE_ANIM_MAX; k++) {
                if (!sr->sprite_anim[k].used) { slot = k; break; }
            }
            if (slot < 0) continue;   /* cache full */
            sr->sprite_anim[slot].used   = true;
            sr->sprite_anim[slot].entity = (uint32_t)e;
            sr->sprite_anim[slot].sheet  = NULL;
            sr->sprite_anim[slot].player = NULL;
            sr->sprite_anim[slot].sheet_path[0] = '\0';
            sr_sprite_idx_put(sr, (uint32_t)e, slot);
        }

        /* Rebuild on authoring change (sheet/atlas/frame size). */
        if (strcmp(sr->sprite_anim[slot].sheet_path, sa->sheet_path) != 0 ||
            strcmp(sr->sprite_anim[slot].atlas_path, sa->atlas_path) != 0 ||
            sr->sprite_anim[slot].frame_w != sa->frame_width ||
            sr->sprite_anim[slot].frame_h != sa->frame_height) {
            sr_sprite_anim_build(sr, slot, sa);
        }

        JceSpritePlayer *pl = sr->sprite_anim[slot].player;
        if (!pl) continue;

        /* Animation switch at runtime. */
        if (strcmp(sr->sprite_anim[slot].cur_anim, sa->current_anim) != 0) {
            if (sa->current_anim[0])
                jce_sprite_player_set_anim(pl, sa->current_anim);
            snprintf(sr->sprite_anim[slot].cur_anim,
                     sizeof(sr->sprite_anim[slot].cur_anim), "%s",
                     sa->current_anim);
        }

        if (sa->playing) {
            float sp = sa->speed > 0.0f ? sa->speed : 1.0f;
            jce_sprite_player_update(pl, dt_sec, sp);
        }
    }
}

/* ── PAK-first asset loaders (single-exe parity) ───────────────────────
 * The .anim_sm.json / .mask assets are pulled into the embedded PAK by the
 * bundle packer, but the deployed exe historically loaded them ONLY through
 * resolve_path→host, which returns nothing in a single-exe build (no loose
 * cooked tree).  Try sr->pak first (decompress the bytes and parse in memory),
 * exactly like sr_terrain.c / the HDR loader, then fall back to the host path
 * (editor / loose files).  Returns NULL only when neither source resolves. */
static JceAnimSmBinding *sr_anim_load_sm(JceSceneRenderer *sr, const char *sm_path)
{
    JceAnimSmBinding *b = NULL;
    if (sr->pak && sm_path && sm_path[0]) {
        const JcePakAsset *a = jce_pak_find(sr->pak, sm_path);
        if (a && a->original_size && a->original_size <= (1u << 20)) {
            char *buf = (char *)JCE_MALLOC((size_t)a->original_size);
            if (buf) {
                if (jce_pak_decompress(a, buf, (size_t)a->original_size) ==
                    (size_t)a->original_size)
                    b = jce_anim_sm_binding_create_mem(buf, (size_t)a->original_size);
                JCE_FREE(buf);
            }
        }
    }
    if (!b) {
        char        res[1024];
        const char *load = sm_path;
        if (sr->has_cbs && sr->cbs.resolve_path &&
            sr->cbs.resolve_path(sm_path, res, (int)sizeof(res), sr->cbs.userdata))
            load = res;
        b = jce_anim_sm_binding_create(load);
    }
    return b;
}

static JceAvatarMask *sr_anim_load_mask(JceSceneRenderer *sr, const char *mask_path,
                                        JceSkeleton *sk)
{
    JceAvatarMask *m = NULL;
    if (sr->pak && mask_path && mask_path[0]) {
        const JcePakAsset *a = jce_pak_find(sr->pak, mask_path);
        if (a && a->original_size && a->original_size <= (1u << 20)) {
            char *buf = (char *)JCE_MALLOC((size_t)a->original_size);
            if (buf) {
                if (jce_pak_decompress(a, buf, (size_t)a->original_size) ==
                    (size_t)a->original_size)
                    m = jce_avatar_mask_load_for_skeleton_mem(
                            buf, (size_t)a->original_size, sk);
                JCE_FREE(buf);
            }
        }
    }
    if (!m) {
        char        res[1024];
        const char *load = mask_path;
        if (sr->has_cbs && sr->cbs.resolve_path &&
            sr->cbs.resolve_path(mask_path, res, (int)sizeof(res), sr->cbs.userdata))
            load = res;
        m = jce_avatar_mask_load_for_skeleton(load, sk);
    }
    return m;
}

/* ── Avatar bone mask (FEATURE 3.3) ────────────────────────────────── */

/* Lazily (re)load the avatar's .mask asset for this instance, resolving bone
 * names against the model skeleton.  One-shot per distinct mask_path; cleared
 * when the path changes.  Returns the cached mask (NULL when none authored or
 * the asset failed to load). Scene-relative paths are resolved through the host
 * callback (same as terrain/HDR/SM) so editor previews open the right file. */
static JceAvatarMask *sr_anim_resolve_mask(JceSceneRenderer *sr,
                                           SrAnimInstance *ai,
                                           const char *mask_path,
                                           JceModel *model)
{
    if (!ai) return NULL;
    if (!mask_path || !mask_path[0]) {
        if (ai->avatar_mask) {
            jce_avatar_mask_unload(ai->avatar_mask);
            ai->avatar_mask = NULL;
            ai->avatar_mask_path[0] = '\0';
        }
        return NULL;
    }
    if (ai->avatar_mask && strcmp(ai->avatar_mask_path, mask_path) == 0)
        return ai->avatar_mask;   /* already loaded for this path */

    if (ai->avatar_mask) {
        jce_avatar_mask_unload(ai->avatar_mask);
        ai->avatar_mask = NULL;
    }
    snprintf(ai->avatar_mask_path, sizeof(ai->avatar_mask_path), "%s", mask_path);

    JceSkeleton *sk = model ? jce_model_get_skeleton(model) : NULL;
    ai->avatar_mask = sr_anim_load_mask(sr, mask_path, sk);
    if (ai->avatar_mask)
        LOG_INFO(LOG_TAG, "avatar mask: loaded '%s' (%u bones)",
                 mask_path, jce_avatar_mask_count(ai->avatar_mask));
    return ai->avatar_mask;
}

/* Resolve/cache the .mask for avatar layer slot `slot` (0..SR_AVATAR_MAX_LAYERS-1).
 * Same one-shot-per-path policy as sr_anim_resolve_mask, but stored in a
 * per-slot cache so an avatar can author several masked layers. Empty path →
 * NULL (the layer applies to all bones). */
static JceAvatarMask *sr_anim_resolve_layer_mask(JceSceneRenderer *sr,
                                                 SrAnimInstance *ai,
                                                 int slot,
                                                 const char *mask_path,
                                                 JceModel *model)
{
    if (!ai || slot < 0 || slot >= SR_AVATAR_MAX_LAYERS) return NULL;
    if (!mask_path || !mask_path[0]) {
        if (ai->layer_mask[slot]) {
            jce_avatar_mask_unload(ai->layer_mask[slot]);
            ai->layer_mask[slot] = NULL;
            ai->layer_mask_path[slot][0] = '\0';
        }
        return NULL;
    }
    if (ai->layer_mask[slot] &&
        strcmp(ai->layer_mask_path[slot], mask_path) == 0)
        return ai->layer_mask[slot];
    if (ai->layer_mask[slot]) {
        jce_avatar_mask_unload(ai->layer_mask[slot]);
        ai->layer_mask[slot] = NULL;
    }
    snprintf(ai->layer_mask_path[slot], sizeof(ai->layer_mask_path[slot]),
             "%s", mask_path);
    JceSkeleton *sk = model ? jce_model_get_skeleton(model) : NULL;
    ai->layer_mask[slot] = sr_anim_load_mask(sr, mask_path, sk);
    return ai->layer_mask[slot];
}

/* ── Frame events (P1 #16) ─────────────────────────────────────────── */

/* Per-call context handed to sr_anim_event_dispatch as the jce_anim_events_advance
 * `user`. Carries BOTH the firing entity (for logging / the hook payload) and the
 * owning renderer (so the static callback can reach sr->anim_event_fn). Lives on
 * the stack for the duration of the advance call — never escapes. */
typedef struct {
    JceSceneRenderer *sr;
    uint64_t          entity;
} SrAnimEventCtx;

/* Dispatch sink for animation frame events. This is the documented hook
 * point: by default it logs the fired event; a game routes id-based events to
 * script / audio (e.g. footstep -> play sfx, hitbox-on -> enable collider).
 * The in-engine LOG_DEBUG is always kept (side-effect-free, safe even before
 * any project authors events); when the renderer has a settable anim-event
 * sink installed (jce_scene_renderer_set_anim_event_fn — the runtime points it
 * at its entity→script dispatch), the event is ADDITIONALLY forwarded there. */
static void sr_anim_event_dispatch(const JceAnimEvent *ev, void *user)
{
    const SrAnimEventCtx *ctx = (const SrAnimEventCtx *)user;
    if (!ev || !ctx) return;
    uint64_t entity = ctx->entity;
    if (ev->name[0])
        LOG_DEBUG(LOG_TAG, "anim event: entity=%llu name=\"%s\" id=%u t=%.3f "
                  "f0=%.3f f1=%.3f i0=%d",
                  (unsigned long long)entity, ev->name, ev->id, ev->time,
                  ev->f0, ev->f1, ev->i0);
    else
        LOG_DEBUG(LOG_TAG, "anim event: entity=%llu id=%u t=%.3f f0=%.3f f1=%.3f i0=%d",
                  (unsigned long long)entity, ev->id, ev->time,
                  ev->f0, ev->f1, ev->i0);

    if (ctx->sr && ctx->sr->anim_event_fn)
        ctx->sr->anim_event_fn(entity, ev, ctx->sr->anim_event_user);
}

static int sr_anim_event_cmp(const void *a, const void *b)
{
    float ta = ((const JceAnimEvent *)a)->time;
    float tb = ((const JceAnimEvent *)b)->time;
    return (ta < tb) ? -1 : (ta > tb) ? 1 : 0;
}

/* Lazily load <skeleton_path>.anim.json once per instance. The sidecar maps
 * clip names to event arrays (every field but "time" is optional; "name" is
 * an optional string label authored by the editor's Animation Editor):
 *   { "Run": [ { "time": 0.25, "name": "footstep", "id": 1,
 *               "f0": 0, "f1": 0, "i0": 0 }, ... ] }
 * Each clip's events are stored contiguously in ev_pool and exposed as a
 * per-clip JceAnimEventTrack indexed by the model's clip index. Absent or
 * malformed sidecars leave zero tracks (events simply never fire). */
static void sr_anim_events_load(SrAnimInstance *ai, const char *skeleton_path,
                                JceModel *model)
{
    if (!ai || ai->ev_loaded) return;
    ai->ev_loaded = true;          /* one-shot: never re-attempt */
    if (!skeleton_path || !skeleton_path[0] || !model) return;

    char sidecar[300];
    snprintf(sidecar, sizeof(sidecar), "%s.anim.json", skeleton_path);
    JceJson *root = jce_json_parse_file(sidecar);
    if (!root) return;             /* no sidecar — common case, not an error */

    int clip_count = (int)jce_model_anim_count(model);
    if (clip_count > 16) clip_count = 16;

    /* Pass 1: count total events across the clips we know about. */
    int total = 0;
    for (int c = 0; c < clip_count; c++) {
        const char *cn = jce_anim_clip_name(jce_model_get_anim(model, (uint32_t)c));
        if (!cn) continue;
        JceJson *arr = jce_json_get(root, cn);
        if (jce_json_is_array(arr)) total += jce_json_array_size(arr);
    }
    if (total <= 0) { jce_json_free(root); return; }

    ai->ev_pool = (JceAnimEvent *)JCE_CALLOC((size_t)total, sizeof(JceAnimEvent));
    if (!ai->ev_pool) { jce_json_free(root); return; }

    /* Pass 2: fill per-clip tracks (contiguous slices of ev_pool). */
    int write = 0;
    for (int c = 0; c < clip_count; c++) {
        const JceAnimClip *clip = jce_model_get_anim(model, (uint32_t)c);
        const char *cn = jce_anim_clip_name(clip);
        JceAnimEventTrack *trk = &ai->ev_tracks[c];
        trk->events        = NULL;
        trk->count         = 0;
        trk->clip_duration = clip ? jce_anim_clip_duration(clip) : 0.0f;
        if (!cn) continue;
        JceJson *arr = jce_json_get(root, cn);
        if (!jce_json_is_array(arr)) continue;

        JceAnimEvent *first = &ai->ev_pool[write];
        int n = 0;
        for (JceJson *it = jce_json_first_child(arr); it && write < total;
             it = jce_json_next_sibling(it)) {
            JceAnimEvent *ev = &ai->ev_pool[write++];
            ev->time = (float)jce_json_get_number(it, "time", 0.0);
            ev->id   = (uint32_t)jce_json_get_int(it, "id", 0);
            ev->f0   = (float)jce_json_get_number(it, "f0", 0.0);
            ev->f1   = (float)jce_json_get_number(it, "f1", 0.0);
            ev->i0   = jce_json_get_int(it, "i0", 0);
            /* Optional string label; numeric-only files leave it empty
             * (pool is calloc'd, so ev->name is already ""). */
            const char *nm = jce_json_get_string(it, "name", NULL);
            if (nm && nm[0])
                snprintf(ev->name, sizeof(ev->name), "%s", nm);
            n++;
        }
        if (n > 0) {
            qsort(first, (size_t)n, sizeof(JceAnimEvent), sr_anim_event_cmp);
            trk->events = first;
            trk->count  = n;
        }
    }
    ai->ev_pool_count  = write;
    ai->ev_track_count = clip_count;
    jce_json_free(root);
    LOG_INFO(LOG_TAG, "anim events: loaded %d events for %s", write, sidecar);
}

/* Advance frame events for the clip the player is currently on, firing every
 * event in the (prev,cur] clip-time window (with loop wrap handled by
 * jce_anim_events_advance). Called after the pose has been advanced so the
 * player's time reflects this frame. */
static void sr_anim_events_advance(JceSceneRenderer *sr,
                                   SrAnimInstance *ai, int clip_index)
{
    if (!ai || !ai->player) return;
    float cur = jce_anim_player_get_time(ai->player);

    /* Clip switch (or first sample): reset baseline, fire nothing this frame. */
    if (clip_index != ai->ev_clip) {
        ai->ev_clip      = clip_index;
        ai->ev_prev_time = cur;
        return;
    }
    if (clip_index < 0 || clip_index >= ai->ev_track_count) {
        ai->ev_prev_time = cur;
        return;
    }
    const JceAnimEventTrack *trk = &ai->ev_tracks[clip_index];
    if (trk->count > 0) {
        /* Stack ctx threads BOTH the renderer (to reach the settable hook) and
           the firing entity to the static dispatch callback. Never escapes. */
        SrAnimEventCtx ctx = { sr, (uint64_t)ai->entity };
        jce_anim_events_advance(trk, ai->ev_prev_time, cur,
                                sr_anim_event_dispatch,
                                &ctx);
    }
    ai->ev_prev_time = cur;
}

/* Case-insensitive, basename-aware clip-name match (mirrors the SM binding):
 * robust to inconsistent casing / paths across different models. */
static int sr_lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static bool sr_clip_name_match(const char *a, const char *b)
{
    if (!a || !b) return false;
    const char *ba = a, *bb = b;
    for (const char *p = a; *p; ++p) if (*p == '/' || *p == '\\') ba = p + 1;
    for (const char *p = b; *p; ++p) if (*p == '/' || *p == '\\') bb = p + 1;
    for (; *ba && *bb; ++ba, ++bb)
        if (sr_lc((unsigned char)*ba) != sr_lc((unsigned char)*bb)) return false;
    return *ba == '\0' && *bb == '\0';
}

/* Resolve an SM state's authored clip name to the model's JceAnimClip
 * (case-insensitive basename match — the same rule the binding's clip
 * resolver uses).  NULL when the state has no clip or the model lacks it. */
static JceAnimClip *sr_sm_state_model_clip(const JceAnimSm *sm, int state,
                                           JceModel *model)
{
    const char *want = jce_anim_sm_state_clip(sm, state);
    if (!want || !want[0] || !model) return NULL;
    int an = (int)jce_model_anim_count(model);
    for (int i = 0; i < an; i++) {
        JceAnimClip *c = jce_model_get_anim(model, (uint32_t)i);
        if (sr_clip_name_match(jce_anim_clip_name(c), want)) return c;
    }
    return NULL;
}

/* Find a model clip by (tolerant) name match — used by the avatar layer stack
 * to resolve each authored layer's clip. NULL if absent. */
static JceAnimClip *sr_find_model_clip(JceModel *model, const char *want)
{
    if (!model || !want || !want[0]) return NULL;
    int an = (int)jce_model_anim_count(model);
    for (int i = 0; i < an; i++) {
        JceAnimClip *c = jce_model_get_anim(model, (uint32_t)i);
        if (sr_clip_name_match(jce_anim_clip_name(c), want)) return c;
    }
    return NULL;
}

/* Compute the entity's planar (XZ) movement speed from the frame-to-frame
 * Transform delta (works for physics, kinematic, and pure-animated entities),
 * EMA-smoothed so render-rate jitter doesn't make a downstream SM/blend flip.
 * The CALLER feeds it into the SM "Speed" param and/or the blend tree's
 * blend_param — this is the generic locomotion driver. */
static float sr_compute_entity_speed(SrAnimInstance *ai, const JceTransform *tc,
                                     float dt_sec)
{
    float raw = 0.0f;
    if (tc && ai->sm_have_prev && dt_sec > 0.0001f) {
        jce_vec3 d = jce_v3_sub(tc->position, ai->sm_prev_pos);
        d.y = 0.0f;                       /* planar locomotion speed */
        raw = jce_v3_len(d) / dt_sec;
    }
    if (tc) { ai->sm_prev_pos = tc->position; ai->sm_have_prev = true; }
    ai->sm_speed += (raw - ai->sm_speed) * 0.18f;   /* EMA smooth */
    if (ai->sm_speed < 0.0f) ai->sm_speed = 0.0f;
    return ai->sm_speed;
}

/* Index of the skeleton's root joint (first joint whose parent is -1, e.g. the
 * hips).  Returns 0 as a safe default when no explicit root is found — joint 0
 * is conventionally the root in glTF skeletons.  Used to drive root motion. */
static uint32_t sr_skeleton_root_joint(const JceSkeleton *skel)
{
    uint32_t nj = jce_skeleton_joint_count(skel);
    for (uint32_t j = 0; j < nj; j++)
        if (jce_skeleton_joint_parent(skel, j) < 0)
            return j;
    return 0;
}

/* Evaluate every skeletal-animator entity's pose ONCE per frame, BEFORE any
 * render pass.  The resulting world-bone palette is cached on the model
 * entry so the shadow pass (which the CPU records before the color pass) and
 * the color pass both consume the identical pose — animation time is advanced
 * exactly once, never per-pass.  This is the "skin once, draw many" rule and
 * is the only ordering consistent with shadow producers preceding the color
 * consumer view. */
/* Deferred pose-sample request produced by the serial management pass and
 * executed in parallel.  Each request targets a DISTINCT anim instance,
 * whose player owns its working buffers — so concurrent sampling reads the
 * shared skeleton/clips read-only and writes only its own palette. */
typedef struct {
    SrAnimInstance *ai;
    int             mode;   /* 1 = single-clip, 2 = blend-tree, 3 = masked single-clip,
                               4 = base clip + authored avatar layer stack */
    /* single */
    float              dt;
    int                ac;  /* active clip, for frame events */
    /* blend */
    const JceAnimClip *clip_a, *clip_b;
    float              ta, tb, wa, wb;
    /* layered (FEATURE 3.3): mode 3 — advance the player normally, then
       compose the active clip as a per-bone MASKED OVERRIDE onto the rest pose
       (masked-out bones fall back to rest). NULL mask leaves this path unused
       (mode stays 1), so default playback is byte-identical. */
    const JceAvatarMask *mask;
    const JceAnimClip   *layer_clip;
    /* mode 4: base = active clip @ player time; layers[] composited on top via
       jce_anim_player_blend_layers (additive or masked override, FEATURE 3.3
       authored on JceAvatarComponent.layers[]). layer_n authored layers. */
    JceAnimLayer  layers[SR_AVATAR_MAX_LAYERS];
    int           layer_n;
} SrAnimSample;

static void sr_anim_do_sample(SrAnimSample *r)
{
    SrAnimInstance *ai = r->ai;
    if (r->mode == 2)
        ai->skin_palette_count = jce_anim_player_blend(
            ai->player, r->clip_a, r->ta, r->wa, r->clip_b, r->tb, r->wb,
            ai->skin_palette, JCE_MAX_BONES);
    else if (r->mode == 3) {
        /* Advance time/state (and root motion, though the masked path is gated
           off when root motion is engaged) without keeping its palette, then
           re-evaluate as a single masked OVERRIDE layer over the rest pose so
           masked bones return to rest. Uses the REAL layered-blend path. */
        jce_anim_player_update(ai->player, r->dt, NULL, 0);
        float t = jce_anim_player_get_time(ai->player);
        JceAnimLayer layer;
        layer.clip     = r->layer_clip;
        layer.time     = t;
        layer.weight   = 1.0f;
        layer.mode     = JCE_ANIM_LAYER_OVERRIDE;
        layer.mask     = r->mask;
        layer.ref_clip = NULL;
        layer.ref_time = 0.0f;
        ai->skin_palette_count = jce_anim_player_blend_layers(
            ai->player, NULL, 0.0f, &layer, 1,
            ai->skin_palette, JCE_MAX_BONES);
    } else if (r->mode == 4) {
        /* Base = active clip @ the player's running time; the authored avatar
           layers (additive/override, optionally masked) are composited on top.
           Advance the player normally so events/root-time keep flowing, then
           re-evaluate with the layer stack (does not mutate playback state). */
        jce_anim_player_update(ai->player, r->dt, NULL, 0);
        float t = jce_anim_player_get_time(ai->player);
        /* base clip resolved by the caller (the active clip); NULL → rest pose */
        const JceAnimClip *base = r->layer_clip;
        /* Share the post-update player phase across the layers (set pre-update
           by the caller, refreshed here so base and layers stay in sync). */
        JceAnimLayer layers[SR_AVATAR_MAX_LAYERS];
        int ln = r->layer_n;
        if (ln > SR_AVATAR_MAX_LAYERS) ln = SR_AVATAR_MAX_LAYERS;
        for (int li = 0; li < ln; li++) {
            layers[li] = r->layers[li];
            layers[li].time = t;
        }
        ai->skin_palette_count = jce_anim_player_blend_layers(
            ai->player, base, t, layers, (uint32_t)ln,
            ai->skin_palette, JCE_MAX_BONES);
    } else if (r->mode == 1)
        ai->skin_palette_count = jce_anim_player_update(
            ai->player, r->dt, ai->skin_palette, JCE_MAX_BONES);
}

/* Worker: sample requests [begin,end) (per-instance disjoint writes). */
static void sr_anim_sample_range(uint32_t begin, uint32_t end, void *user)
{
    SrAnimSample *reqs = (SrAnimSample *)user;
    for (uint32_t i = begin; i < end; i++)
        sr_anim_do_sample(&reqs[i]);
}

/* ── Two-bone IK pass (consumes JceIkConstraintComponent) ───────────
 *
 * Runs SERIALLY at the end of sr_update_skinned_anims, after the parallel
 * pose sample wrote each instance's skin palette and before the palette is
 * consumed by the shadow/color passes. */

/* Shortest-arc rotation taking direction `from` onto direction `to`. */
static jce_quat sr_quat_from_to(jce_vec3 from, jce_vec3 to)
{
    jce_vec3 f = jce_v3_normalize(from);
    jce_vec3 t = jce_v3_normalize(to);
    float d = jce_v3_dot(f, t);
    if (d >= 1.0f - 1e-6f) return jce_q_identity();
    if (d <= -1.0f + 1e-6f) {
        /* Anti-parallel: rotate 180° about any axis orthogonal to f. */
        jce_vec3 axis = jce_v3_cross(f, jce_v3(1.0f, 0.0f, 0.0f));
        if (jce_v3_len(axis) < 1e-6f)
            axis = jce_v3_cross(f, jce_v3(0.0f, 1.0f, 0.0f));
        return jce_q_from_axis_angle(axis, JCE_PI);
    }
    jce_vec3 c = jce_v3_cross(f, t);
    return jce_q_normalize(jce_v4(c.x, c.y, c.z, 1.0f + d));
}

/* T(pivot) * R(q) * T(-pivot): rotate about a fixed point. */
static jce_mat4 sr_rotate_about_point(jce_vec3 pivot, jce_quat q)
{
    jce_mat4 r  = jce_q_to_mat4(q);
    jce_mat4 tn = jce_m4_translate(jce_v3_negate(pivot));
    jce_mat4 tp = jce_m4_translate(pivot);
    jce_mat4 m  = jce_m4_multiply(&r, &tn);
    return jce_m4_multiply(&tp, &m);
}

static jce_vec3 sr_m4_translation(const jce_mat4 *m)
{
    return jce_v3(m->raw[3][0], m->raw[3][1], m->raw[3][2]);
}

/* Transform a world-space point into the rigged entity's MODEL space.
 * `inv_model` is the inverse of the SAME matrix the skinned color draw
 * uses — the hierarchical, pivot-aware world matrix — because the palette
 * is consumed under that matrix. */
static jce_vec3 sr_world_to_model(const jce_mat4 *inv_model, jce_vec3 p)
{
    jce_vec4 r = jce_m4_mul_v4(inv_model, jce_v4(p.x, p.y, p.z, 1.0f));
    return jce_v3(r.x, r.y, r.z);
}

/* ── Animator sub-list ──────────────────────────────────────────────
 * With >=1 SkeletalAnimator in the scene, SIX passes (clip select, IK,
 * foot IK, full-body IK, morph weights, ragdoll) each walked the FULL
 * collect list probing has_skeletal_animator per entity — 6 x O(150k)
 * flecs probes per viewport per frame on a large world with one character.
 * sr_anim_build_selection runs once per sr_update_skinned_anims: the (tiny)
 * animator id set comes from component iteration, then a single list walk
 * records the indices carrying an animator, in list order.  Membership and
 * order semantics (focus-bounded streaming collect, event dispatch order,
 * instance slot allocation) are preserved exactly, and every pass keeps its
 * own has_/get checks so mid-frame component removal behaves as before.
 * On allocation failure s_asel_valid stays false and the passes fall back
 * to the original full walks. */
static uint32_t *s_asel_set;      /* open-addressed animator-id set */
static uint32_t  s_asel_set_cap;  /* power of two, 0 = unallocated */
static int      *s_asel_idx;      /* collect-list indices with an animator */
static int       s_asel_idx_cap;
static int       s_asel_count;
static bool      s_asel_valid;

static void sr_asel_collect_cb(JceScene *s, JceEntity e, void *ud)
{
    (void)s; (void)ud;
    uint32_t id = (uint32_t)e;
    if (id == 0) return;
    const uint32_t mask = s_asel_set_cap - 1u;
    uint32_t h = (id * 2654435761u) & mask;
    while (s_asel_set[h] && s_asel_set[h] != id) h = (h + 1u) & mask;
    s_asel_set[h] = id;
}

static void sr_anim_build_selection(JceScene *scene, const EntityList *list)
{
    s_asel_valid = false;
    s_asel_count = 0;
    if (!list) return;
    const int n = jce_scene_count_skeletal_animators(scene);
    if (n <= 0 || list->count <= 0) { s_asel_valid = true; return; }

    uint32_t need = (uint32_t)n * 2u;
    uint32_t cap = s_asel_set_cap ? s_asel_set_cap : 64u;
    while (cap < need) cap <<= 1;
    if (cap != s_asel_set_cap || !s_asel_set) {
        uint32_t *ns = (uint32_t *)JCE_REALLOC(s_asel_set,
                                               (size_t)cap * sizeof *ns);
        if (!ns) return;
        s_asel_set = ns; s_asel_set_cap = cap;
    }
    memset(s_asel_set, 0, (size_t)s_asel_set_cap * sizeof *s_asel_set);
    jce_scene_each_skeletal_animator(scene, sr_asel_collect_cb, NULL);

    if (s_asel_idx_cap < n) {
        int *ni = (int *)JCE_REALLOC(s_asel_idx, (size_t)n * sizeof *ni);
        if (!ni) return;
        s_asel_idx = ni; s_asel_idx_cap = n;
    }
    const uint32_t mask = s_asel_set_cap - 1u;
    for (int i = 0; i < list->count; i++) {
        uint32_t id = (uint32_t)list->entities[i];
        if (id == 0) continue;
        uint32_t h = (id * 2654435761u) & mask;
        while (s_asel_set[h]) {
            if (s_asel_set[h] == id) {
                if (s_asel_count >= s_asel_idx_cap) return; /* dup ids: bail */
                s_asel_idx[s_asel_count++] = i;
                break;
            }
            h = (h + 1u) & mask;
        }
    }
    s_asel_valid = true;
}

/* k-th selected list index; full walk when selection couldn't be built. */
static inline int sr_asel_n(const EntityList *list)
{ return s_asel_valid ? s_asel_count : list->count; }
static inline int sr_asel_i(int k)
{ return s_asel_valid ? s_asel_idx[k] : k; }

static void sr_apply_ik_constraints(JceSceneRenderer *sr, JceScene *scene,
                                    EntityList *list)
{
    const int an = sr_asel_n(list);
    for (int k = 0; k < an; k++) {
        const int i = sr_asel_i(k);
        JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;
        if (!jce_scene_has_skeletal_animator(scene, e)) continue;
        if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SKELETAL_ANIMATOR)) continue;
        if (!jce_scene_has_ik_constraints(scene, e)) continue;
        /* Honour the per-component disable (presence-gated: no flag bit). */
        { static int s_ikc_cid = -2;
          if (s_ikc_cid == -2) s_ikc_cid = jce_component_find("IkConstraints");
          if (s_ikc_cid >= 0 && !jce_scene_comp_enabled(scene, e, s_ikc_cid)) continue; }

        JceIkConstraintComponent *ik = jce_scene_get_ik_constraints(scene, e);
        if (!ik || ik->count <= 0) continue;

        JceSkeletalAnimatorComponent *sa = jce_scene_get_skeletal_animator(scene, e);
        if (!sa || !sa->skeleton_path[0]) continue;

        SrModelCache *mc = sr_get_model(sr, sa->skeleton_path, (uint32_t)e);
        if (!mc || !mc->model) continue;

        SrAnimInstance *ai = sr_find_anim_instance(sr, (uint32_t)e);
        if (!ai || ai->skin_palette_count == 0) continue;

        JceSkeleton *skel = jce_model_get_skeleton(mc->model);
        if (!skel) continue;

        if (!jce_scene_has_transform(scene, e)) continue;

        /* Same matrix the skinned color submit uses: the hierarchical,
         * pivot-aware world matrix.  A raw local TRS here desynced IK
         * targets for parented entities and for entities with an edited
         * pivot (the color pass honors both through the world matrix). */
        jce_mat4 model_mtx = jce_scene_get_world_matrix(scene, e);
        jce_mat4 inv_model = jce_m4_inverse(&model_mtx);

        uint32_t nj = jce_skeleton_joint_count(skel);
        if (nj > ai->skin_palette_count) nj = ai->skin_palette_count;
        if (nj > JCE_MAX_BONES)          nj = JCE_MAX_BONES;
        if (nj == 0) continue;

        int cn = ik->count;
        int cap = (int)(sizeof(ik->constraints) / sizeof(ik->constraints[0]));
        if (cn > cap) cn = cap;

        for (int ci = 0; ci < cn; ci++) {
            const JceIkConstraint *c = &ik->constraints[ci];
            /* Dispatch by authored kind (jce_scene.h JceIkConstraint):
             *   0 Aim         -> jce_anim_ik_aim_solve (single-bone aim)
             *   1 TwoBoneIK   -> jce_anim_ik_two_bone_solve (analytic 3-joint)
             *   2 MultiParent -> position + rotation toward the single target
             *   3 Position    -> jce_anim_ik_position_solve (root translation)
             *   4 Rotation    -> jce_anim_ik_rotation_solve (root rotation)
             *   5 CCD         -> jce_anim_ik_ccd_solve  (root->mid->end chain)
             *   6 FABRIK      -> jce_anim_ik_fabrik_solve(root->mid->end chain)
             * All seven kinds are now solved. */
            if (c->kind < 0 || c->kind > 6)
                continue;  /* unknown kind: round-trips but inert */
            if (!c->enabled || c->weight <= 0.0f) continue;
            if (c->target_entity == 0) continue;

            /* Single-target constraints (Position 3 / Rotation 4 / MultiParent
             * 2) drive only the root bone toward the target entity — they are
             * NOT chain-reach problems and need no mid/end joint. */
            bool single_target = (c->kind == 2 || c->kind == 3 || c->kind == 4);

            int jr = jce_skeleton_find_joint(skel, c->root_bone);
            int jm = jce_skeleton_find_joint(skel, c->mid_bone);
            int je = jce_skeleton_find_joint(skel, c->end_bone);
            /* Aim (kind 0) rotates a single bone: it needs the root bone plus
             * ONE child reference for the forward axis (prefer mid, else end).
             * Chain kinds (1/5/6) require the full root/mid/end triple. */
            int aim_child = (jm >= 0 && jm < (int)nj) ? jm
                          : ((je >= 0 && je < (int)nj) ? je : -1);
            bool ok;
            if (c->kind == 0 || single_target) {
                ok = (jr >= 0 && jr < (int)nj);
                if (c->kind == 0) ok = ok && (aim_child >= 0);
            } else {
                ok = (jr >= 0 && jm >= 0 && je >= 0 &&
                      jr < (int)nj && jm < (int)nj && je < (int)nj);
            }
            if (!ok) {
                if (!ai->ik_warned) {
                    LOG_WARN(LOG_TAG,
                             "IK constraint '%s' on entity %u: unresolved "
                             "bone(s) root='%s'(%d) mid='%s'(%d) end='%s'(%d) "
                             "— constraint skipped",
                             c->name, (uint32_t)e,
                             c->root_bone, jr, c->mid_bone, jm,
                             c->end_bone, je);
                    ai->ik_warned = true;
                }
                continue;
            }

            /* Reconstruct current model-space joint globals from the palette:
             * global[j] = palette[j] * inverse(inverse_bind[j]).  Recomputed
             * per constraint so stacked constraints see each other's result. */
            jce_mat4 globals[JCE_MAX_BONES];
            for (uint32_t j = 0; j < nj; j++) {
                jce_mat4 ib   = jce_skeleton_get_inverse_bind(skel, j);
                jce_mat4 bind = jce_m4_inverse(&ib);
                globals[j] = jce_m4_multiply(&ai->skin_palette[j], &bind);
            }

            jce_vec3 p0 = sr_m4_translation(&globals[jr]);   /* root */

            /* Target / pole world positions → entity model space. */
            jce_mat4 tw = jce_scene_get_world_matrix(scene,
                                                     (JceEntity)c->target_entity);
            jce_vec3 target = sr_world_to_model(&inv_model, sr_m4_translation(&tw));
            jce_vec3 pole;
            if (c->pole_entity != 0) {
                jce_mat4 pw = jce_scene_get_world_matrix(scene,
                                                         (JceEntity)c->pole_entity);
                pole = sr_world_to_model(&inv_model, sr_m4_translation(&pw));
            } else {
                pole = jce_v3_add(p0, jce_v3(c->pole_offset[0],
                                             c->pole_offset[1],
                                             c->pole_offset[2]));
            }

            float w_clamp = c->weight > 1.0f ? 1.0f : c->weight;

            /* ── Kind 0: AIM (single-bone) ──────────────────────────────
             * Rotate the root bone about its pivot so its forward axis
             * (root → child) points at the target, biased by the pole as up.
             * Reuses the chain write-back's descendant propagation. */
            if (c->kind == 0) {
                jce_vec3 child = sr_m4_translation(&globals[aim_child]);
                JceIkAimInput ain;
                ain.pivot[0]=p0.x; ain.pivot[1]=p0.y; ain.pivot[2]=p0.z;
                jce_vec3 fwd = jce_v3_sub(child, p0);
                ain.forward[0]=fwd.x; ain.forward[1]=fwd.y; ain.forward[2]=fwd.z;
                jce_vec3 up = jce_v3_sub(pole, p0);
                ain.up[0]=up.x; ain.up[1]=up.y; ain.up[2]=up.z;
                ain.target[0]=target.x; ain.target[1]=target.y; ain.target[2]=target.z;
                ain.weight = w_clamp;

                float aimed[3];
                if (!jce_anim_ik_aim_solve(&ain, aimed)) continue;

                jce_quat ra = sr_quat_from_to(fwd, jce_v3(aimed[0], aimed[1], aimed[2]));
                jce_mat4 rot_root = sr_rotate_about_point(p0, ra);

                jce_mat4 new_globals[JCE_MAX_BONES];
                bool     modified[JCE_MAX_BONES];
                memcpy(new_globals, globals, sizeof(jce_mat4) * nj);
                memset(modified, 0, sizeof(bool) * nj);

                new_globals[jr] = jce_m4_multiply(&rot_root, &globals[jr]);
                modified[jr] = true;
                for (uint32_t j = 0; j < nj; j++) {
                    if ((int)j == jr) continue;
                    int p = jce_skeleton_joint_parent(skel, j);
                    if (p < 0 || p >= (int)nj || !modified[p]) continue;
                    jce_mat4 inv_old_parent = jce_m4_inverse(&globals[p]);
                    jce_mat4 local = jce_m4_multiply(&inv_old_parent, &globals[j]);
                    new_globals[j] = jce_m4_multiply(&new_globals[p], &local);
                    modified[j] = true;
                }
                for (uint32_t j = 0; j < nj; j++) {
                    if (!modified[j]) continue;
                    jce_mat4 ib = jce_skeleton_get_inverse_bind(skel, j);
                    ai->skin_palette[j] = jce_m4_multiply(&new_globals[j], &ib);
                }
                continue;
            }

            /* ── Kinds 2/3/4: single-target parent constraints ───────────
             * Drive ONLY the root bone toward the target entity, then re-use
             * the EXACT same delta-on-globals write-back + forward descendant
             * propagation the Aim/chain kinds use (so children follow rigidly):
             *   3 Position : blend root model-space position toward the target
             *                position  -> a pure translation delta about model
             *                space (pre-multiply T(new_p - p0)).
             *   4 Rotation : blend root model-space rotation toward the target
             *                rotation  -> a delta rotation about the root pivot
             *                (sr_rotate_about_point, mirroring Aim).
             *   2 MultiParent : BOTH of the above toward the single target —
             *                a full parent constraint (the component carries one
             *                target_entity, so this is single-source by design).
             * The target transform is taken in the rigged entity's MODEL space
             * (inv_model * target_world), matching every other kind here. */
            if (single_target) {
                /* Target world transform → entity model space. */
                jce_mat4 t_model = jce_m4_multiply(&inv_model, &tw);

                /* Compose the root-bone delta in model space as
                 *   delta = T(new_p0) * R(rot_delta) * T(-p0)
                 * built from the blended position and/or the blended rotation
                 * (identity for the components a given kind does not drive). */
                jce_mat4 delta = jce_m4_identity();

                if (c->kind == 4 || c->kind == 2) {
                    /* Blend root rotation toward the target's rotation. */
                    jce_quat cur_q = jce_m4_to_quat(&globals[jr]);
                    jce_quat tgt_q = jce_m4_to_quat(&t_model);
                    float cur4[4] = { cur_q.x, cur_q.y, cur_q.z, cur_q.w };
                    float tgt4[4] = { tgt_q.x, tgt_q.y, tgt_q.z, tgt_q.w };
                    float out4[4];
                    jce_anim_ik_rotation_solve(cur4, tgt4, w_clamp, out4);
                    jce_quat blended = jce_v4(out4[0], out4[1], out4[2], out4[3]);
                    /* Delta rotation that takes cur_q onto blended:
                     *   r_delta = blended * inverse(cur_q).  Apply about pivot. */
                    jce_quat inv_cur = jce_v4(-cur_q.x, -cur_q.y, -cur_q.z, cur_q.w);
                    jce_quat r_delta = jce_q_multiply(blended, inv_cur);
                    delta = sr_rotate_about_point(p0, r_delta);
                }

                if (c->kind == 3 || c->kind == 2) {
                    /* Blend root position toward the target position. */
                    jce_vec3 tp = sr_m4_translation(&t_model);
                    float cur3[3] = { p0.x, p0.y, p0.z };
                    float tgt3[3] = { tp.x, tp.y, tp.z };
                    float out3[3];
                    jce_anim_ik_position_solve(cur3, tgt3, w_clamp, out3);
                    /* For Rotation+Position (MultiParent), the rotation delta
                     * already keeps p0 fixed (rotate-about-pivot), so the
                     * translation needed is simply (solved - p0). */
                    jce_vec3 tr = jce_v3(out3[0] - p0.x, out3[1] - p0.y,
                                         out3[2] - p0.z);
                    jce_mat4 trans = jce_m4_translate(tr);
                    /* Pre-multiply: translate AFTER any rotate-about-pivot. */
                    delta = jce_m4_multiply(&trans, &delta);
                }

                jce_mat4 new_globals[JCE_MAX_BONES];
                bool     modified[JCE_MAX_BONES];
                memcpy(new_globals, globals, sizeof(jce_mat4) * nj);
                memset(modified, 0, sizeof(bool) * nj);

                new_globals[jr] = jce_m4_multiply(&delta, &globals[jr]);
                modified[jr] = true;
                /* Identical forward descendant propagation as the Aim kind:
                 * parents precede children, so each child global is rebuilt
                 * from its parent's new global × the preserved local offset. */
                for (uint32_t j = 0; j < nj; j++) {
                    if ((int)j == jr) continue;
                    int p = jce_skeleton_joint_parent(skel, j);
                    if (p < 0 || p >= (int)nj || !modified[p]) continue;
                    jce_mat4 inv_old_parent = jce_m4_inverse(&globals[p]);
                    jce_mat4 local = jce_m4_multiply(&inv_old_parent, &globals[j]);
                    new_globals[j] = jce_m4_multiply(&new_globals[p], &local);
                    modified[j] = true;
                }
                for (uint32_t j = 0; j < nj; j++) {
                    if (!modified[j]) continue;
                    jce_mat4 ib = jce_skeleton_get_inverse_bind(skel, j);
                    ai->skin_palette[j] = jce_m4_multiply(&new_globals[j], &ib);
                }
                continue;
            }

            jce_vec3 p1 = sr_m4_translation(&globals[jm]);   /* mid  */
            jce_vec3 p2 = sr_m4_translation(&globals[je]);   /* end  */

            /* Compute solved mid (q1) and end (q2) positions per solver kind.
             * TwoBoneIK is analytic; CCD/FABRIK iterate over the 3-joint
             * root→mid→end chain. All three share the rotation write-back. */
            jce_vec3 q1, q2;
            if (c->kind == 1) {
                /* The solver lerps toward the IK pose by `weight` internally —
                 * pass it through and do NOT blend again on write-back. */
                JceIkTwoBoneInput in;
                in.root_pos[0] = p0.x; in.root_pos[1] = p0.y; in.root_pos[2] = p0.z;
                in.mid_pos[0]  = p1.x; in.mid_pos[1]  = p1.y; in.mid_pos[2]  = p1.z;
                in.end_pos[0]  = p2.x; in.end_pos[1]  = p2.y; in.end_pos[2]  = p2.z;
                in.target[0] = target.x; in.target[1] = target.y; in.target[2] = target.z;
                in.pole[0]   = pole.x;   in.pole[1]   = pole.y;   in.pole[2]   = pole.z;
                in.weight    = w_clamp;

                JceIkTwoBoneOutput out;
                jce_anim_ik_two_bone_solve(&in, &out);
                q1 = jce_v3(out.mid_pos[0], out.mid_pos[1], out.mid_pos[2]);
                q2 = jce_v3(out.end_pos[0], out.end_pos[1], out.end_pos[2]);
            } else {
                /* kind 5 (CCD) / kind 6 (FABRIK): 3-joint chain in place. */
                float chain[3 * 3] = {
                    p0.x, p0.y, p0.z,
                    p1.x, p1.y, p1.z,
                    p2.x, p2.y, p2.z,
                };
                float tgt3[3] = { target.x, target.y, target.z };
                if (c->kind == 5)
                    jce_anim_ik_ccd_solve(chain, 3, tgt3, 16, 1e-4f);
                else
                    jce_anim_ik_fabrik_solve(chain, 3, tgt3, 16, 1e-4f);
                /* Blend toward the solved chain by weight (root is fixed). */
                jce_vec3 s1 = jce_v3(chain[3], chain[4], chain[5]);
                jce_vec3 s2 = jce_v3(chain[6], chain[7], chain[8]);
                q1 = jce_v3_lerp(p1, s1, w_clamp);
                q2 = jce_v3_lerp(p2, s2, w_clamp);
            }

            /* Write-back as delta rotations on the joint GLOBALS:
             *  - root: shortest arc from old (mid-root) dir to new (mid'-root),
             *    rotating about the root position;
             *  - mid:  after the root rotation carried the old end to
             *    R0*(p2-p1), shortest arc from that onto (end'-mid'),
             *    rotating about the new mid position. */
            jce_quat r0 = sr_quat_from_to(jce_v3_sub(p1, p0),
                                          jce_v3_sub(q1, p0));
            jce_vec3 end_dir_rot = jce_q_rotate(r0, jce_v3_sub(p2, p1));
            jce_quat r1 = sr_quat_from_to(end_dir_rot, jce_v3_sub(q2, q1));

            jce_mat4 rot_root = sr_rotate_about_point(p0, r0);
            jce_mat4 rot_mid  = sr_rotate_about_point(q1, r1);

            jce_mat4 new_globals[JCE_MAX_BONES];
            bool     modified[JCE_MAX_BONES];
            memcpy(new_globals, globals, sizeof(jce_mat4) * nj);
            memset(modified, 0, sizeof(bool) * nj);

            new_globals[jr] = jce_m4_multiply(&rot_root, &globals[jr]);
            {
                jce_mat4 tmp = jce_m4_multiply(&rot_root, &globals[jm]);
                new_globals[jm] = jce_m4_multiply(&rot_mid, &tmp);
            }
            modified[jm] = true;

            /* Propagate to ALL descendants of mid: joints are ordered with
             * parents preceding children, so one forward pass recomputes each
             * child global from its parent's new global while preserving the
             * old parent-relative offset.  (root and mid keep their explicit
             * new globals computed above.) */
            for (uint32_t j = 0; j < nj; j++) {
                if ((int)j == jr || (int)j == jm) continue;
                int p = jce_skeleton_joint_parent(skel, j);
                if (p < 0 || p >= (int)nj || !modified[p]) continue;
                jce_mat4 inv_old_parent = jce_m4_inverse(&globals[p]);
                jce_mat4 local = jce_m4_multiply(&inv_old_parent, &globals[j]);
                new_globals[j] = jce_m4_multiply(&new_globals[p], &local);
                modified[j] = true;
            }

            /* Re-derive the skin palette for the changed joints:
             * palette[j] = global'[j] * inverse_bind[j]. */
            for (uint32_t j = 0; j < nj; j++) {
                if ((int)j != jr && !modified[j]) continue;
                jce_mat4 ib = jce_skeleton_get_inverse_bind(skel, j);
                ai->skin_palette[j] = jce_m4_multiply(&new_globals[j], &ib);
            }
        }
    }
}

/* ── Foot IK pass (consumes JceFootIkComponent) ──────────────────────────
 *
 * Sibling of sr_apply_ik_constraints, run right after it (Pass 4b).  For each
 * skeletal-animator entity carrying an enabled JceFootIkComponent it:
 *   1. resolves pelvis/hip/knee/ankle joint indices by name,
 *   2. reconstructs each joint's current model-space global from the sampled
 *      palette (global[j] = palette[j] * inverse(inverse_bind[j])) — exactly
 *      as the constraint pass does,
 *   3. raycasts DOWN under each foot through the renderer's ground-query hook
 *      (origin = ankle + up*cast_up in WORLD space; dir = (0,-1,0); max =
 *      cast_up + cast_down),
 *   4. calls jce_anim_foot_ik_solve in MODEL space (foot world hit converted
 *      to a model-Y target), and
 *   5. writes the pelvis drop + the solved per-leg knee/ankle back into the
 *      palette using the SAME delta-rotation write-back the constraint pass
 *      uses (sr_quat_from_to + sr_rotate_about_point + forward descendant
 *      propagation).
 *
 * GATING / ZERO-REGRESSION: when the ground-query hook is NULL (the default in
 * tools / before Play) NO ray is cast, so every leg is "ungrounded" and the
 * solver passes the pose through unchanged — the pass is a complete no-op.
 * Likewise for entities without an enabled FootIk component, or with blend 0.
 * The model-space up axis is assumed to align with world up (upright bipeds),
 * matching every other vertical-placement assumption in the renderer. */
static void sr_apply_foot_ik(JceSceneRenderer *sr, JceScene *scene,
                             EntityList *list)
{
    /* No ground source -> nothing this pass can do; keep the pose identical. */
    if (!sr->ground_query_fn) return;

    const int an = sr_asel_n(list);
    for (int k = 0; k < an; k++) {
        const int i = sr_asel_i(k);
        JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;
        if (!jce_scene_has_skeletal_animator(scene, e)) continue;
        if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SKELETAL_ANIMATOR)) continue;
        if (!jce_scene_has_foot_ik(scene, e)) continue;
        /* Honour the per-component disable (presence-gated: no flag bit). */
        { static int s_fik_cid = -2;
          if (s_fik_cid == -2) s_fik_cid = jce_component_find("FootIk");
          if (s_fik_cid >= 0 && !jce_scene_comp_enabled(scene, e, s_fik_cid)) continue; }

        JceFootIkComponent *fk = jce_scene_get_foot_ik(scene, e);
        if (!fk || !fk->enabled || fk->blend <= 0.0f) continue;

        JceSkeletalAnimatorComponent *sa = jce_scene_get_skeletal_animator(scene, e);
        if (!sa || !sa->skeleton_path[0]) continue;

        SrModelCache *mc = sr_get_model(sr, sa->skeleton_path, (uint32_t)e);
        if (!mc || !mc->model) continue;

        SrAnimInstance *ai = sr_find_anim_instance(sr, (uint32_t)e);
        if (!ai || ai->skin_palette_count == 0) continue;

        JceSkeleton *skel = jce_model_get_skeleton(mc->model);
        if (!skel) continue;

        if (!jce_scene_has_transform(scene, e)) continue;

        /* Same hierarchical, pivot-aware world matrix the color/IK pass uses. */
        jce_mat4 model_mtx = jce_scene_get_world_matrix(scene, e);
        jce_mat4 inv_model = jce_m4_inverse(&model_mtx);

        uint32_t nj = jce_skeleton_joint_count(skel);
        if (nj > ai->skin_palette_count) nj = ai->skin_palette_count;
        if (nj > JCE_MAX_BONES)          nj = JCE_MAX_BONES;
        if (nj == 0) continue;

        /* Reconstruct current model-space joint globals from the palette. */
        jce_mat4 globals[JCE_MAX_BONES];
        for (uint32_t j = 0; j < nj; j++) {
            jce_mat4 ib   = jce_skeleton_get_inverse_bind(skel, j);
            jce_mat4 bind = jce_m4_inverse(&ib);
            globals[j] = jce_m4_multiply(&ai->skin_palette[j], &bind);
        }

        int j_pelvis = (fk->pelvis_bone[0])
                       ? jce_skeleton_find_joint(skel, fk->pelvis_bone) : -1;

        /* Build the solver input in MODEL space; ground via the hook in WORLD
         * space.  Track each leg's joint indices for the write-back. */
        JceFootIkInput in;
        memset(&in, 0, sizeof(in));
        in.leg_count       = JCE_FOOT_IK_MAX_LEGS;
        in.max_step_height = fk->max_step_height;
        in.foot_offset     = fk->foot_offset;
        in.blend           = fk->blend < 1.0f ? fk->blend : 1.0f;
        if (j_pelvis >= 0 && j_pelvis < (int)nj) {
            jce_vec3 pp = sr_m4_translation(&globals[j_pelvis]);
            in.pelvis[0] = pp.x; in.pelvis[1] = pp.y; in.pelvis[2] = pp.z;
        }

        int leg_hip[JCE_FOOT_IK_MAX_LEGS];
        int leg_knee[JCE_FOOT_IK_MAX_LEGS];
        int leg_ankle[JCE_FOOT_IK_MAX_LEGS];
        int any_grounded = 0;

        for (int lgi = 0; lgi < JCE_FOOT_IK_MAX_LEGS; lgi++) {
            leg_hip[lgi] = leg_knee[lgi] = leg_ankle[lgi] = -1;
            JceFootIkLeg *lg = &in.legs[lgi];
            lg->grounded = false;

            /* Empty ankle name disables this leg. */
            if (!fk->ankle_bone[lgi][0]) continue;
            int jh = fk->hip_bone[lgi][0]
                     ? jce_skeleton_find_joint(skel, fk->hip_bone[lgi]) : -1;
            int jk = fk->knee_bone[lgi][0]
                     ? jce_skeleton_find_joint(skel, fk->knee_bone[lgi]) : -1;
            int ja = jce_skeleton_find_joint(skel, fk->ankle_bone[lgi]);
            if (jh < 0 || jk < 0 || ja < 0 ||
                jh >= (int)nj || jk >= (int)nj || ja >= (int)nj) {
                if (!ai->ik_warned) {
                    LOG_WARN(LOG_TAG,
                             "Foot IK on entity %u: unresolved leg %d bone(s) "
                             "hip='%s'(%d) knee='%s'(%d) ankle='%s'(%d) — leg skipped",
                             (uint32_t)e, lgi,
                             fk->hip_bone[lgi], jh, fk->knee_bone[lgi], jk,
                             fk->ankle_bone[lgi], ja);
                    ai->ik_warned = true;
                }
                continue;
            }

            jce_vec3 hip   = sr_m4_translation(&globals[jh]);
            jce_vec3 knee  = sr_m4_translation(&globals[jk]);
            jce_vec3 ankle = sr_m4_translation(&globals[ja]);

            lg->hip[0]=hip.x;   lg->hip[1]=hip.y;   lg->hip[2]=hip.z;
            lg->knee[0]=knee.x; lg->knee[1]=knee.y; lg->knee[2]=knee.z;
            lg->ankle[0]=ankle.x; lg->ankle[1]=ankle.y; lg->ankle[2]=ankle.z;

            /* Ground query in WORLD space: origin above the foot, ray down. */
            jce_vec4 aw4 = jce_m4_mul_v4(&model_mtx,
                                         jce_v4(ankle.x, ankle.y, ankle.z, 1.0f));
            float origin[3] = { aw4.x, aw4.y + fk->cast_up, aw4.z };
            float dir[3]    = { 0.0f, -1.0f, 0.0f };
            float max_dist  = fk->cast_up + fk->cast_down;
            float hit_y = 0.0f, normal_w[3] = { 0.0f, 1.0f, 0.0f };
            if (!sr->ground_query_fn((uint64_t)e, origin, dir, max_dist,
                                     &hit_y, normal_w, sr->ground_query_user))
                continue;   /* miss -> leg stays ungrounded (pass-through) */

            /* Convert the world hit Y into MODEL space at the foot's XZ so it
             * lines up with the model-space ankle the solver bends toward. */
            jce_vec4 hm4 = jce_m4_mul_v4(&inv_model,
                                         jce_v4(aw4.x, hit_y, aw4.z, 1.0f));
            lg->ground_y = hm4.y;
            /* Ground normal -> model space (rotate as a direction). */
            jce_vec4 nm4 = jce_m4_mul_v4(&inv_model,
                              jce_v4(normal_w[0], normal_w[1], normal_w[2], 0.0f));
            jce_vec3 nmod = jce_v3_normalize(jce_v3(nm4.x, nm4.y, nm4.z));
            lg->ground_normal[0]=nmod.x; lg->ground_normal[1]=nmod.y; lg->ground_normal[2]=nmod.z;
            lg->grounded = true;

            leg_hip[lgi]=jh; leg_knee[lgi]=jk; leg_ankle[lgi]=ja;
            any_grounded = 1;
        }

        if (!any_grounded) continue;

        JceFootIkOutput out;
        jce_anim_foot_ik_solve(&in, &out);

        /* ── Pelvis drop: translate the pelvis joint global + ALL descendants
         * by pelvis_offset_y (model-Y), so hips/knees/ankles follow before the
         * per-leg reach.  Done as a global translate + forward propagation. */
        if (j_pelvis >= 0 && j_pelvis < (int)nj &&
            (out.pelvis_offset_y < -1e-6f || out.pelvis_offset_y > 1e-6f)) {
            jce_mat4 shift = jce_m4_translate(jce_v3(0.0f, out.pelvis_offset_y, 0.0f));
            globals[j_pelvis] = jce_m4_multiply(&shift, &globals[j_pelvis]);
            for (uint32_t j = 0; j < nj; j++) {
                if ((int)j == j_pelvis) continue;
                /* Is j a (transitive) descendant of the pelvis? Walk parents. */
                int p = jce_skeleton_joint_parent(skel, j);
                bool under = false;
                int guard = 0;
                while (p >= 0 && guard++ < (int)nj) {
                    if (p == j_pelvis) { under = true; break; }
                    p = jce_skeleton_joint_parent(skel, p);
                }
                if (under)
                    globals[j] = jce_m4_multiply(&shift, &globals[j]);
            }
            /* Re-derive palette for everything we just shifted. */
            for (uint32_t j = 0; j < nj; j++) {
                jce_mat4 ib = jce_skeleton_get_inverse_bind(skel, j);
                ai->skin_palette[j] = jce_m4_multiply(&globals[j], &ib);
            }
        }

        /* ── Per-leg two-bone write-back (mirror sr_apply_ik_constraints). ── */
        for (int lgi = 0; lgi < JCE_FOOT_IK_MAX_LEGS; lgi++) {
            if (!out.legs[lgi].solved) continue;
            int jr = leg_hip[lgi], jm = leg_knee[lgi], je = leg_ankle[lgi];
            if (jr < 0 || jm < 0 || je < 0) continue;

            /* Current (post-pelvis-shift) globals. */
            jce_vec3 p0 = sr_m4_translation(&globals[jr]);
            jce_vec3 p1 = sr_m4_translation(&globals[jm]);
            jce_vec3 p2 = sr_m4_translation(&globals[je]);
            /* Solved knee/ankle from the solver (model space). */
            jce_vec3 q1 = jce_v3(out.legs[lgi].knee[0],
                                 out.legs[lgi].knee[1],
                                 out.legs[lgi].knee[2]);
            jce_vec3 q2 = jce_v3(out.legs[lgi].ankle[0],
                                 out.legs[lgi].ankle[1],
                                 out.legs[lgi].ankle[2]);

            jce_quat r0 = sr_quat_from_to(jce_v3_sub(p1, p0),
                                          jce_v3_sub(q1, p0));
            jce_vec3 end_dir_rot = jce_q_rotate(r0, jce_v3_sub(p2, p1));
            jce_quat r1 = sr_quat_from_to(end_dir_rot, jce_v3_sub(q2, q1));

            jce_mat4 rot_root = sr_rotate_about_point(p0, r0);
            jce_mat4 rot_mid  = sr_rotate_about_point(q1, r1);

            jce_mat4 new_globals[JCE_MAX_BONES];
            bool     modified[JCE_MAX_BONES];
            memcpy(new_globals, globals, sizeof(jce_mat4) * nj);
            memset(modified, 0, sizeof(bool) * nj);

            new_globals[jr] = jce_m4_multiply(&rot_root, &globals[jr]);
            {
                jce_mat4 tmp = jce_m4_multiply(&rot_root, &globals[jm]);
                new_globals[jm] = jce_m4_multiply(&rot_mid, &tmp);
            }
            modified[jr] = true;
            modified[jm] = true;

            for (uint32_t j = 0; j < nj; j++) {
                if ((int)j == jr || (int)j == jm) continue;
                int p = jce_skeleton_joint_parent(skel, j);
                if (p < 0 || p >= (int)nj || !modified[p]) continue;
                jce_mat4 inv_old_parent = jce_m4_inverse(&globals[p]);
                jce_mat4 local = jce_m4_multiply(&inv_old_parent, &globals[j]);
                new_globals[j] = jce_m4_multiply(&new_globals[p], &local);
                modified[j] = true;
            }

            /* rotate_to_normal: aim the foot (ankle) bone's forward at the
             * ground normal so the sole follows the slope.  Optional; only the
             * ankle global is nudged, descendants (toes) follow via the same
             * propagation rule next.  We apply it on the ankle's new global. */
            if (fk->rotate_to_normal) {
                jce_vec3 nrm = jce_v3(in.legs[lgi].ground_normal[0],
                                      in.legs[lgi].ground_normal[1],
                                      in.legs[lgi].ground_normal[2]);
                if (jce_v3_len(nrm) > 1e-4f) {
                    jce_vec3 ankle_pos = sr_m4_translation(&new_globals[je]);
                    /* Foot "up" today is the leg direction (ankle - knee). */
                    jce_vec3 cur_up = jce_v3_sub(ankle_pos,
                                                 sr_m4_translation(&new_globals[jm]));
                    JceIkAimInput ain;
                    ain.pivot[0]=ankle_pos.x; ain.pivot[1]=ankle_pos.y; ain.pivot[2]=ankle_pos.z;
                    ain.forward[0]=cur_up.x; ain.forward[1]=cur_up.y; ain.forward[2]=cur_up.z;
                    ain.up[0]=0.0f; ain.up[1]=0.0f; ain.up[2]=1.0f;
                    /* Aim the leg "up" toward the +normal direction. */
                    ain.target[0]=ankle_pos.x + nrm.x;
                    ain.target[1]=ankle_pos.y + nrm.y;
                    ain.target[2]=ankle_pos.z + nrm.z;
                    ain.weight = in.blend;
                    float aimed[3];
                    if (jce_anim_ik_aim_solve(&ain, aimed)) {
                        jce_quat ra = sr_quat_from_to(cur_up,
                                          jce_v3(aimed[0], aimed[1], aimed[2]));
                        jce_mat4 rot_foot = sr_rotate_about_point(ankle_pos, ra);
                        new_globals[je] = jce_m4_multiply(&rot_foot, &new_globals[je]);
                        /* Re-propagate to foot descendants (toes). */
                        for (uint32_t j = 0; j < nj; j++) {
                            int p = jce_skeleton_joint_parent(skel, j);
                            if (p == je && modified[j]) {
                                jce_mat4 inv_old = jce_m4_inverse(&globals[p]);
                                jce_mat4 local = jce_m4_multiply(&inv_old, &globals[j]);
                                new_globals[j] = jce_m4_multiply(&new_globals[p], &local);
                            }
                        }
                    }
                }
            }

            /* Re-derive palette for changed joints, and commit globals so a
             * following leg sees this one's result. */
            for (uint32_t j = 0; j < nj; j++) {
                if ((int)j != jr && !modified[j]) continue;
                jce_mat4 ib = jce_skeleton_get_inverse_bind(skel, j);
                ai->skin_palette[j] = jce_m4_multiply(&new_globals[j], &ib);
                globals[j] = new_globals[j];
            }
        }
    }
}

/* ── Full-Body IK pass (consumes JceFullBodyIkComponent, Pass 4d) ─────────
 *
 * Sibling of sr_apply_foot_ik.  For each skeletal-animator entity carrying an
 * enabled JceFullBodyIkComponent it reconstructs the current model-space joint
 * globals from the sampled palette, builds an FBBIK body from the whole rig,
 * pulls every effector's named bone toward its WORLD target (converted to model
 * space), runs the coupled FABRIK-tree solve, blends the solved positions toward
 * the animated pose by `blend`, and writes the result back via the tested pure
 * jce_anim_fbbik_write_back (joints at solved positions + bones re-oriented),
 * re-deriving the palette.  No effectors / no resolvable bones -> pose
 * unchanged. */
static void sr_apply_full_body_ik(JceSceneRenderer *sr, JceScene *scene,
                                  EntityList *list)
{
    const int an = sr_asel_n(list);
    for (int k = 0; k < an; k++) {
        const int i = sr_asel_i(k);
        JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;
        if (!jce_scene_has_skeletal_animator(scene, e)) continue;
        if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SKELETAL_ANIMATOR)) continue;
        if (!jce_scene_has_full_body_ik(scene, e)) continue;
        /* Honour the per-component disable (presence-gated: no flag bit). */
        { static int s_fbik_cid = -2;
          if (s_fbik_cid == -2) s_fbik_cid = jce_component_find("FullBodyIk");
          if (s_fbik_cid >= 0 && !jce_scene_comp_enabled(scene, e, s_fbik_cid)) continue; }

        JceFullBodyIkComponent *fb = jce_scene_get_full_body_ik(scene, e);
        if (!fb || !fb->enabled || fb->blend <= 0.0f || fb->effector_count <= 0) continue;

        JceSkeletalAnimatorComponent *sa = jce_scene_get_skeletal_animator(scene, e);
        if (!sa || !sa->skeleton_path[0]) continue;
        SrModelCache *mc = sr_get_model(sr, sa->skeleton_path, (uint32_t)e);
        if (!mc || !mc->model) continue;
        SrAnimInstance *ai = sr_find_anim_instance(sr, (uint32_t)e);
        if (!ai || ai->skin_palette_count == 0) continue;
        JceSkeleton *skel = jce_model_get_skeleton(mc->model);
        if (!skel || !jce_scene_has_transform(scene, e)) continue;

        jce_mat4 model_mtx = jce_scene_get_world_matrix(scene, e);
        jce_mat4 inv_model = jce_m4_inverse(&model_mtx);

        uint32_t nj = jce_skeleton_joint_count(skel);
        if (nj > ai->skin_palette_count) nj = ai->skin_palette_count;
        if (nj > JCE_MAX_BONES)           nj = JCE_MAX_BONES;
        if (nj == 0 || nj > JCE_FBBIK_MAX_NODES) continue;

        /* Reconstruct model-space joint globals from the palette (as foot IK). */
        jce_mat4 globals[JCE_MAX_BONES];
        for (uint32_t j = 0; j < nj; j++) {
            jce_mat4 ib   = jce_skeleton_get_inverse_bind(skel, j);
            jce_mat4 bind = jce_m4_inverse(&ib);
            globals[j] = jce_m4_multiply(&ai->skin_palette[j], &bind);
        }

        /* Build the FBBIK body (one node per joint) from the current globals. */
        JceFbbikBody body;
        body.node_count = (int)nj;
        for (uint32_t j = 0; j < nj; j++) {
            body.positions[j] = sr_m4_translation(&globals[j]);
            body.parents[j]   = jce_skeleton_joint_parent(skel, j);
        }
        jce_anim_fbbik_compute_lengths(&body);

        /* Resolve effectors: named bone -> joint, world target -> model space. */
        JceFbbikEffector effs[JCE_FBBIK_MAX_EFFECTORS];
        int neff = 0;
        int ec = fb->effector_count;
        if (ec > (int)(sizeof(fb->effectors) / sizeof(fb->effectors[0])))
            ec = (int)(sizeof(fb->effectors) / sizeof(fb->effectors[0]));
        for (int k = 0; k < ec && neff < JCE_FBBIK_MAX_EFFECTORS; k++) {
            if (!fb->effectors[k].bone[0]) continue;
            int jx = jce_skeleton_find_joint(skel, fb->effectors[k].bone);
            if (jx < 0 || jx >= (int)nj) continue;
            effs[neff].node   = jx;
            effs[neff].target = sr_world_to_model(&inv_model, fb->effectors[k].target);
            effs[neff].weight = fb->effectors[k].weight;
            neff++;
        }
        if (neff == 0) continue;

        /* Keep the animated positions for the blend. */
        jce_vec3 anim_pos[JCE_FBBIK_MAX_NODES];
        for (uint32_t j = 0; j < nj; j++) anim_pos[j] = body.positions[j];

        int iters = fb->iterations > 0 ? fb->iterations : 10;
        jce_anim_fbbik_solve(&body, effs, neff, iters, 1e-3f);

        /* Blend solved toward animated by (1-blend); blend==1 => full solve. */
        float blend = fb->blend < 1.0f ? fb->blend : 1.0f;
        jce_vec3 solved[JCE_FBBIK_MAX_NODES];
        for (uint32_t j = 0; j < nj; j++)
            solved[j] = jce_v3_lerp(anim_pos[j], body.positions[j], blend);

        /* Position -> rotation write-back (tested pure), then re-derive palette. */
        jce_mat4 new_globals[JCE_MAX_BONES];
        jce_anim_fbbik_write_back(skel, globals, solved, new_globals);
        for (uint32_t j = 0; j < nj; j++) {
            jce_mat4 ib = jce_skeleton_get_inverse_bind(skel, j);
            ai->skin_palette[j] = jce_m4_multiply(&new_globals[j], &ib);
        }
    }
}

/* ── Ragdoll override pass (scene-pass last-mile) ────────────────────────
 *
 * Runs SERIALLY at the very end of sr_update_skinned_anims, AFTER the clip
 * sample AND after the IK post-process — so a live ragdoll's pose WINS over
 * both (precedence decision: physics overrides authored animation/IK).  The
 * runtime publishes the ragdoll's resolved per-bone LOCAL transforms into the
 * shared scene relay (jce_scene_set_ragdoll_pose) after each physics step; this
 * pass reads ONLY that relay and evaluates the LOCAL pose into the skin palette
 * via jce_skeleton_evaluate (the same call the clip path uses).
 *
 * LAYERING: the renderer touches NEITHER jce_ragdoll.h NOR any physics type —
 * it reads jce_mat4 out of the scene relay — so the scene layer stays
 * physics-agnostic and never calls up into the runtime.  Gated on
 * jce_scene_has_ragdoll_pose: for EVERY entity without a published relay pose
 * this is a no-op and the palette is byte-identical to the clip/IK result. */
static void sr_apply_ragdoll_override(JceSceneRenderer *sr, JceScene *scene,
                                      EntityList *list)
{
    const int an = sr_asel_n(list);
    for (int k = 0; k < an; k++) {
        const int i = sr_asel_i(k);
        JceEntity e = list->entities[i];
        if (!jce_scene_has_ragdoll_pose(scene, e)) continue;
        if (!entity_enabled(scene, e)) continue;
        if (!jce_scene_has_skeletal_animator(scene, e)) continue;
        if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SKELETAL_ANIMATOR)) continue;

        JceSkeletalAnimatorComponent *sa = jce_scene_get_skeletal_animator(scene, e);
        if (!sa || !sa->skeleton_path[0]) continue;

        SrModelCache *mc = sr_get_model(sr, sa->skeleton_path, (uint32_t)e);
        if (!mc || !mc->model) continue;

        SrAnimInstance *ai = sr_find_anim_instance(sr, (uint32_t)e);
        if (!ai) continue;

        const JceSkeleton *skel = jce_model_get_skeleton(mc->model);
        if (!skel) continue;

        jce_mat4 locals[JCE_MAX_BONES];
        uint32_t cnt = 0;
        if (!jce_scene_get_ragdoll_pose(scene, e, locals, &cnt) || cnt == 0)
            continue;

        /* Evaluate the ragdoll LOCAL pose into the skinning palette
         * (palette[j] = global[j] * inverse_bind[j]), overwriting the clip/IK
         * result for this instance. */
        jce_skeleton_evaluate(skel, locals, ai->skin_palette, JCE_MAX_BONES);
        ai->skin_palette_count = cnt;
    }
}

/* FEATURE 3.1 last-mile — per-instance morph (blendshape) weight resolution.
 *
 * For each skinned entity whose model carries morph-target data, compute the
 * FINAL per-target weights = the clip-driven morph-weight track (sampled at the
 * active clip's current time) OVERRIDDEN per-target by the entity's authored
 * static JceMorphWeights component, via jce_morph_resolve_weights.  The result
 * is cached on the instance (ai->morph_weights / morph_count).
 *
 * Gating keeps legacy content byte-identical: morph_count stays 0 (and the
 * stored weights untouched) unless the model actually has morph targets AND
 * either a JceMorphWeights component or an imported weight track is present.
 *
 * Once weights are resolved, sr_deform_morph_prims runs the CPU pre-skin deform
 * (jce_morph_apply) into per-instance dynamic vertex buffers that the UNCHANGED
 * skinned program reads; the bone palette / shaders are untouched.  No morph
 * component => morph_count 0 => no deform => no dynamic VB => byte-identical. */

/* Per-instance morph-VB override callback handed to jce_model_draw_morphed /
 * _shadow.  Given (node, prim), returns the live dynamic-VB handle idx for that
 * primitive, or UINT16_MAX to fall through to the static skinned VB.  The same
 * callback (and the same ai) MUST drive both color and shadow so the cast
 * silhouette matches the morphed, lit mesh. */
uint16_t sr_morph_vb_cb(void *user, uint32_t node, uint32_t prim)
{
    const SrAnimInstance *ai = (const SrAnimInstance *)user;
    if (!ai) return (uint16_t)UINT16_MAX;
    for (uint16_t s = 0; s < ai->morph_vb_count; s++) {
        if (ai->morph_vb_node[s] == node && ai->morph_vb_prim[s] == prim)
            return ai->morph_vb[s].idx;   /* UINT16_MAX if not yet created */
    }
    return (uint16_t)UINT16_MAX;
}

/* Lazy-find (or allocate) the per-instance morph-VB slot for a (node, prim).
 * Returns the slot index, or -1 if the bounded array is full. */
static int sr_morph_vb_slot(SrAnimInstance *ai, uint32_t node, uint32_t prim)
{
    for (uint16_t s = 0; s < ai->morph_vb_count; s++) {
        if (ai->morph_vb_node[s] == node && ai->morph_vb_prim[s] == prim)
            return (int)s;
    }
    if (ai->morph_vb_count >= SR_MORPH_PRIM_MAX) return -1;
    int s = (int)ai->morph_vb_count++;
    ai->morph_vb_node[s] = node;
    ai->morph_vb_prim[s] = prim;
    /* handle stays BGFX_INVALID_HANDLE until the first upload creates it */
    return s;
}

/* CPU pre-skin deform: for each morph-bearing primitive, morph the retained
 * base verts by ai->morph_weights and (re)upload into the instance's dynamic
 * VB.  Dirty-gated against ai->morph_last_weights so static weights cost nothing
 * after the first upload.  LOD-guarded: only deform when the bound mesh's vertex
 * count equals the morph delta vertex count. */
static void sr_deform_morph_prims(SrAnimInstance *ai)
{
    if (!ai || !ai->model || ai->morph_count == 0) return;

    /* Dirty token: skip the whole deform when the resolved weights are
     * unchanged from the last upload (count + every value). */
    bool dirty = (ai->morph_last_count != (int)ai->morph_count);
    if (!dirty) {
        for (uint32_t t = 0; t < ai->morph_count; t++) {
            if (ai->morph_last_weights[t] != ai->morph_weights[t]) { dirty = true; break; }
        }
    }
    if (!dirty) return;

    uint32_t nnodes = jce_model_node_count(ai->model);
    for (uint32_t n = 0; n < nnodes; n++) {
        uint32_t nprims = jce_model_node_prim_count(ai->model, n);
        for (uint32_t p = 0; p < nprims; p++) {
            const JceMorphData *md = jce_model_prim_morph(ai->model, n, p);
            if (!md) continue;   /* not a morph-bearing prim */

            uint32_t delta_verts = jce_morph_vertex_count(md);
            uint32_t mesh_verts  = jce_model_prim_vertex_count(ai->model, n, p);
            /* LOD/topology guard: deform only when the bound mesh vertex count
             * matches the morph delta vertex count.  A mismatch (e.g. a future
             * LOD-substituted mesh) disables morph for that prim rather than
             * indexing past the deltas. */
            if (delta_verts == 0 || delta_verts != mesh_verts) continue;

            const JceSkinnedMesh *sm = jce_model_prim_skinned_mesh(ai->model, n, p);
            if (!sm) continue;
            const void *base = jce_skinned_mesh_base_verts(sm);
            uint32_t stride  = jce_skinned_mesh_stride(sm);
            const void *layout = jce_skinned_mesh_layout(sm);
            if (!base || stride == 0 || !layout) continue;  /* not retained */

            int slot = sr_morph_vb_slot(ai, n, p);
            if (slot < 0) continue;   /* per-instance VB array full */

            /* Lazily create the dynamic VB with the SAME layout as the static VB. */
            if (ai->morph_vb[slot].idx == UINT16_MAX) {
                ai->morph_vb[slot] = bgfx_create_dynamic_vertex_buffer(
                    mesh_verts, (const bgfx_vertex_layout_t *)layout,
                    BGFX_BUFFER_NONE);
                if (ai->morph_vb[slot].idx == UINT16_MAX) continue;  /* pool full */
            }

            /* Deform into a bgfx-owned transient buffer, then upload.  IMPORTANT:
             * jce_morph_apply writes ONLY the pos/normal fields — it does NOT
             * touch (or copy) the trailing uv/tangent/joints/weights bytes.  So
             * we first memcpy the FULL base vertex array through (preserving the
             * skinning attributes the GPU palette-skin reads), then run the
             * deform IN-PLACE over that copy (base==out aliasing is supported).
             * This guarantees the dynamic VB carries the SAME interleaved layout
             * as the static VB with only pos/normal rewritten. */
            uint32_t bytes = mesh_verts * stride;
            const bgfx_memory_t *mem = bgfx_alloc(bytes);
            if (!mem) continue;
            memcpy(mem->data, base, bytes);
            jce_morph_apply(md, ai->morph_weights, ai->morph_count,
                            mem->data, mem->data, mesh_verts, stride,
                            (int32_t)jce_skinned_mesh_pos_offset(sm),
                            (int32_t)jce_skinned_mesh_normal_offset(sm));
            bgfx_update_dynamic_vertex_buffer(ai->morph_vb[slot], 0, mem);
        }
    }

    /* Record the uploaded weight vector for the next frame's dirty compare. */
    memcpy(ai->morph_last_weights, ai->morph_weights,
           ai->morph_count * sizeof(float));
    ai->morph_last_count = (int)ai->morph_count;
}

static void sr_resolve_morph_weights(JceSceneRenderer *sr, JceScene *scene,
                                     EntityList *list)
{
    const int an = sr_asel_n(list);
    for (int k = 0; k < an; k++) {
        JceEntity e = list->entities[sr_asel_i(k)];
        SrAnimInstance *ai;
        JceMorphWeightsComponent *mw;
        uint32_t targets;

        if (!entity_enabled(scene, e)) continue;
        if (!jce_scene_has_skeletal_animator(scene, e)) continue;

        ai = sr_find_anim_instance(sr, (uint32_t)e);
        if (!ai) continue;
        ai->morph_count = 0;   /* default: no morph this frame (legacy parity) */

        /* Authored static morph weights drive the deform.  (A clip-driven
         * morph-weight track can later be sampled and passed as `track` here;
         * jce_morph_resolve_weights already supports the combine.)
         * No component -> morph_count stays 0 -> no deform -> byte-identical. */
        if (!jce_scene_has_morph_weights(scene, e)) continue;
        /* Honour the per-component disable (presence-gated: no flag bit). */
        { static int s_mw_cid = -2;
          if (s_mw_cid == -2) s_mw_cid = jce_component_find("MorphWeights");
          if (s_mw_cid >= 0 && !jce_scene_comp_enabled(scene, e, s_mw_cid)) continue; }
        mw = jce_scene_get_morph_weights(scene, e);
        if (!mw || mw->count <= 0) continue;

        targets = (uint32_t)mw->count;
        if (targets > JCE_MORPH_MAX_WEIGHTS) targets = JCE_MORPH_MAX_WEIGHTS;

        /* track = NULL (no clip channel sampled yet) -> resolve_weights falls
         * back to the authored weight for every overridden target. */
        ai->morph_count = jce_morph_resolve_weights(
            NULL, mw->weights, mw->override_mask, targets,
            ai->morph_weights, JCE_MORPH_MAX_WEIGHTS);

        /* Run the CPU pre-skin deform into per-instance dynamic VBs (FEATURE
         * 3.1).  Dirty-gated + LOD-guarded inside; no-op when morph_count == 0. */
        sr_deform_morph_prims(ai);
    }
}

/* ── Animation retargeting (optional per-instance path) ──────────────
 *
 * Play a clip authored for a DIFFERENT (source) skeleton on the entity's own
 * (dst) skeleton.  Self-contained: samples the active clip against the SOURCE
 * rig's rest TRS (clip channels are indexed for the source skeleton), transfers
 * the pose onto the dst skeleton via the cached bind-relative retarget map, then
 * evaluates the dst skeleton into the instance's skin palette.  The map is
 * (re)built when either skeleton pointer or the source path changes.
 *
 * Uses the player purely as a playhead CLOCK (advance + get_time): its own
 * sampled palette — bound to the dst skeleton — is discarded.  This keeps the
 * editor timeline/progress query (jce_scene_renderer_get_anim_player) live while
 * the actual deformation comes from the retargeted dst locals.
 *
 * Returns true if it handled the instance (caller must skip the legacy path);
 * false to fall through to legacy playback (no/invalid retarget source). */
static bool sr_anim_try_retarget(JceSceneRenderer *sr,
                                 JceScene *scene, JceEntity e,
                                 JceSkeletalAnimatorComponent *sa,
                                 SrModelCache *dst_mc, SrAnimInstance *ai,
                                 float dt_sec)
{
    (void)scene;   /* entity resolves through sr_get_model (path-keyed cache) */
    /* Gate: a non-empty source DIFFERENT from the entity's own skeleton. Empty
       or identical ⇒ legacy path (byte-identical). */
    if (!sa->retarget_source_skeleton[0]) return false;
    if (strcmp(sa->retarget_source_skeleton, sa->skeleton_path) == 0) return false;

    /* Resolve (path-keyed, cached) the SOURCE model holding the source rig +
       the clip authored for it. Pending/failed ⇒ skip this frame, but still
       claim the instance so we don't fall back to a mismatched legacy sample. */
    SrModelCache *src_mc = sr_get_model(sr, sa->retarget_source_skeleton, (uint32_t)e);
    if (!src_mc || !src_mc->model) return true;

    JceSkeleton *src_skel = jce_model_get_skeleton(src_mc->model);
    JceSkeleton *dst_skel = jce_model_get_skeleton(dst_mc->model);
    if (!src_skel || !dst_skel) return true;

    /* The active clip lives in the SOURCE model (it is authored for the source
       rig). Resolve it by the same active_clip index used for legacy playback. */
    int ac = sa->active_clip;
    uint32_t src_anim_n = jce_model_anim_count(src_mc->model);
    if (ac < 0 || ac >= (int)src_anim_n) return true;   /* nothing to play */
    JceAnimClip *clip = jce_model_get_anim(src_mc->model, (uint32_t)ac);
    if (!clip) return true;

    /* (Re)build the retarget map when the source path or either skeleton
       pointer changed (model reloads reuse the cache slot, new pointer). */
    if (!ai->retarget_map ||
        ai->retarget_src_skel != src_skel ||
        ai->retarget_dst_skel != dst_skel ||
        strcmp(ai->retarget_src, sa->retarget_source_skeleton) != 0) {
        if (ai->retarget_map) jce_anim_retarget_map_destroy(ai->retarget_map);
        ai->retarget_map = jce_anim_retarget_map_create(src_skel, dst_skel);
        ai->retarget_src_skel = src_skel;
        ai->retarget_dst_skel = dst_skel;
        snprintf(ai->retarget_src, sizeof(ai->retarget_src), "%s",
                 sa->retarget_source_skeleton);
    }
    if (!ai->retarget_map) return true;   /* map build failed; don't mis-sample */

    /* Advance the playhead CLOCK with the dst-bound player (palette ignored).
       Honor the component's playing/loop/speed like the legacy single-clip path
       so the timeline behaves identically. */
    float sp = (sa->speed > 0.0f ? sa->speed : 1.0f);
    bool  loop_eff = sa->loop;
    bool  clip_changed = (ai->active_clip != ac);
    if (sa->playing) {
        if (!jce_anim_player_is_playing(ai->player) || clip_changed ||
            ai->loop != loop_eff) {
            jce_anim_player_play(ai->player, clip, loop_eff, sp);
        }
        jce_anim_player_pause(ai->player, false);
        jce_anim_player_set_speed(ai->player, sp);
        jce_anim_player_update(ai->player, dt_sec, NULL, 0);  /* advance only */
    } else {
        if (clip_changed || ai->loop != loop_eff) {
            jce_anim_player_play(ai->player, clip, loop_eff, sp);
            jce_anim_player_set_time(ai->player, 0.0f);
        }
        if (jce_anim_player_is_playing(ai->player))
            jce_anim_player_pause(ai->player, true);
    }
    ai->active_clip = ac;
    ai->loop  = loop_eff;
    ai->speed = sp;
    ai->paused = !sa->playing;

    /* Sample the clip against the SOURCE skeleton's rest TRS into a SOURCE-sized
       locals buffer (clip channels are indexed for the source rig). */
    uint32_t src_n = jce_skeleton_joint_count(src_skel);
    uint32_t dst_n = jce_skeleton_joint_count(dst_skel);
    if (src_n == 0 || dst_n == 0 || src_n > JCE_MAX_BONES || dst_n > JCE_MAX_BONES)
        return true;

    const jce_vec3 *rest_t = NULL; const jce_quat *rest_r = NULL; const jce_vec3 *rest_s = NULL;
    jce_skeleton_rest_trs(src_skel, &rest_t, &rest_r, &rest_s);

    /* Seed source locals with the source rest pose so joints untouched by the
       clip carry the source bind (the retargeter then maps bind→dst bind). */
    jce_mat4 *src_locals = (jce_mat4 *)JCE_MALLOC(
        (size_t)(src_n + dst_n) * sizeof(jce_mat4));
    if (!src_locals) return true;
    jce_mat4 *dst_locals = src_locals + src_n;
    const jce_mat4 *src_rest = jce_skeleton_rest_pose(src_skel);
    if (src_rest) memcpy(src_locals, src_rest, (size_t)src_n * sizeof(jce_mat4));
    else for (uint32_t j = 0; j < src_n; j++) src_locals[j] = jce_m4_identity();

    float t = jce_anim_player_get_time(ai->player);
    jce_anim_clip_sample(clip, t, src_locals, src_n, rest_t, rest_r, rest_s);

    /* Transfer the source pose onto the dst skeleton, then evaluate. */
    jce_anim_retarget_pose(ai->retarget_map, src_locals, dst_locals);
    jce_skeleton_evaluate(dst_skel, dst_locals, ai->skin_palette, JCE_MAX_BONES);
    ai->skin_palette_count = dst_n;

    JCE_FREE(src_locals);
    return true;
}

/* GPU crowd instancing (JCE_CROWD_INSTANCE): pack every resident skinned
 * character's CURRENT world-space bone palette into one RGBA32F texture (4
 * texels per bone = the bone matrix's 4 columns, bit-for-bit what
 * bgfx_set_transform uploads into u_model[]) and stamp each SrAnimInstance with
 * its bone base offset + this pack frame.  Called once per viewport draw right
 * after sr_update_skinned_anims, so the color/prepass/shadow batchers can draw
 * a whole same-mesh crowd in ONE instanced submit (per-instance base in
 * i_data0.x).  Characters not packed this frame keep a stale crowd_palette_frame
 * so the batchers' equality check fails and they fall back to the per-character
 * skinned path — always correct, opt-in, byte-identical when never called. */
void sr_pack_bone_palettes(JceSceneRenderer *sr)
{
    if (!sr) return;

    /* Editor 2nd viewport: palettes are unchanged (the pose advance is gated
     * by skin_anim_gen to the FIRST viewport), so skip the whole pack + two
     * texture uploads.  MUST return without bumping bone_tex_frame — the bump
     * is the staleness fence, and not bumping preserves crowd_palette_frame
     * equality so viewport 2 still takes the instanced path.  Runtime
     * single-viewport callers (velocity_frame_driven false) are unaffected. */
    if (sr->velocity_frame_driven) {
        if (sr->bone_pack_gen == sr->vel_frame_gen) return;
        sr->bone_pack_gen = sr->vel_frame_gen;
    }

    uint32_t total_bones = 0;
    for (int i = 0; i < SR_ANIM_INSTANCE_MAX; i++) {
        const SrAnimInstance *ai = &sr->anim_inst[i];
        if (ai->used && ai->skin_palette_count > 0)
            total_bones += ai->skin_palette_count;
    }

    /* Bump every call: a character not packed this frame keeps an older
     * crowd_palette_frame, so the batchers fall back to the per-character path. */
    uint32_t frame = ++sr->bone_tex_frame;
    if (total_bones == 0) return;

    const uint16_t W = 512u;                 /* texels/row (128 bone matrices) */
    uint32_t total_texels = total_bones * 4u;
    uint32_t rows = (total_texels + W - 1u) / W;
    if (rows > 16384u) return;               /* bgfx max texture dim guard */
    uint32_t cap_texels = (uint32_t)W * rows;

    if (cap_texels > sr->bone_tex_texel_cap || !BGFX_HANDLE_IS_VALID(sr->bone_tex)) {
        JCE_FREE(sr->bone_pack_buf);
        sr->bone_pack_buf = (float *)JCE_MALLOC((size_t)cap_texels * 4u * sizeof(float));
        if (!sr->bone_pack_buf) { sr->bone_tex_texel_cap = 0u; return; }
        if (BGFX_HANDLE_IS_VALID(sr->bone_tex))
            bgfx_destroy_texture(sr->bone_tex);
        if (BGFX_HANDLE_IS_VALID(sr->bone_prev_tex))
            bgfx_destroy_texture(sr->bone_prev_tex);
        const uint64_t flags = BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                               BGFX_SAMPLER_MIP_POINT | BGFX_SAMPLER_U_CLAMP |
                               BGFX_SAMPLER_V_CLAMP;
        sr->bone_tex = bgfx_create_texture_2d(W, (uint16_t)rows, false, 1,
                                              BGFX_TEXTURE_FORMAT_RGBA32F, flags, NULL, 0);
        /* PREV-palette sibling (animated crowd velocity): same dims, packed at
         * the same bases.  Optional — velocity falls back per-char without it. */
        sr->bone_prev_tex = bgfx_create_texture_2d(W, (uint16_t)rows, false, 1,
                                                   BGFX_TEXTURE_FORMAT_RGBA32F, flags, NULL, 0);
        if (!BGFX_HANDLE_IS_VALID(sr->bone_tex)) { sr->bone_tex_texel_cap = 0u; return; }
        sr->bone_tex_w = W;
        sr->bone_tex_h = (uint16_t)rows;
        sr->bone_tex_texel_cap = cap_texels;
    }

    uint32_t base = 0;
    for (int i = 0; i < SR_ANIM_INSTANCE_MAX; i++) {
        SrAnimInstance *ai = &sr->anim_inst[i];
        if (!ai->used || ai->skin_palette_count == 0) continue;
        memcpy(sr->bone_pack_buf + (size_t)base * 16u, ai->skin_palette,
               (size_t)ai->skin_palette_count * 16u * sizeof(float));
        ai->crowd_palette_base  = base;
        ai->crowd_palette_frame = frame;
        base += ai->skin_palette_count;
    }

    uint32_t rows_used = (total_texels + W - 1u) / W;
    uint32_t bytes = (uint32_t)W * rows_used * 4u * (uint32_t)sizeof(float);
    const bgfx_memory_t *mem = bgfx_copy(sr->bone_pack_buf, bytes);
    uint16_t pitch = (uint16_t)((uint32_t)W * 4u * (uint32_t)sizeof(float)); /* 512*16=8192 */
    bgfx_update_texture_2d(sr->bone_tex, 0, 0, 0, 0, W, (uint16_t)rows_used, mem, pitch);

    /* Second walk: PREVIOUS palettes at the SAME bases (staging buffer reused —
     * the cur upload above already copied out via bgfx_copy).  A char without a
     * valid prev (first frame) packs its CURRENT palette => zero bone motion,
     * exactly the per-char path's prev==NULL semantics; a prev shorter than cur
     * pads the tail bones with CURRENT (same skeleton in practice). */
    if (BGFX_HANDLE_IS_VALID(sr->bone_prev_tex)) {
        base = 0;
        for (int i = 0; i < SR_ANIM_INSTANCE_MAX; i++) {
            const SrAnimInstance *ai = &sr->anim_inst[i];
            if (!ai->used || ai->skin_palette_count == 0) continue;
            uint32_t n    = ai->skin_palette_count;
            uint32_t np   = (ai->prev_skin_valid && ai->prev_skin_palette_count > 0)
                              ? ai->prev_skin_palette_count : 0;
            uint32_t take = np < n ? np : n;
            if (take > 0)
                memcpy(sr->bone_pack_buf + (size_t)base * 16u, ai->prev_skin_palette,
                       (size_t)take * 16u * sizeof(float));
            if (take < n)
                memcpy(sr->bone_pack_buf + (size_t)(base + take) * 16u,
                       ai->skin_palette + take,
                       (size_t)(n - take) * 16u * sizeof(float));
            base += n;
        }
        const bgfx_memory_t *pmem = bgfx_copy(sr->bone_pack_buf, bytes);
        bgfx_update_texture_2d(sr->bone_prev_tex, 0, 0, 0, 0, W, (uint16_t)rows_used,
                               pmem, pitch);
    }
}

void sr_update_skinned_anims(JceSceneRenderer *sr, JceScene *scene,
                                    EntityList *list, float dt_sec,
                                    const JceCamera *camera)
{
    /* Pass 1 (this loop, serial): resolve models/instances, drive SM/blend
     * lifecycle, select clips — all shared/lazy state.  The actual pose
     * sampling is deferred into `reqs` and run in parallel afterwards
     * (Pass 2), then frame events fire serially (Pass 3). */
    SrAnimSample reqs[SR_ANIM_INSTANCE_MAX];
    int req_count = 0;

    /* TAA velocity: the editor renders the Scene + Game viewports through this
     * one shared renderer every displayed frame.  Advance the animation AND
     * snapshot the previous-frame skin palette only on the FIRST viewport of
     * the frame (gated by the per-frame generation); the 2nd viewport reuses the
     * already-computed palettes.  Without this, the 2nd pass advances the pose a
     * 2nd time and snapshots THIS frame's palette as "prev" → cur==prev → zero
     * per-bone motion → skinned characters ghost in TAA.
     *
     * Gated on velocity_frame_driven (the editor calls begin_velocity_frame once
     * per frame): this must run on the FIRST viewport even though THAT viewport
     * (e.g. the Scene view) may have velocity off, because the Game view that
     * DOES want velocity renders 2nd and would otherwise snapshot this-frame's
     * already-sampled palette as prev.  Runtime/single-viewport callers that
     * never call begin_velocity_frame keep the original per-render path. */
    if (sr->velocity_frame_driven) {
        if (sr->skin_anim_gen == sr->vel_frame_gen) return;
        sr->skin_anim_gen = sr->vel_frame_gen;
    }

    /* O(1) empty-scene early-out: a world with NO SkeletalAnimator components
     * (e.g. 150k static primitives) paid a per-entity probe below every frame
     * (~84 ms/frame at 150k = the #1 CPU phase, on loops that could never hit).
     * Zero components + zero live instances => provably no work; live instances
     * without components still take the full path so stale-instance pruning
     * keeps working.  The 256-slot scan is negligible next to the list walk. */
    if (jce_scene_count_skeletal_animators(scene) == 0) {
        bool live = false;
        for (int li = 0; li < SR_ANIM_INSTANCE_MAX; li++)
            if (sr->anim_inst[li].used) { live = true; break; }
        if (!live) return;
    }

    /* Build the animator sub-list once for this update + the IK/morph/ragdoll
     * passes below (see sr_anim_build_selection).  A component ADDED mid-frame
     * by an anim-event callback joins the passes next frame. */
    sr_anim_build_selection(scene, list);

    const int an = sr_asel_n(list);
    for (int k = 0; k < an; k++) {
        const int i = sr_asel_i(k);
        JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;
        if (!jce_scene_has_skeletal_animator(scene, e)) continue;
        if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SKELETAL_ANIMATOR)) continue;

        JceSkeletalAnimatorComponent *sa = jce_scene_get_skeletal_animator(scene, e);
        if (!sa || !sa->skeleton_path[0]) continue;

        SrModelCache *mc = sr_get_model(sr, sa->skeleton_path, (uint32_t)e);
        if (!mc || !mc->model) continue;

        SrAnimInstance *ai = sr_get_anim_instance(sr, (uint32_t)e, mc->model);
        if (!ai) continue;

        /* Sim-LOD animation gating (large-world #3, Play-only): freeze a far-tier
         * actor's skinned pose — keep the already-evaluated palette and skip the
         * reset + re-sample below, so the renderer draws the frozen pose at zero
         * pose-eval cost.  Opt-in via the SimLod ANIM gate bit or the gate_anim_far
         * hint; skip the first frame (no palette yet) so the bind pose is replaced.
         * Gated to anim_sm_active (Play) so the editor viewport keeps full-fidelity
         * animation for authoring. */
        if (sr->anim_sm_active && camera && ai->skin_palette_count > 0) {
            JceSimLodComponent *sl = jce_scene_get_sim_lod(scene, e);
            if (sl && sl->enabled &&
                ((sl->gate_mask & JCE_SIMLOD_GATE_ANIM) || sl->gate_anim_far)) {
                float mid = sl->mid_radius > 0.0f ? sl->mid_radius : 80.0f;
                jce_mat4 wm = jce_scene_get_world_matrix(scene, e);
                jce_vec3 eye = jce_camera_get_position(camera);
                float dx = wm.col[3].x - eye.x;
                float dy = wm.col[3].y - eye.y;
                float dz = wm.col[3].z - eye.z;
                if (dx * dx + dy * dy + dz * dz > mid * mid)
                    continue;                 /* FAR tier -> freeze pose */
            }
        }
        /* TAA per-bone motion: snapshot the PRIOR (last-frame) skin palette into
           prev_skin_palette BEFORE this frame overwrites it.  The contents of
           skin_palette still hold last frame's pose here (the sample pass
           rewrites them later), and skin_palette_count is reset to 0 just below
           — so capture both now.  prev_skin_valid gates the first frame (no prev
           => no bone motion that frame).
           Captured whenever a velocity view is active this frame OR the editor
           drives the frame: in the editor the FIRST viewport (which may be the
           Scene view with velocity off) must capture prev here, because the Game
           view that wants velocity renders 2nd and reuses these palettes. */
        if (sr->taa_want_velocity || sr->velocity_frame_driven) {
            uint32_t pc = ai->skin_palette_count;
            if (pc > JCE_MAX_BONES) pc = JCE_MAX_BONES;
            if (pc > 0) {
                memcpy(ai->prev_skin_palette, ai->skin_palette,
                       pc * sizeof(jce_mat4));
                ai->prev_skin_palette_count = pc;
                ai->prev_skin_valid = true;
            } else {
                ai->prev_skin_palette_count = 0;
                /* keep prev_skin_valid as-is: a momentary 0-count frame should
                   not permanently disable prev once it was valid */
            }
        }
        ai->skin_palette_count = 0;
        if (!ai->player) continue;

        /* Frame events: one-shot lazy load of the <skeleton>.anim.json sidecar. */
        sr_anim_events_load(ai, sa->skeleton_path, mc->model);

        /* OPTIONAL retargeting: when the entity authors a source skeleton, play
           the active clip (authored for THAT rig) on this entity's skeleton via
           the bind-relative retargeter, then skip the legacy SM/blend/single-
           clip paths.  Returns false (no/identical source) → legacy path runs
           verbatim, byte-identical. */
        if (sr_anim_try_retarget(sr, scene, e, sa, mc, ai, dt_sec))
            continue;

        int ac = sa->active_clip;
        /* OPT-IN auto-locomotion: only when the component sets auto_speed do we
         * compute movement speed and feed it into the SM "Speed" param /
         * blend_param. Default off → the engine stays generic (params and
         * blend_param are driven by game code or authored values; the renderer
         * only EVALUATES the SM/blend tree). Also gated to Play. */
        float move_speed = 0.0f;
        if (sr->anim_sm_active && sa->auto_speed) {
            /* Prefer the live physics state written by the runtime's
             * character driver (loco_valid) — steadier than the transform
             * delta and immune to interpolation noise. */
            move_speed = sa->loco_valid
                       ? sa->loco_speed
                       : sr_compute_entity_speed(
                             ai, jce_scene_get_transform(scene, e), dt_sec);
        }

        /* Effective loop/speed for the single-clip path below; an active SM
         * state overrides them with its authored values.  sm_handoff marks
         * the frame an SM transition completed — only then may the pending
         * sm_seed_time be applied to the freshly-played clip. */
        bool  loop_eff = sa->loop;
        float sm_speed_mul = 1.0f;
        bool  sm_handoff = false;
        /* Airborne per live physics: the 1D blend tree only models GROUND
         * locomotion (speed-blended walk/run), so while airborne it yields
         * to the SM / single-clip path — otherwise jumps keep showing the
         * walk/run blend. */
        bool  loco_airborne = sr->anim_sm_active && sa->auto_speed &&
                              sa->loco_valid && !sa->loco_grounded;

        /* State-machine override: when an .anim_sm.json is bound, tick it and
           let it pick the active clip by name (parameter-driven transitions).
           The binding is per-instance runtime state, (re)created when the path
           changes; falls through to single-clip playback when unbound. */
        if (sa->sm_path[0]) {
            if (!ai->sm_binding || strcmp(ai->sm_path, sa->sm_path) != 0) {
                if (ai->sm_binding) jce_anim_sm_binding_destroy(ai->sm_binding);
                /* PAK-first (single-exe bundled .anim_sm.json), else resolve
                 * scene-relative paths through the host callback (editor /
                 * loose tree) — the raw path would open relative to the
                 * process CWD and fail. */
                ai->sm_binding = sr_anim_load_sm(sr, sa->sm_path);
                snprintf(ai->sm_path, sizeof(ai->sm_path), "%s", sa->sm_path);
                ai->sm_trans_idx = -1;
                ai->sm_seed_time = -1.0f;
                /* Fresh binding = fresh SM at its default state: re-seed the
                 * state-change cursor so the new SM's initial on_state_enter
                 * fires once (also covers re-entering Play, which rebinds). */
                ai->sm_prev_state = SR_SM_STATE_SEED;
            }
            if (ai->sm_binding && sr->anim_sm_active) {
                /* Only DRIVE the SM in Play; in the editor the binding stays
                 * idle so manual clip selection previews normally. The engine
                 * auto-feeds "Speed" ONLY when auto_speed is set — otherwise the
                 * SM's params are whatever game code set (engine stays generic);
                 * tick still runs so those game-driven params take effect. */
                if (sa->auto_speed) {
                    jce_anim_sm_binding_set_float(ai->sm_binding, "Speed",
                                                  move_speed);
                    /* Physics-backed locomotion params (each set_* is a no-op
                     * when the SM doesn't declare the param, so plain Speed-
                     * only SMs are unaffected).  loco_jump is one-shot. */
                    if (sa->loco_valid) {
                        jce_anim_sm_binding_set_bool(ai->sm_binding,
                                                     "IsGrounded",
                                                     sa->loco_grounded);
                        jce_anim_sm_binding_set_float(ai->sm_binding,
                                                      "VerticalVel",
                                                      sa->loco_vert_vel);
                        if (sa->loco_jump) {
                            jce_anim_sm_binding_set_trigger(ai->sm_binding,
                                                            "Jump");
                            sa->loco_jump = false;
                        }
                    }
                }
                /* Drain script-pushed SM commands (jce.anim_set_float/int/bool/
                 * trigger via the scene relay) into the binding BEFORE ticking,
                 * so script-driven animation (attack/hit/death/etc.) takes
                 * effect this frame.  Runs regardless of auto_speed. */
                {
                    JceAnimParamCmd cmds[JCE_ANIM_CMD_RELAY_MAX];
                    uint32_t nc = jce_scene_anim_take_params(scene, e, cmds,
                                                             JCE_ANIM_CMD_RELAY_MAX);
                    for (uint32_t ci = 0; ci < nc; ++ci) {
                        const JceAnimParamCmd *c = &cmds[ci];
                        switch (c->type) {
                        case JCE_ANIM_PARAM_FLOAT:
                            jce_anim_sm_binding_set_float(ai->sm_binding, c->name, c->value); break;
                        case JCE_ANIM_PARAM_INT:
                            jce_anim_sm_binding_set_int(ai->sm_binding, c->name, (int)c->value); break;
                        case JCE_ANIM_PARAM_BOOL:
                            jce_anim_sm_binding_set_bool(ai->sm_binding, c->name, c->value != 0.0f); break;
                        case JCE_ANIM_PARAM_TRIGGER:
                            jce_anim_sm_binding_set_trigger(ai->sm_binding, c->name); break;
                        }
                    }
                }
                jce_anim_sm_binding_tick(ai->sm_binding, dt_sec);
                int an = (int)jce_model_anim_count(mc->model);
                if (an > 0) {
                    const char *names[64];
                    if (an > 64) an = 64;
                    for (int ci = 0; ci < an; ci++)
                        names[ci] = jce_anim_clip_name(
                            jce_model_get_anim(mc->model, (uint32_t)ci));
                    int sm_clip = jce_anim_sm_binding_resolve_clip_index(
                        ai->sm_binding, names, an);
                    if (sm_clip >= 0) ac = sm_clip;
                }

                const JceAnimSmEval *ev = jce_anim_sm_binding_eval(ai->sm_binding);
                JceAnimSm *smr = jce_anim_sm_binding_runtime(ai->sm_binding);

                /* State-change → gameplay dispatch (state-enter/exit). Only
                 * polled when a hook is installed (the runtime's entity→script
                 * dispatch), so this is provably zero-cost otherwise. The poll
                 * fires once the SM's active state has actually changed (after a
                 * transition completes / on an instant transition / on the first
                 * poll into the initial state via the seed sentinel). */
                if (sr->anim_state_fn && smr) {
                    int sm_from = -1, sm_to = -1;
                    if (jce_anim_sm_poll_state_change(smr, &ai->sm_prev_state,
                                                      &sm_from, &sm_to)) {
                        const char *from_name =
                            (sm_from >= 0) ? jce_anim_sm_state_name(smr, sm_from)
                                           : NULL;
                        const char *to_name =
                            (sm_to >= 0) ? jce_anim_sm_state_name(smr, sm_to)
                                         : NULL;
                        sr->anim_state_fn((uint64_t)ai->entity, from_name,
                                          to_name, sr->anim_state_user);
                    }
                }

                float comp_sp = sa->speed > 0.0f ? sa->speed : 1.0f;
                if (ev && smr && ev->transition_index >= 0 &&
                    (!sa->use_blend_tree || loco_airborne)) {
                    /* Transition crossfade: sample the from/to state clips
                       together, weighted by the transition blend, instead of
                       the old hard clip switch at transition end. */
                    if (ai->sm_trans_idx != ev->transition_index) {
                        ai->sm_trans_idx  = ev->transition_index;
                        ai->sm_trans_time = 0.0f;
                    }
                    ai->sm_trans_time += dt_sec;
                    JceAnimClip *from_clip =
                        sr_sm_state_model_clip(smr, ev->from_state, mc->model);
                    JceAnimClip *to_clip =
                        sr_sm_state_model_clip(smr, ev->to_state, mc->model);
                    if (from_clip && to_clip) {
                        float fsp = jce_anim_sm_state_speed(smr, ev->from_state) * comp_sp;
                        float tsp = jce_anim_sm_state_speed(smr, ev->to_state) * comp_sp;
                        bool  flp = jce_anim_sm_state_loop(smr, ev->from_state);
                        bool  tlp = jce_anim_sm_state_loop(smr, ev->to_state);
                        float fdur = jce_anim_clip_duration(from_clip);
                        float tdur = jce_anim_clip_duration(to_clip);
                        float ft = ev->state_time * fsp;
                        float tt = ai->sm_trans_time * tsp;
                        ft = (fdur > 0.0001f)
                           ? (flp ? fmodf(ft, fdur) : (ft < fdur ? ft : fdur))
                           : 0.0f;
                        tt = (tdur > 0.0001f)
                           ? (tlp ? fmodf(tt, tdur) : (tt < tdur ? tt : tdur))
                           : 0.0f;
                        SrAnimSample req = {0};
                        req.ai = ai; req.mode = 2;
                        req.clip_a = from_clip; req.ta = ft; req.wa = 1.0f - ev->blend;
                        req.clip_b = to_clip;   req.tb = tt; req.wb = ev->blend;
                        if (req_count < SR_ANIM_INSTANCE_MAX) reqs[req_count++] = req;
                        else                                  sr_anim_do_sample(&req);
                        /* Hand the to-clip's running time to the single-clip
                           player on the completion frame (no pose pop). */
                        ai->sm_seed_time = tt;
                        ai->active_clip  = -1;   /* blend owns the pose */
                        ai->loop   = tlp;
                        ai->speed  = tsp;
                        ai->paused = false;
                        continue;   /* skip blend-tree + single-clip paths */
                    }
                } else if (ev && smr && ev->state_index >= 0) {
                    /* Steady state: honor the SM state's speed/loop (the old
                       path ignored both, so non-looping Jump_Start/Land
                       states would loop forever). */
                    if (ai->sm_trans_idx >= 0) sm_handoff = true;
                    ai->sm_trans_idx = -1;
                    sm_speed_mul = jce_anim_sm_state_speed(smr, ev->state_index);
                    loop_eff     = jce_anim_sm_state_loop(smr, ev->state_index);
                }
            }
        } else if (ai->sm_binding) {
            /* sm_path cleared at runtime — drop the stale binding. */
            jce_anim_sm_binding_destroy(ai->sm_binding);
            ai->sm_binding = NULL;
            ai->sm_path[0] = '\0';
            ai->sm_trans_idx = -1;
            ai->sm_seed_time = -1.0f;
            ai->sm_prev_state = SR_SM_STATE_SEED;
        }
        float sp = (sa->speed > 0.0f ? sa->speed : 1.0f) * sm_speed_mul;

        /* Blend-tree path: cross-blend the two clips bracketing blend_param.
           Takes precedence over the SM / single-clip path WHILE GROUNDED;
           airborne falls through so jump/fall clips (SM-resolved, or name-
           driven by the runtime) can play. The 1D tree is cached on the
           instance and rebuilt only when the clip set or the thresholds
           change. */
        if (sa->use_blend_tree && !loco_airborne) {
            /* Opt-in locomotion: auto-drive the blend by movement speed only
             * when auto_speed is set (and in Play). Otherwise blend_param is
             * whatever game code / the author set it to — so the blend tree is
             * generic (can be driven by direction, lean, etc., not just speed). */
            if (sr->anim_sm_active && sa->auto_speed) sa->blend_param = move_speed;
            int bmode = sa->blend_mode;
            if (bmode < 0 || bmode > 2) bmode = 0;
            int cc = sa->clip_count;
            if (cc < 0) cc = 0;
            if (cc > 8) cc = 8;
            bool rebuild = (!ai->blend_tree) || (ai->bt_count != cc) ||
                           (ai->bt_mode != bmode);
            for (int t = 0; t < cc && !rebuild; t++) {
                if (ai->bt_thresh[t] != sa->blend_thresholds[t]) rebuild = true;
                if (bmode != 0 && ai->bt_pos_y[t] != sa->blend_pos_y[t])
                    rebuild = true;
            }
            if (rebuild && cc > 0) {
                if (ai->blend_tree) jce_anim_blend_tree_destroy(ai->blend_tree);
                if (bmode == 0)
                    ai->blend_tree = jce_anim_blend_tree_create_1d((uint32_t)cc);
                else
                    ai->blend_tree = jce_anim_blend_tree_create_2d(
                        (uint32_t)cc, bmode == 2 /* directional */);
                ai->bt_count = cc;
                ai->bt_mode  = bmode;
                int an = (int)jce_model_anim_count(mc->model);
                for (int t = 0; t < cc; t++) {
                    ai->bt_thresh[t] = sa->blend_thresholds[t];
                    ai->bt_pos_y[t]  = sa->blend_pos_y[t];
                    if (bmode == 0)
                        jce_anim_blend_tree_add(ai->blend_tree, sa->clip_names[t],
                                                sa->blend_thresholds[t]);
                    else
                        jce_anim_blend_tree_add_2d(ai->blend_tree, sa->clip_names[t],
                                                   sa->blend_thresholds[t],
                                                   sa->blend_pos_y[t]);
                    bool matched = false;
                    for (int k = 0; k < an; k++) {
                        const char *cn = jce_anim_clip_name(
                            jce_model_get_anim(mc->model, (uint32_t)k));
                        if (sr_clip_name_match(cn, sa->clip_names[t])) {
                            jce_anim_blend_tree_set_clip(ai->blend_tree,
                                sa->clip_names[t],
                                jce_model_get_anim(mc->model, (uint32_t)k));
                            matched = true;
                            break;
                        }
                    }
                    if (!matched && sa->clip_names[t][0])
                        LOG_WARN(LOG_TAG, "blend tree: clip '%s' not found in "
                                 "model '%s' (%d clips) — check name/casing",
                                 sa->clip_names[t], sa->skeleton_path, an);
                }
            }
            if (ai->blend_tree && ai->bt_count > 0) {
                const JceAnimClip *clip_a = NULL, *clip_b = NULL;
                float wa = 1.0f, wb = 0.0f;
                if (bmode == 0) {
                    JceAnimBlendTreeEval bt;
                    jce_anim_blend_tree_evaluate(ai->blend_tree, sa->blend_param, &bt);
                    clip_a = bt.clip_a; wa = bt.weight_a;
                    clip_b = bt.clip_b; wb = bt.weight_b;
                } else {
                    /* 2D: reduce the per-sample weight vector to the two
                       dominant clips and blend them with the tested two-clip
                       path (renderer has no N-way blend primitive). */
                    JceAnimBlendTreeEval2D e2;
                    jce_anim_blend_tree_eval_2d(ai->blend_tree, sa->blend_param,
                                                sa->blend_param_y, &e2);
                    int i0 = -1, i1 = -1;
                    float w0 = -1.0f, w1 = -1.0f;
                    for (uint32_t i = 0; i < e2.count; i++) {
                        if (e2.weights[i] > w0) {
                            w1 = w0; i1 = i0;
                            w0 = e2.weights[i]; i0 = (int)i;
                        } else if (e2.weights[i] > w1) {
                            w1 = e2.weights[i]; i1 = (int)i;
                        }
                    }
                    if (i0 >= 0) { clip_a = e2.clips[i0]; wa = w0 > 0.0f ? w0 : 0.0f; }
                    if (i1 >= 0) { clip_b = e2.clips[i1]; wb = w1 > 0.0f ? w1 : 0.0f; }
                    float ws = wa + wb;
                    if (ws > 0.0001f) { wa /= ws; wb /= ws; } else { wa = 1.0f; wb = 0.0f; }
                }
                ai->bt_time += dt_sec * sp;          /* shared phase */
                float da = clip_a ? jce_anim_clip_duration(clip_a) : 0.0f;
                float db = clip_b ? jce_anim_clip_duration(clip_b) : 0.0f;
                float ta = da > 0.0001f ? fmodf(ai->bt_time, da) : 0.0f;
                float tb = db > 0.0001f ? fmodf(ai->bt_time, db) : 0.0f;
                /* Defer the blend sample to the parallel pass. */
                SrAnimSample req = {0};
                req.ai = ai; req.mode = 2;
                req.clip_a = clip_a; req.ta = ta; req.wa = wa;
                req.clip_b = clip_b; req.tb = tb; req.wb = wb;
                if (req_count < SR_ANIM_INSTANCE_MAX) reqs[req_count++] = req;
                else                                  sr_anim_do_sample(&req);
                ai->active_clip = -1;   /* tree owns the pose this frame */
                ai->loop   = sa->loop;
                ai->speed  = sp;
                ai->paused = false;
                ai->sm_seed_time = -1.0f;   /* tree owns the pose; drop any
                                               pending SM hand-off */
                continue;               /* skip the single-clip path */
            }
        }

        JceAnimClip *clip = NULL;
        if (ac >= 0 && ac < (int)jce_model_anim_count(mc->model))
            clip = jce_model_get_anim(mc->model, (uint32_t)ac);

        bool comp_playing = sa->playing;
        bool clip_changed = (ai->active_clip != ac);
        bool loop_changed = (ai->loop != loop_eff);
        bool speed_changed = fabsf(ai->speed - sp) > 0.0001f;
        bool paused_changed = (ai->paused == comp_playing);

        if (comp_playing && clip) {
            /* A finished NON-LOOPING clip holds its last pose until the clip
             * changes (e.g. an SM exit-time transition fires) — restarting it
             * every frame would strobe the first pose forever. */
            bool ended_hold = !loop_eff && !clip_changed && !loop_changed &&
                              !jce_anim_player_is_playing(ai->player);
            if (ended_hold) {
                /* Re-pin the player at the clip END each held frame: a
                 * stopped player's update() early-outs without writing a
                 * pose, and an empty palette degrades to the BIND pose.
                 * Events can't re-fire — the (prev,cur] interval at the
                 * end is empty. */
                jce_anim_player_play(ai->player, clip, false, sp);
                jce_anim_player_set_time(ai->player,
                                         jce_anim_clip_duration(clip));
            } else if (!jce_anim_player_is_playing(ai->player)
                || clip_changed || loop_changed) {
                jce_anim_player_play(ai->player, clip, loop_eff, sp);
                /* Continue from the SM crossfade's to-clip time on the
                 * transition-completion frame instead of popping to 0. */
                if (sm_handoff && ai->sm_seed_time >= 0.0f)
                    jce_anim_player_set_time(ai->player, ai->sm_seed_time);
            } else if (speed_changed || paused_changed) {
                jce_anim_player_set_speed(ai->player, sp);
            }
            jce_anim_player_pause(ai->player, false);
            jce_anim_player_set_speed(ai->player, sp);
        } else {
            if (clip && (clip_changed || loop_changed)) {
                jce_anim_player_play(ai->player, clip, loop_eff, sp);
                jce_anim_player_set_time(ai->player, 0.0f);
            }
            if (jce_anim_player_is_playing(ai->player))
                jce_anim_player_pause(ai->player, true);
        }
        ai->sm_seed_time = -1.0f;   /* consumed (or irrelevant) this frame */

        ai->active_clip = ac;
        ai->loop = loop_eff;
        ai->speed = sp;
        ai->paused = !comp_playing;

        /* Root motion (FEATURE 3.2): when the entity carries a JceAvatar with
         * apply_root_motion set AND we're in Play, drive the player to extract
         * the root joint's per-frame delta and re-center the rendered pose. The
         * consumed delta is stashed on the component for the runtime's transform
         * step in Pass 3.  Gated to Play and to the single-clip path so the
         * editor preview and the existing physics-driven player are untouched
         * (apply_root_motion=false → byte-identical to before). */
        bool                want_root_motion = false;
        JceAvatarComponent *av               = NULL;
        if (jce_scene_has_avatar(scene, e)) {
            av = jce_scene_get_avatar(scene, e);
            want_root_motion = sr->anim_sm_active && av && av->apply_root_motion;
        }
        {
            JceSkeleton *rmskel = jce_model_get_skeleton(mc->model);
            uint32_t root_j = want_root_motion ? sr_skeleton_root_joint(rmskel) : 0;
            jce_anim_player_set_root_motion(ai->player, want_root_motion, root_j);
            /* When root motion is OFF this frame (e.g. left Play, avatar flag
             * cleared, or this entity dropped to the inline-sample path), make
             * sure no stale delta lingers on the component: the runtime would
             * otherwise apply a previous frame's rm_* once more before noticing
             * it is no longer being refreshed. */
            if (!want_root_motion && sa->rm_valid) {
                sa->rm_valid = false;
                sa->rm_dx = sa->rm_dy = sa->rm_dz = 0.0f;
                sa->rm_dyaw = 0.0f;
            }
        }

        /* Avatar bone mask (FEATURE 3.3): when the avatar authors a .mask, the
         * single-clip pose is composed as a per-bone MASKED OVERRIDE of the
         * active clip onto the rest pose (masked-out bones return to rest).
         * Resolve/cache the mask; an absent or unloadable mask leaves the
         * request on the plain single-clip path (mode 1) so playback is
         * byte-identical for the no-mask case (all current content).  Root
         * motion takes precedence (it owns the player update + re-center), so
         * the masked override yields to it. */
        JceAvatarMask *amask = NULL;
        if (av && av->mask_path[0] && !want_root_motion)
            amask = sr_anim_resolve_mask(sr, ai, av->mask_path, mc->model);

        /* Avatar additive/override layer stack (FEATURE 3.3 authoring): when the
         * avatar authors layers[] they are composited on top of the active clip
         * via the real layered-blend path. Each layer's clip is resolved by name
         * in the model and its .mask cached per slot. Takes precedence over the
         * single-mask path; yields to root motion (which owns the player update).
         * No authored layers → byte-identical (mode 1/3). */
        JceAnimLayer  av_layers[SR_AVATAR_MAX_LAYERS];
        int           av_layer_n = 0;
        if (av && av->layer_count > 0 && !want_root_motion) {
            int lc = av->layer_count;
            if (lc > SR_AVATAR_MAX_LAYERS) lc = SR_AVATAR_MAX_LAYERS;
            float ltime = jce_anim_player_get_time(ai->player);
            for (int li = 0; li < lc; li++) {
                const JceAvatarLayer *src = &av->layers[li];
                if (!src->clip[0] || src->weight <= 0.0f) continue;
                JceAnimClip *lclip = sr_find_model_clip(mc->model, src->clip);
                if (!lclip) continue;   /* missing clip simply contributes nothing */
                JceAvatarMask *lmask = src->mask_path[0]
                    ? sr_anim_resolve_layer_mask(sr, ai, li, src->mask_path, mc->model)
                    : sr_anim_resolve_layer_mask(sr, ai, li, NULL, mc->model);
                JceAnimLayer *L = &av_layers[av_layer_n++];
                L->clip     = lclip;
                L->time     = ltime;
                L->weight   = src->weight > 1.0f ? 1.0f : src->weight;
                L->mode     = (src->mode == 1) ? JCE_ANIM_LAYER_OVERRIDE
                                               : JCE_ANIM_LAYER_ADDITIVE;
                L->mask     = lmask;
                L->ref_clip = NULL;   /* additive ref = rest pose */
                L->ref_time = 0.0f;
            }
        }

        /* Defer the single-clip sample (advances player time + writes the
         * palette) to the parallel pass; frame events fire afterwards in
         * Pass 3 (they read post-sample time and dispatch into game code,
         * so they MUST stay on the main thread). */
        {
            SrAnimSample req = {0};
            req.ai = ai; req.dt = dt_sec; req.ac = ac;
            if (av_layer_n > 0 && clip) {
                req.mode = 4; req.layer_clip = clip; req.layer_n = av_layer_n;
                for (int li = 0; li < av_layer_n; li++) req.layers[li] = av_layers[li];
            } else if (amask && clip) {
                req.mode = 3; req.mask = amask; req.layer_clip = clip;
            } else {
                req.mode = 1;
            }
            if (req_count < SR_ANIM_INSTANCE_MAX) {
                reqs[req_count++] = req;
            } else {
                sr_anim_do_sample(&req);
                sr_anim_events_advance(sr, ai, ac);
            }
        }

        /* Two-bone IK runs in the dedicated serial pass at the end of this
           function (sr_apply_ik_constraints): it must execute AFTER the
           parallel pose sample (Pass 2) has written this instance's palette,
           and it covers the inline-sample overflow path above as well because
           it iterates the entity list, not the reqs array. */
    }

    /* Pass 2: sample all deferred poses in parallel.  Each request targets a
     * distinct instance whose player owns its scratch buffers, so this only
     * reads shared skeletons/clips and writes per-instance palettes. */
    if (req_count > 0) {
        JceThreadPool *pool = jce_thread_pool_shared();
        if (pool && req_count >= 2)
            jce_thread_pool_parallel_for(pool, (uint32_t)req_count, 1,
                                         sr_anim_sample_range, reqs);
        else
            sr_anim_sample_range(0u, (uint32_t)req_count, reqs);

        /* Pass 3 (serial): fire frame events for single-clip samples — they
         * read the post-sample player time and dispatch into game code. */
        for (int i = 0; i < req_count; i++)
            if (reqs[i].mode == 1)
                sr_anim_events_advance(sr, reqs[i].ai, reqs[i].ac);

        /* Pass 3b (serial): harvest root motion produced this frame and hand it
         * to the component's transient rm_* fields so the runtime's transform
         * step (which owns entity position write-back) can apply it.  Only the
         * single-clip path enables root motion on the player today. */
        for (int i = 0; i < req_count; i++) {
            SrAnimInstance *ai = reqs[i].ai;
            if (reqs[i].mode != 1 || !ai || !ai->player) continue;
            JceSkeletalAnimatorComponent *sa =
                jce_scene_get_skeletal_animator(scene, (JceEntity)ai->entity);
            if (!sa) continue;
            JceAnimRootDelta d = jce_anim_player_consume_root_motion(ai->player);
            if (d.valid) {
                sa->rm_dx    = d.translation.x;
                sa->rm_dy    = d.translation.y;
                sa->rm_dz    = d.translation.z;
                sa->rm_dyaw  = d.yaw_delta;
                sa->rm_valid = true;
            }
        }
    }

    /* Pass 4 (serial): apply authored IK constraints on top of the sampled
     * palettes, before the shadow/color passes consume them. */
    sr_apply_ik_constraints(sr, scene, list);

    /* Pass 4b (serial): ground-adaptive Foot IK (no-op without a ground-query
     * hook).  Pass 4d (serial): coordinated Full-Body IK (no-op without an
     * enabled component + resolvable effectors).  Both run AFTER the constraint
     * pass and mutate the same sampled palettes the shadow/color passes read. */
    sr_apply_foot_ik(sr, scene, list);
    sr_apply_full_body_ik(sr, scene, list);

    /* Pass 5 (serial): resolve per-instance morph (blendshape) weights =
     * track ⊕ authored static JceMorphWeights (FEATURE 3.1 last-mile).  No-op
     * for models without morph targets / entities without authored weights or
     * a track, so legacy playback is byte-identical. */
    sr_resolve_morph_weights(sr, scene, list);

    /* Pass 6 (serial): apply the runtime-published ragdoll pose ON TOP of the
     * sampled + IK'd palette (ragdoll scene-pass last-mile).  Gated on
     * jce_scene_has_ragdoll_pose -> byte-identical for every non-ragdoll
     * entity.  Applied LAST so a live ragdoll wins over clip + IK. */
    sr_apply_ragdoll_override(sr, scene, list);
}
