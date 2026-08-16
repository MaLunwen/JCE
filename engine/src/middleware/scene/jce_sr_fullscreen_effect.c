#include "jce_sr_internal.h"

typedef struct SrFullscreenItem {
    JceEntity entity;
    JceSceneFullscreenEffect effect;
    JceFullscreenEffectSortKey key;
} SrFullscreenItem;

typedef struct SrFullscreenCollect {
    SrFullscreenItem items[JCE_SR_FULLSCREEN_EFFECT_CAP];
    uint32_t count;
    uint32_t active_count;
    uint32_t dropped_count;
    JceEntity dropped_required_entity;
    int component_id;
    uint32_t insertion;
} SrFullscreenCollect;

typedef struct SrFullscreenPresence {
    int component_id;
    bool found;
} SrFullscreenPresence;

static int sr_fullscreen_viewport_slot(int viewport_id)
{
    if (viewport_id < 0 || viewport_id >= JCE_SR_VIEWPORT_SLOTS)
        return 0;
    return viewport_id;
}

static void sr_fullscreen_copy(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;

    if (!dst || dst_size == 0)
        return;
    if (src) {
        while (i + 1 < dst_size && src[i]) {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
}

static void sr_fullscreen_status_reset(
    JceSceneFullscreenEffectStageStatus *status)
{
    if (!status)
        return;
    memset(status, 0, sizeof(*status));
    status->struct_size = sizeof(*status);
    status->pass.struct_size = sizeof(status->pass);
    status->pass.result = JCE_FULLSCREEN_EFFECT_RESULT_BYPASSED;
}

static bool sr_fullscreen_entity_active(JceScene *scene, JceEntity entity,
                                        int component_id,
                                        JceSceneFullscreenEffect **out_effect)
{
    JceSceneFullscreenEffect *effect;

    if (!entity_enabled(scene, entity))
        return false;
    if (component_id >= 0 &&
        !jce_scene_comp_enabled(scene, entity, component_id))
        return false;
    effect = jce_scene_get_fullscreen_effect(scene, entity);
    if (!effect || !effect->enabled)
        return false;
    if (out_effect)
        *out_effect = effect;
    return true;
}

static void sr_fullscreen_presence_cb(JceScene *scene, JceEntity entity,
                                      void *userdata)
{
    SrFullscreenPresence *ctx = (SrFullscreenPresence *)userdata;

    if (!ctx || ctx->found)
        return;
    ctx->found = sr_fullscreen_entity_active(scene, entity,
                                              ctx->component_id, NULL);
}

bool jce_scene_renderer_has_fullscreen_effect(JceScene *scene)
{
    SrFullscreenPresence ctx;

    if (!scene)
        return false;
    ctx.component_id = jce_component_find("FullscreenEffect");
    ctx.found = false;
    jce_scene_each_fullscreen_effect(scene, sr_fullscreen_presence_cb, &ctx);
    return ctx.found;
}

static void sr_fullscreen_collect_insert(SrFullscreenCollect *ctx,
                                         const SrFullscreenItem *item)
{
    uint32_t pos = 0;

    while (pos < ctx->count &&
           jce_fullscreen_effect_sort_key_compare(&ctx->items[pos].key,
                                                  &item->key) <= 0)
        ++pos;

    if (ctx->count < JCE_SR_FULLSCREEN_EFFECT_CAP) {
        uint32_t i;
        for (i = ctx->count; i > pos; --i)
            ctx->items[i] = ctx->items[i - 1];
        ctx->items[pos] = *item;
        ++ctx->count;
        return;
    }

    ++ctx->dropped_count;
    if (pos >= JCE_SR_FULLSCREEN_EFFECT_CAP) {
        if (item->effect.required &&
            ctx->dropped_required_entity == JCE_ENTITY_INVALID)
            ctx->dropped_required_entity = item->entity;
        return;
    }

    if (ctx->items[JCE_SR_FULLSCREEN_EFFECT_CAP - 1].effect.required &&
        ctx->dropped_required_entity == JCE_ENTITY_INVALID)
        ctx->dropped_required_entity =
            ctx->items[JCE_SR_FULLSCREEN_EFFECT_CAP - 1].entity;
    for (uint32_t i = JCE_SR_FULLSCREEN_EFFECT_CAP - 1; i > pos; --i)
        ctx->items[i] = ctx->items[i - 1];
    ctx->items[pos] = *item;
}

static void sr_fullscreen_collect_cb(JceScene *scene, JceEntity entity,
                                     void *userdata)
{
    SrFullscreenCollect *ctx = (SrFullscreenCollect *)userdata;
    JceSceneFullscreenEffect *effect = NULL;
    SrFullscreenItem item;

    if (!ctx || !sr_fullscreen_entity_active(scene, entity,
                                              ctx->component_id, &effect))
        return;
    if (effect->insertion != ctx->insertion)
        return;
    if (ctx->active_count != UINT32_MAX)
        ++ctx->active_count;
    memset(&item, 0, sizeof(item));
    item.entity = entity;
    item.effect = *effect;
    item.key.insertion = effect->insertion;
    item.key.order = effect->order;
    item.key.entity_id = (uint64_t)entity;
    sr_fullscreen_collect_insert(ctx, &item);
}

static void sr_fullscreen_collect(JceScene *scene, uint32_t insertion,
                                  SrFullscreenCollect *out)
{
    memset(out, 0, sizeof(*out));
    out->component_id = jce_component_find("FullscreenEffect");
    out->insertion = insertion;
    out->dropped_required_entity = JCE_ENTITY_INVALID;
    jce_scene_each_fullscreen_effect(scene, sr_fullscreen_collect_cb, out);
}

static JceFullscreenEffectPassDesc sr_fullscreen_pass_desc(
    JceSceneRenderer *sr, const JceSceneFullscreenEffect *effect)
{
    JceFullscreenEffectPassDesc pass =
        jce_fullscreen_effect_pass_desc_default();

    pass.enabled = effect->enabled;
    pass.required = effect->required;
    pass.use_scene_color = effect->use_scene_color;
    pass.use_scene_depth = effect->use_scene_depth;
    pass.use_history = effect->use_history;
    pass.texture_count = effect->texture_count;
    pass.order = effect->order;
    pass.insertion = effect->insertion;
    pass.blend = effect->blend;
    pass.output_format = effect->output_format;
    pass.resolution_scale = effect->resolution_scale;
    sr_fullscreen_copy(pass.shader, sizeof(pass.shader), effect->shader);
    memcpy(pass.samplers, effect->samplers, sizeof(pass.samplers));
    memcpy(pass.params, effect->params, sizeof(pass.params));
    for (uint32_t i = 0; i < pass.texture_count &&
                         i < JCE_FULLSCREEN_EFFECT_MAX_TEXTURES; ++i) {
        JceTexture texture = sr_resolve_texture(sr, effect->textures[i]);
        pass.textures[i].idx = texture.idx;
    }
    return pass;
}

static bool sr_fullscreen_item_active(const SrFullscreenCollect *collect,
                                      JceEntity entity)
{
    for (uint32_t i = 0; i < collect->count; ++i) {
        if (collect->items[i].entity == entity)
            return true;
    }
    return false;
}

static SrFullscreenEffectSlot *sr_fullscreen_get_slot(
    JceSceneRenderer *sr, int viewport_slot, JceEntity entity,
    const SrFullscreenCollect *collect)
{
    SrFullscreenEffectSlot *slots = sr->fullscreen_effects[viewport_slot];
    int reusable = -1;

    for (uint32_t i = 0; i < JCE_SR_FULLSCREEN_EFFECT_CAP; ++i) {
        if (slots[i].entity == entity)
            return &slots[i];
        if (reusable < 0 && slots[i].entity == JCE_ENTITY_INVALID)
            reusable = (int)i;
    }
    if (reusable < 0) {
        for (uint32_t i = 0; i < JCE_SR_FULLSCREEN_EFFECT_CAP; ++i) {
            if (!sr_fullscreen_item_active(collect, slots[i].entity)) {
                reusable = (int)i;
                break;
            }
        }
    }
    if (reusable < 0)
        return NULL;
    if (slots[reusable].pipeline)
        jce_fullscreen_effect_reset_history(slots[reusable].pipeline);
    slots[reusable].entity = entity;
    return &slots[reusable];
}

static bool sr_fullscreen_required_conflict(
    const SrFullscreenCollect *collect, JceEntity *out_entity)
{
    for (uint32_t i = 1; i < collect->count; ++i) {
        JceFullscreenEffectPassDesc a =
            jce_fullscreen_effect_pass_desc_default();
        JceFullscreenEffectPassDesc b = a;
        const JceSceneFullscreenEffect *ca = &collect->items[i - 1].effect;
        const JceSceneFullscreenEffect *cb = &collect->items[i].effect;

        a.enabled = ca->enabled;
        a.required = ca->required;
        a.insertion = ca->insertion;
        a.order = ca->order;
        a.blend = ca->blend;
        b.enabled = cb->enabled;
        b.required = cb->required;
        b.insertion = cb->insertion;
        b.order = cb->order;
        b.blend = cb->blend;
        if (jce_fullscreen_effect_required_replace_conflict(&a, &b)) {
            if (out_entity)
                *out_entity = collect->items[i].entity;
            return true;
        }
    }
    return false;
}

static void sr_fullscreen_fail_stage(
    JceSceneFullscreenEffectStageStatus *status,
    JceFullscreenEffectError error, JceEntity entity)
{
    status->required_failure = true;
    status->failed_entity = (uint64_t)entity;
    status->pass.result = JCE_FULLSCREEN_EFFECT_RESULT_REQUIRED_FAILED;
    status->pass.error = error;
}

static JceFullscreenEffectFrameDesc sr_fullscreen_frame_desc(
    JceSceneRenderer *sr, JceScene *scene, const JceCamera *camera,
    JceEntity entity, JceTextureHandle color, JceTextureHandle depth,
    uint32_t width, uint32_t height, uint16_t view_id, int viewport_slot,
    float dt_sec, uint32_t frame_index)
{
    JceFullscreenEffectFrameDesc frame =
        jce_fullscreen_effect_frame_desc_default();
    const float aspect = height ? (float)width / (float)height : 1.0f;
    jce_mat4 view = camera ? jce_camera_view(camera) : jce_m4_identity();
    jce_mat4 proj = camera
        ? jce_camera_proj(camera, aspect, sr->homogeneous_depth)
        : jce_m4_identity();

    frame.width = width;
    frame.height = height;
    frame.view_id = view_id;
    frame.elapsed_sec = sr->fullscreen_elapsed[viewport_slot];
    frame.delta_sec = dt_sec;
    frame.frame_index = frame_index;
    frame.history_key = (uint64_t)entity;
    frame.scene_color = color;
    frame.scene_depth = depth;
    frame.view_proj = jce_m4_multiply(&proj, &view);
    frame.inv_view_proj = jce_m4_inverse(&frame.view_proj);
    frame.prev_view_proj = sr->fullscreen_prev_vp_valid[viewport_slot]
        ? sr->fullscreen_prev_vp[viewport_slot] : frame.view_proj;
    frame.effect_world = jce_scene_get_world_matrix(scene, entity);
    frame.effect_world_inv = jce_m4_inverse(&frame.effect_world);
    frame.aspect_ratio = aspect;
    if (camera) {
        frame.camera_position = jce_camera_get_position(camera);
        frame.camera_right = jce_camera_get_right(camera);
        frame.camera_up = jce_camera_get_up(camera);
        frame.camera_forward = jce_camera_get_forward(camera);
        if (jce_camera_get_mode(camera) == JCE_CAMERA_ORTHO) {
            const float sy = fabsf(proj.raw[1][1]);
            frame.projection_kind = 1.0f;
            frame.ortho_half_height = sy > 1.0e-8f ? 1.0f / sy : 1.0f;
        } else {
            frame.tan_half_vertical_fov = tanf(
                jce_camera_get_fov(camera) * 0.008726646259971648f);
        }
    }
    return frame;
}

JceTextureHandle jce_scene_renderer_apply_fullscreen_effects(
    JceSceneRenderer *sr, JceScene *scene, const JceCamera *camera,
    JceTextureHandle scene_color, JceTextureHandle scene_depth,
    uint32_t width, uint32_t height, uint16_t view_id_base,
    int viewport_id, uint32_t insertion, float dt_sec)
{
    SrFullscreenCollect collect;
    JceTextureHandle current = scene_color;
    const JceTextureHandle original = scene_color;
    int viewport_slot;
    uint32_t frame_index;
    JceSceneFullscreenEffectStageStatus *stage;
    JceEntity conflict_entity = JCE_ENTITY_INVALID;
    bool recorded_failure = false;

    if (!sr || !scene || width == 0 || height == 0)
        return scene_color;
    viewport_slot = sr_fullscreen_viewport_slot(viewport_id);
    stage = &sr->fullscreen_status[viewport_slot];
    sr_fullscreen_status_reset(stage);
    sr_fullscreen_collect(scene, insertion, &collect);
    stage->active_count = collect.active_count;
    stage->dropped_count = collect.dropped_count;
    if (collect.count == 0)
        return scene_color;
    if (collect.dropped_required_entity != JCE_ENTITY_INVALID) {
        sr_fullscreen_fail_stage(stage,
            JCE_FULLSCREEN_EFFECT_ERROR_PASS_CAPACITY,
            collect.dropped_required_entity);
        return original;
    }
    if (sr_fullscreen_required_conflict(&collect, &conflict_entity)) {
        sr_fullscreen_fail_stage(stage,
            JCE_FULLSCREEN_EFFECT_ERROR_REQUIRED_CONFLICT,
            conflict_entity);
        return original;
    }

    frame_index = jce_renderer_get_frame_index(sr->renderer);
    if (!sr->fullscreen_frame_valid[viewport_slot] ||
        sr->fullscreen_last_frame[viewport_slot] != frame_index) {
        if (!isfinite(dt_sec) || dt_sec < 0.0f)
            dt_sec = 0.0f;
        sr->fullscreen_elapsed[viewport_slot] += dt_sec;
        sr->fullscreen_last_frame[viewport_slot] = frame_index;
        sr->fullscreen_frame_valid[viewport_slot] = true;
    } else {
        dt_sec = 0.0f;
    }

    for (uint32_t i = 0; i < collect.count; ++i) {
        const SrFullscreenItem *item = &collect.items[i];
        SrFullscreenEffectSlot *slot = sr_fullscreen_get_slot(
            sr, viewport_slot, item->entity, &collect);
        JceFullscreenEffectPassDesc pass;
        JceFullscreenEffectFrameDesc frame;
        JceFullscreenEffectStatus pass_status;

        if (!slot) {
            if (item->effect.required) {
                sr_fullscreen_fail_stage(stage,
                    JCE_FULLSCREEN_EFFECT_ERROR_PASS_CAPACITY, item->entity);
                return original;
            }
            ++stage->dropped_count;
            continue;
        }
        if (!slot->pipeline) {
            slot->pipeline = jce_fullscreen_effect_create(
                jce_allocator_default(), sr->pak);
            if (!slot->pipeline) {
                if (item->effect.required) {
                    sr_fullscreen_fail_stage(stage,
                        JCE_FULLSCREEN_EFFECT_ERROR_TARGET_ALLOCATION,
                        item->entity);
                    return original;
                }
                ++stage->dropped_count;
                continue;
            }
            jce_fullscreen_effect_set_shader_dev_dir(
                slot->pipeline, sr->fullscreen_shader_dir);
        }

        pass = sr_fullscreen_pass_desc(sr, &item->effect);
        frame = sr_fullscreen_frame_desc(sr, scene, camera, item->entity,
            current, scene_depth, width, height,
            (uint16_t)(view_id_base + i), viewport_slot, dt_sec, frame_index);
        current = jce_fullscreen_effect_apply(slot->pipeline, &pass, &frame);
        jce_fullscreen_effect_get_status(slot->pipeline, &pass_status);
        if (pass_status.result == JCE_FULLSCREEN_EFFECT_RESULT_APPLIED) {
            ++stage->applied_count;
            if (!recorded_failure)
                stage->pass = pass_status;
        } else if (pass_status.result ==
                   JCE_FULLSCREEN_EFFECT_RESULT_REQUIRED_FAILED) {
            stage->pass = pass_status;
            stage->required_failure = true;
            stage->failed_entity = (uint64_t)item->entity;
            return original;
        } else if (pass_status.result ==
                   JCE_FULLSCREEN_EFFECT_RESULT_OPTIONAL_FAILED) {
            if (!recorded_failure) {
                stage->pass = pass_status;
                stage->failed_entity = (uint64_t)item->entity;
                recorded_failure = true;
            }
        }
    }

    if (camera) {
        const float aspect = (float)width / (float)height;
        jce_mat4 view = jce_camera_view(camera);
        jce_mat4 proj = jce_camera_proj(camera, aspect,
                                        sr->homogeneous_depth);
        sr->fullscreen_prev_vp[viewport_slot] =
            jce_m4_multiply(&proj, &view);
        sr->fullscreen_prev_vp_valid[viewport_slot] = true;
    }
    return current;
}

void jce_scene_renderer_get_fullscreen_effect_status(
    const JceSceneRenderer *sr, int viewport_id,
    JceSceneFullscreenEffectStageStatus *out_status)
{
    int viewport_slot;

    if (!out_status)
        return;
    sr_fullscreen_status_reset(out_status);
    if (!sr)
        return;
    viewport_slot = sr_fullscreen_viewport_slot(viewport_id);
    if (sr->fullscreen_status[viewport_slot].struct_size ==
        sizeof(sr->fullscreen_status[viewport_slot]))
        *out_status = sr->fullscreen_status[viewport_slot];
}

void jce_scene_renderer_set_project_shader_dir(
    JceSceneRenderer *sr, const char *directory)
{
    char next[sizeof(sr->fullscreen_shader_dir)];

    if (!sr)
        return;
    sr_fullscreen_copy(next, sizeof(next), directory);
    if (strcmp(next, sr->fullscreen_shader_dir) == 0)
        return;
    sr_fullscreen_copy(sr->fullscreen_shader_dir,
                       sizeof(sr->fullscreen_shader_dir), next);
    for (uint32_t v = 0; v < JCE_SR_VIEWPORT_SLOTS; ++v) {
        for (uint32_t i = 0; i < JCE_SR_FULLSCREEN_EFFECT_CAP; ++i) {
            JceFullscreenEffectPipeline *pipeline =
                sr->fullscreen_effects[v][i].pipeline;
            if (pipeline)
                jce_fullscreen_effect_set_shader_dev_dir(pipeline, next);
        }
    }
}

void sr_fullscreen_effect_reset_all(JceSceneRenderer *sr)
{
    if (!sr)
        return;
    for (uint32_t v = 0; v < JCE_SR_VIEWPORT_SLOTS; ++v) {
        for (uint32_t i = 0; i < JCE_SR_FULLSCREEN_EFFECT_CAP; ++i) {
            SrFullscreenEffectSlot *slot = &sr->fullscreen_effects[v][i];
            if (slot->pipeline)
                jce_fullscreen_effect_reset_history(slot->pipeline);
            slot->entity = JCE_ENTITY_INVALID;
        }
        sr_fullscreen_status_reset(&sr->fullscreen_status[v]);
        sr->fullscreen_prev_vp_valid[v] = false;
        sr->fullscreen_elapsed[v] = 0.0f;
        sr->fullscreen_frame_valid[v] = false;
    }
}

void sr_fullscreen_effect_destroy_all(JceSceneRenderer *sr)
{
    if (!sr)
        return;
    for (uint32_t v = 0; v < JCE_SR_VIEWPORT_SLOTS; ++v) {
        for (uint32_t i = 0; i < JCE_SR_FULLSCREEN_EFFECT_CAP; ++i) {
            SrFullscreenEffectSlot *slot = &sr->fullscreen_effects[v][i];
            jce_fullscreen_effect_destroy(slot->pipeline);
            slot->pipeline = NULL;
            slot->entity = JCE_ENTITY_INVALID;
        }
    }
}
