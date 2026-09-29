/*
 * jce_panel_terrain.cpp -- Terrain authoring panel.
 *
 * Hosts a heightmap + 4-layer splat painter on top of jce_terrain.
 * Two modes: Sculpt (Raise / Lower / Smooth / Flatten brushes
 * driving the heightmap) and Splat (paint a chosen texture layer
 * up at the expense of the others).  Brush strokes apply at the
 * current cursor world XZ; for the panel-only flow we expose
 * "stamp at point" so the user can author without a full
 * scene-view raycast plumbing yet.
 *
 * IO: New / Load / Save buttons round-trip a .terrain.json meta file
 * (binary heightmap + splat live in a side-car .terrain.bin).
 */

#include "io/jce_editor_file_util.h"
#include "ui/jce_editor_dnd.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_project_state.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_ui_state.h"
#include "core/jce_editor_state.h"
#include "scene/jce_editor_scene_render.h"

#include <jce/tools/jce_imgui.hpp>
#include <panels/jce_terrain_history.h>
#include "dialogs/jce_path_input.h"
#include "core/jce_assetdb.h"
extern "C" {
#include <jce/middleware/scene/jce_terrain.h>
#include <jce/renderer/jce_image.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_rand.h>

void jce_foliage_brush_set_armed(bool armed);
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

enum class ToolMode { Sculpt, Splat, Holes };

struct PanelState {
    JceTerrain *terrain = nullptr;
    JceScene   *terrain_scene = nullptr;
    bool        scene_owns_terrain = false;
    char        terrain_scene_path[512] = "";

    /* Authoring state. */
    ToolMode tool         = ToolMode::Sculpt;
    int      sculpt_mode  = JCE_TERRAIN_SCULPT_RAISE;
    int      splat_layer  = 0;
    bool     hole_erase   = false;   /* Holes tool: false = cut cells, true = fill back */
    float    brush_radius = 4.0f;
    float    brush_strength = 4.0f;
    float    brush_rotation = 0.0f;
    float    brush_hardness = 0.0f;
    float    brush_spacing = 0.25f;
    float    brush_scatter = 0.0f;
    float    brush_rotation_jitter = 0.0f;
    char     brush_mask_path[512] = "";
    std::vector<float> brush_mask;
    int      brush_mask_w = 0;
    int      brush_mask_h = 0;

    /* Continuous-stroke state.  Input points remain unscattered so the
     * path sampler cannot drift; scatter is applied only to emitted stamps. */
    bool     stroke_has_point = false;
    float    stroke_prev_x = 0.0f;
    float    stroke_prev_z = 0.0f;
    float    stroke_flatten_target = 0.0f;
    uint64_t stroke_serial = 0;
    JceRng   stroke_rng = {};

    /* Cursor sample point in world XZ. */
    float    cursor_x    = 0.0f;
    float    cursor_z    = 0.0f;

    /* Create-new dialog inputs. */
    int      new_w = 257;
    int      new_h = 257;
    float    new_size_x = 100.0f;
    float    new_size_z = 100.0f;
    float    new_max_h  = 20.0f;
    int      new_chunk  = 32;

    /* IO. */
    char     io_path[512] = "scenes/terrain.terrain.json";

    /* Heightmap image / RAW import + r16 export path. */
    char     heightmap_path[512] = "scenes/heightmap.png";

    /* Last status line. */
    std::string status;

    /* When true, mouse clicks/drags in Scene View raycast against
     * the active terrain and apply the brush at the hit XZ. */
    bool     paint_in_scene = false;

    /* Cached preview texture (greyscale heightmap as RGBA8). */
    std::vector<uint32_t> preview_pixels;
    int preview_w = 0;
    int preview_h = 0;
    bool preview_dirty = true;

    /* ── Generators (cook-time passes over the whole grid) ────────────
     * These were implemented and unit-tested with no way for an artist to
     * reach them, which makes them shelf-ware however correct they are.
     * They are whole-grid operations, so they live here rather than on the
     * brush: a brush edits where you drag, a generator rewrites the map. */
    unsigned gen_seed       = 1337u;
    int      gen_octaves    = 0;        /* 0 = the filter's tuned default */
    float    gen_strength   = 0.0f;     /* 0 = default                    */
    float    gen_detail     = 0.0f;     /* 0 = default                    */
    float    talus_angle    = 35.0f;
    int      talus_iters    = 8;
    float    talus_strength = 0.5f;
    int      occl_dirs      = 16;
    /* Sky-occlusion output, kept so the artist can see it took effect and a
     * later cook can consume it without recomputing. */
    std::vector<float> sky_visibility;
    std::vector<float> ridge_field;
} s;

const int kBrushMaskMaxDimension = 2048;
const int kBrushMaxStampsPerFrame = 256;
JceTerrainHistory s_terrain_history;
bool s_stroke_open = false;
bool s_stroke_changed = false;
bool s_stroke_region_valid = false;
float s_stroke_min_x = 0.0f;
float s_stroke_min_z = 0.0f;
float s_stroke_max_x = 0.0f;
float s_stroke_max_z = 0.0f;
uint32_t s_stroke_edit_flags = 0u;

void clear_undo_history(void);

struct TerrainBindingSearch {
    const char *requested = nullptr;
    char        requested_abs[1024] = "";
    JceEntity   entity = 0;
    char        terrain_path[512] = "";
};

void find_terrain_binding_cb(JceScene *scene, JceEntity entity, void *user_data)
{
    TerrainBindingSearch *search =
        static_cast<TerrainBindingSearch *>(user_data);
    if (!search || search->entity || !jce_scene_has_terrain(scene, entity))
        return;
    JceTerrainComponent *component = jce_scene_get_terrain(scene, entity);
    if (!component || !component->terrain_path[0]) return;

    bool matches = search->requested &&
                   std::strcmp(component->terrain_path, search->requested) == 0;
    if (!matches && search->requested_abs[0]) {
        char component_abs[1024];
        matches = jce_editor_resolve_asset_path(component->terrain_path,
                                                 component_abs,
                                                 sizeof(component_abs)) &&
                  std::strcmp(component_abs, search->requested_abs) == 0;
    }
    if (!matches) return;

    search->entity = entity;
    std::snprintf(search->terrain_path, sizeof(search->terrain_path), "%s",
                  component->terrain_path);
}

bool find_terrain_binding(JceScene *scene, JceEntity *out_entity,
                          char *out_path, size_t out_path_size)
{
    if (!scene || !s.io_path[0]) return false;
    TerrainBindingSearch search;
    search.requested = s.scene_owns_terrain && s.terrain_scene_path[0]
        ? s.terrain_scene_path : s.io_path;
    (void)jce_editor_resolve_asset_path(search.requested,
                                        search.requested_abs,
                                        sizeof(search.requested_abs));
    jce_scene_each_entity(scene, find_terrain_binding_cb, &search);
    if (!search.entity) return false;
    if (out_entity) *out_entity = search.entity;
    if (out_path && out_path_size > 0)
        std::snprintf(out_path, out_path_size, "%s", search.terrain_path);
    return true;
}

void validate_scene_owned_terrain(void)
{
    if (!s.scene_owns_terrain) return;
    JceScene *scene = jce_state_get_scene();
    if (scene &&
        jce_scene_peek_terrain(scene, s.terrain_scene_path) == s.terrain) {
        s.terrain_scene = scene;
        return;
    }

    s.terrain = nullptr;
    s.terrain_scene = nullptr;
    s.scene_owns_terrain = false;
    s.terrain_scene_path[0] = '\0';
    clear_undo_history();
}

void release_panel_terrain(void)
{
    validate_scene_owned_terrain();
    if (s.terrain && !s.scene_owns_terrain)
        jce_terrain_free(s.terrain);
    s.terrain = nullptr;
    s.terrain_scene = nullptr;
    s.scene_owns_terrain = false;
    s.terrain_scene_path[0] = '\0';
}

bool publish_terrain_to_scene(void)
{
    validate_scene_owned_terrain();
    if (!s.terrain || s.scene_owns_terrain) return s.scene_owns_terrain;

    JceScene *scene = jce_state_get_scene();
    char canonical_path[512];
    if (!find_terrain_binding(scene, nullptr, canonical_path,
                              sizeof(canonical_path)))
        return false;
    if (!jce_scene_adopt_terrain(scene, canonical_path, s.terrain))
        return false;

    s.terrain_scene = scene;
    s.scene_owns_terrain = true;
    std::snprintf(s.terrain_scene_path, sizeof(s.terrain_scene_path), "%s",
                  canonical_path);
    if (JceSceneRenderer *renderer = jce_editor_get_scene_renderer())
        jce_scene_renderer_invalidate_terrain(renderer, canonical_path);
    return true;
}

void notify_terrain_changed_full(void)
{
    if (!publish_terrain_to_scene()) return;
    (void)jce_scene_touch_terrain(s.terrain_scene, s.terrain_scene_path);
}

void notify_terrain_changed_region(float min_x, float min_z,
                                   float max_x, float max_z,
                                   uint32_t edit_flags)
{
    if (!publish_terrain_to_scene()) return;
    if (JceSceneRenderer *renderer = jce_editor_get_scene_renderer())
        jce_scene_renderer_invalidate_terrain_region(
            renderer, s.terrain_scene, s.terrain_scene_path,
            min_x, min_z, max_x, max_z, edit_flags);
}

void accumulate_stroke_region(float min_x, float min_z,
                              float max_x, float max_z,
                              uint32_t edit_flags)
{
    if (!s_stroke_region_valid) {
        s_stroke_min_x = min_x;
        s_stroke_min_z = min_z;
        s_stroke_max_x = max_x;
        s_stroke_max_z = max_z;
        s_stroke_region_valid = true;
    } else {
        s_stroke_min_x = std::min(s_stroke_min_x, min_x);
        s_stroke_min_z = std::min(s_stroke_min_z, min_z);
        s_stroke_max_x = std::max(s_stroke_max_x, max_x);
        s_stroke_max_z = std::max(s_stroke_max_z, max_z);
    }
    s_stroke_edit_flags |= edit_flags;
}

bool push_undo(uint32_t flags)
{
    s_terrain_history.attach(s.terrain);
    if (!s_terrain_history.begin_edit()) return false;
    return s_terrain_history.capture_region(
        0.0f, 0.0f, jce_terrain_world_size_x(s.terrain),
        jce_terrain_world_size_z(s.terrain), flags);
}

bool push_undo_region(float min_x, float min_z,
                      float max_x, float max_z, uint32_t flags)
{
    s_terrain_history.attach(s.terrain);
    if (!s_terrain_history.begin_edit()) return false;
    const bool captured = s_terrain_history.capture_region(
        min_x, min_z, max_x, max_z, flags);
    return captured;
}

void drop_last_undo(void)
{
    s_terrain_history.cancel_edit();
}

bool commit_terrain_history(void)
{
    if (!s_terrain_history.commit_edit()) return false;
    const uint64_t sequence =
        jce_state_history_commit_external(&s_terrain_history);
    if (s_terrain_history.set_latest_undo_sequence(sequence)) return true;
    s_terrain_history.discard_last_undo();
    return false;
}

void apply_history_change(const JceTerrainHistoryChange &change)
{
    if (change.edit_flags & JCE_TERRAIN_EDIT_HEIGHTS)
        s.preview_dirty = true;
    notify_terrain_changed_full();
    notify_terrain_changed_region(
        change.min_x, change.min_z, change.max_x, change.max_z,
        change.edit_flags);
}

/* Begin a brush transaction. Tiles are captured lazily before each stamp. */
void begin_stroke(float wx, float wz)
{
    if (s_stroke_open) return;
    s_terrain_history.attach(s.terrain);
    (void)s_terrain_history.begin_edit();
    s_stroke_open = true;
    s_stroke_changed = false;
    s_stroke_region_valid = false;
    s_stroke_edit_flags = 0u;
    s.stroke_has_point = false;
    s.stroke_flatten_target = jce_terrain_sample_height(s.terrain, wx, wz);
    jce_rng_seed(&s.stroke_rng, 0x7465727261696eULL ^ ++s.stroke_serial,
                 0x6272757368ULL);
}

void end_stroke(void)
{
    if (s_stroke_open && s_stroke_changed) {
        (void)commit_terrain_history();
        notify_terrain_changed_full();
        if (s_stroke_region_valid)
            notify_terrain_changed_region(
                s_stroke_min_x, s_stroke_min_z,
                s_stroke_max_x, s_stroke_max_z,
                s_stroke_edit_flags);
    } else {
        s_terrain_history.cancel_edit();
    }
    s_stroke_open = false;
    s_stroke_changed = false;
    s_stroke_region_valid = false;
    s_stroke_edit_flags = 0u;
    s.stroke_has_point = false;
}

void clear_undo_history(void)
{
    s_terrain_history.attach(s.terrain);
    s_terrain_history.clear();
    s_stroke_open = false;
    s_stroke_changed = false;
    s_stroke_region_valid = false;
    s_stroke_edit_flags = 0u;
    s.stroke_has_point = false;
}

bool terrain_history_undo(void)
{
    JceTerrainHistoryChange change;
    if (!s_terrain_history.undo(&change)) return false;
    apply_history_change(change);
    return true;
}

bool terrain_history_redo(void)
{
    JceTerrainHistoryChange change;
    if (!s_terrain_history.redo(&change)) return false;
    apply_history_change(change);
    return true;
}

uint64_t terrain_history_peek_undo(void *user)
{
    return static_cast<JceTerrainHistory *>(user)->undo_sequence();
}

uint64_t terrain_history_peek_redo(void *user)
{
    return static_cast<JceTerrainHistory *>(user)->redo_sequence();
}

bool terrain_history_apply_undo(void *user)
{
    (void)user;
    validate_scene_owned_terrain();
    return terrain_history_undo();
}

bool terrain_history_apply_redo(void *user)
{
    (void)user;
    validate_scene_owned_terrain();
    return terrain_history_redo();
}

void terrain_history_clear(void *user)
{
    (void)user;
    clear_undo_history();
}

void terrain_history_clear_redo(void *user)
{
    static_cast<JceTerrainHistory *>(user)->clear_redo();
}

void ensure_history_provider_registered(void)
{
    static bool registered = false;
    if (registered) return;
    const JceEditorHistoryProvider provider = {
        &s_terrain_history,
        terrain_history_peek_undo,
        terrain_history_peek_redo,
        terrain_history_apply_undo,
        terrain_history_apply_redo,
        terrain_history_clear,
        terrain_history_clear_redo,
    };
    registered = jce_state_history_register_provider(&provider);
}

void set_brush_mask_status(const char *key, const char *path, int w, int h)
{
    char message[1200];
    if (w > 0 && h > 0)
        snprintf(message, sizeof(message), jce_editor_i18n(key), w, h, path);
    else
        snprintf(message, sizeof(message), jce_editor_i18n(key), path);
    s.status = message;
}

bool load_brush_mask(bool report_status)
{
    if (s.brush_mask_path[0] == '\0') return false;

    char resolved[1024];
    const char *load_path = s.brush_mask_path;
    if (jce_editor_resolve_asset_path(s.brush_mask_path, resolved,
                                      sizeof(resolved)))
        load_path = resolved;

    uint64_t byte_count = 0;
    void *bytes = jce_fs_host_read_all(load_path, &byte_count);
    int w = 0;
    int h = 0;
    uint16_t *pixels = bytes
        ? jce_image_load_gray16_from_memory(bytes, byte_count, &w, &h)
        : nullptr;
    jce_fs_buffer_free(bytes);

    if (!pixels || w <= 0 || h <= 0 ||
        w > kBrushMaskMaxDimension || h > kBrushMaskMaxDimension) {
        jce_image_free_gray16(pixels);
        if (report_status)
            set_brush_mask_status("terrain.brush.maskLoadFailed",
                                  load_path, 0, 0);
        return false;
    }

    const size_t count = (size_t)w * (size_t)h;
    std::vector<float> mask(count);
    for (size_t i = 0; i < count; ++i)
        mask[i] = (float)pixels[i] / 65535.0f;
    jce_image_free_gray16(pixels);

    s.brush_mask.swap(mask);
    s.brush_mask_w = w;
    s.brush_mask_h = h;
    jce_editor_ui_state_save_str("brush.terrain.mask_path",
                                 s.brush_mask_path);
    if (report_status)
        set_brush_mask_status("terrain.brush.maskLoaded", load_path, w, h);
    return true;
}

void clear_brush_mask(void)
{
    s.brush_mask.clear();
    s.brush_mask_w = 0;
    s.brush_mask_h = 0;
    s.brush_mask_path[0] = '\0';
    jce_editor_ui_state_save_str("brush.terrain.mask_path", "");
    s.status = jce_editor_i18n("terrain.brush.maskCleared");
}

JceTerrainBrushDesc make_brush_desc(float rotation_deg,
                                    bool lock_flatten,
                                    float flatten_target)
{
    JceTerrainBrushDesc desc = jce_terrain_brush_desc_default();
    if (!s.brush_mask.empty()) {
        desc.mask = s.brush_mask.data();
        desc.mask_width = s.brush_mask_w;
        desc.mask_height = s.brush_mask_h;
    }
    desc.rotation_deg = rotation_deg;
    desc.hardness = s.brush_hardness;
    desc.use_flatten_target = lock_flatten;
    desc.flatten_target_world = flatten_target;
    return desc;
}

void apply_brush_stamp(float path_x, float path_z, float dt,
                       bool randomize, bool lock_flatten,
                       float flatten_target)
{
    float wx = path_x;
    float wz = path_z;
    float rotation = s.brush_rotation;
    if (randomize) {
        if (s.brush_scatter > 0.0f) {
            float angle = jce_rng_range_f(&s.stroke_rng, 0.0f, 6.28318530718f);
            float distance = sqrtf(jce_rng_f32(&s.stroke_rng)) *
                             s.brush_scatter * s.brush_radius;
            wx += cosf(angle) * distance;
            wz += sinf(angle) * distance;
        }
        if (s.brush_rotation_jitter > 0.0f)
            rotation += jce_rng_range_f(&s.stroke_rng,
                                        -s.brush_rotation_jitter,
                                         s.brush_rotation_jitter);
    }

    JceTerrainBrushDesc desc = make_brush_desc(rotation, lock_flatten,
                                               flatten_target);
    if (s.tool == ToolMode::Sculpt) {
        jce_terrain_sculpt_apply_brush(
            s.terrain, (JceTerrainSculptMode)s.sculpt_mode, &desc,
            wx, wz, s.brush_radius, s.brush_strength, dt);
    } else if (s.tool == ToolMode::Splat) {
        jce_terrain_splat_paint_brush(
            s.terrain, s.splat_layer, &desc,
            wx, wz, s.brush_radius, s.brush_strength, dt);
    } else {
        jce_terrain_hole_apply(s.terrain, wx, wz,
                               s.brush_radius, s.hole_erase);
    }
}

void apply_continuous_stroke(float wx, float wz, float dt)
{
    float dx = s.stroke_has_point ? wx - s.stroke_prev_x : 0.0f;
    float dz = s.stroke_has_point ? wz - s.stroke_prev_z : 0.0f;
    float distance = sqrtf(dx * dx + dz * dz);
    float step = std::max(s.brush_radius * s.brush_spacing, 0.001f);
    int stamp_count = s.stroke_has_point
        ? std::max(1, (int)ceilf(distance / step))
        : 1;
    stamp_count = std::min(stamp_count, kBrushMaxStampsPerFrame);
    float stamp_dt = dt / (float)stamp_count;
    bool lock_flatten = s.tool == ToolMode::Sculpt &&
                        s.sculpt_mode == JCE_TERRAIN_SCULPT_FLATTEN;

    for (int i = 1; i <= stamp_count; ++i) {
        float t = s.stroke_has_point ? (float)i / (float)stamp_count : 1.0f;
        float px = s.stroke_has_point ? s.stroke_prev_x + dx * t : wx;
        float pz = s.stroke_has_point ? s.stroke_prev_z + dz * t : wz;
        apply_brush_stamp(px, pz, stamp_dt, true, lock_flatten,
                          s.stroke_flatten_target);
    }

    s.stroke_prev_x = wx;
    s.stroke_prev_z = wz;
    s.stroke_has_point = true;
}

void rebuild_preview()
{
    s.preview_dirty = false;
    s.preview_pixels.clear();
    s.preview_w = 0;
    s.preview_h = 0;
    if (!s.terrain) return;
    int w = jce_terrain_width(s.terrain);
    int h = jce_terrain_height(s.terrain);
    /* Down-sample to <= 256 px on the long side. */
    int max_side = std::max(w, h);
    int step = std::max(1, (max_side + 255) / 256);
    int pw = (w + step - 1) / step;
    int ph = (h + step - 1) / step;
    s.preview_w = pw;
    s.preview_h = ph;
    s.preview_pixels.resize((size_t)pw * (size_t)ph, 0xFF000000u);
    const float *heights = jce_terrain_heights(s.terrain);
    if (!heights) {
        /* Tiled / procedural: no resident height grid to preview. Leave the
         * cleared image rather than dereferencing NULL -- this runs from
         * draw_preview_section on the frame after Load, and a `"procedural":
         * true` meta reaches it. */
        s.preview_dirty = false;
        return;
    }
    for (int j = 0; j < ph; ++j) {
        for (int i = 0; i < pw; ++i) {
            int x = std::min(i * step, w - 1);
            int z = std::min(j * step, h - 1);
            float v = heights[(size_t)z * w + x];
            uint8_t g = (uint8_t)std::clamp((int)(v * 255.0f), 0, 255);
            s.preview_pixels[(size_t)j * pw + i] =
                0xFF000000u | ((uint32_t)g << 16) | ((uint32_t)g << 8) | (uint32_t)g;
        }
    }
}

void ensure_terrain()
{
    if (s.terrain) return;
    s.terrain = jce_terrain_create(s.new_w, s.new_h,
                                   s.new_size_x, s.new_size_z,
                                   s.new_max_h, s.new_chunk);
    s.preview_dirty = true;
    (void)publish_terrain_to_scene();
}

void draw_toolbar()
{
    /* One-time prefill of the last successfully loaded terrain document
     * (per-project) so one click on Load reopens it.  Never auto-loads,
     * and never clobbers a path the user already typed. */
    static bool s_path_prefilled = false;
    if (!s_path_prefilled && jce_editor_pstate_active()) {
        s_path_prefilled = true;
        if (std::strcmp(s.io_path, "scenes/terrain.terrain.json") == 0)
            jce_editor_pstate_get_str("doc.terrain.last", s.io_path,
                                      sizeof(s.io_path));
    }

    if (ImGui::BeginTable("terrain_io", 4, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Button(jce_editor_i18n("terrain.toolbar.new"), ImVec2(-1, 0))) {
            release_panel_terrain();
            clear_undo_history();
            ensure_terrain();
            s.status = jce_editor_i18n("terrain.status.created");
        }
        ImGui::TableNextColumn();
        if (ImGui::Button(jce_editor_i18n("terrain.toolbar.load"), ImVec2(-1, 0))) {
            release_panel_terrain();
            clear_undo_history();
            char resolved[1024];
            const char *load_path = s.io_path;
            if (jce_editor_resolve_asset_path(s.io_path, resolved, sizeof(resolved)))
                load_path = resolved;
            s.terrain = jce_terrain_load_file(load_path);
            s.preview_dirty = true;
            if (s.terrain) (void)publish_terrain_to_scene();
            s.status = s.terrain
                ? std::string(jce_editor_i18n("terrain.status.loaded")) + load_path
                : std::string(jce_editor_i18n("terrain.status.loadFailed")) + load_path;
            if (s.terrain)   /* remember the last document that loaded OK */
                jce_editor_pstate_set_str("doc.terrain.last", s.io_path);
        }
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(s.terrain == nullptr);
        if (ImGui::Button(jce_editor_i18n("terrain.toolbar.save"), ImVec2(-1, 0))) {
            char resolved[1024];
            const char *save_path = s.io_path;
            if (jce_editor_resolve_asset_path(s.io_path, resolved, sizeof(resolved)))
                save_path = resolved;
            bool ok = jce_terrain_save_file(s.terrain, save_path);
            s.status = ok
                ? std::string(jce_editor_i18n("terrain.status.saved")) + save_path
                : std::string(jce_editor_i18n("terrain.status.saveFailed")) + save_path;
            if (ok) {
                /* The resident grid is already the authoring grid.  Save only
                 * persists it; replacing the cache here would free the panel's
                 * borrowed pointer and reload the same bytes needlessly. */
                (void)publish_terrain_to_scene();
            }
        }
        ImGui::EndDisabled();
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(s.terrain == nullptr);
        if (ImGui::Button(jce_editor_i18n("terrain.toolbar.close"), ImVec2(-1, 0))) {
            release_panel_terrain();
            clear_undo_history();
            s.preview_pixels.clear();
            s.preview_w = s.preview_h = 0;
            s.status = jce_editor_i18n("terrain.status.closed");
        }
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    jce_draw_path_input(jce_editor_i18n("terrain.toolbar.path"), s.io_path, sizeof(s.io_path), JcePathKind::FileAbs);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *payload =
                ImGui::AcceptDragDropPayload(JCE_DND_ASSET_PATH)) {
            const char *path = (const char *)payload->Data;
            char rel[1024];
            const char *store_path = jce_editor_path_relative_or(rel, sizeof(rel), path);
            snprintf(s.io_path, sizeof(s.io_path), "%s", store_path);
            /* Auto-load if a .terrain.json was dropped. */
            const char *ext = strrchr(path, '.');
            if (ext && (strcmp(ext, ".json") == 0 || strstr(path, ".terrain."))) {
                release_panel_terrain();
                clear_undo_history();
                char resolved[1024];
                const char *load_path = s.io_path;
                if (jce_editor_resolve_asset_path(s.io_path, resolved, sizeof(resolved)))
                    load_path = resolved;
                s.terrain = jce_terrain_load_file(load_path);
                s.preview_dirty = true;
                if (s.terrain) (void)publish_terrain_to_scene();
                s.status = s.terrain
                    ? std::string(jce_editor_i18n("terrain.status.loaded")) + load_path
                    : std::string(jce_editor_i18n("terrain.status.loadFailed")) + load_path;
                if (s.terrain)   /* remember the last document that loaded OK */
                    jce_editor_pstate_set_str("doc.terrain.last", s.io_path);
            }
        }
        ImGui::EndDragDropTarget();
    }
}

void draw_create_section()
{
    if (ImGui::CollapsingHeader(jce_editor_i18n("terrain.create.header"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragInt2 (jce_editor_i18n("terrain.create.vertexWH"),  &s.new_w,        1.0f, 2, 4097);
        ImGui::DragFloat2(jce_editor_i18n("terrain.create.worldSize"), &s.new_size_x,  0.5f, 1.0f, 4096.0f);
        ImGui::DragFloat (jce_editor_i18n("terrain.create.maxHeight"), &s.new_max_h,   0.1f, 0.1f, 1024.0f);
        ImGui::DragInt   (jce_editor_i18n("terrain.create.chunkSize"), &s.new_chunk,   1.0f, 4, 256);
        ImGui::TextDisabled("%s", jce_editor_i18n("terrain.create.hint"));
    }
}

void draw_heightmap_io_section()
{
    if (!s.terrain) return;
    if (ImGui::CollapsingHeader(jce_editor_i18n("terrain.heightmap.header"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("%s", jce_editor_i18n("terrain.heightmap.note"));
        jce_draw_path_input(jce_editor_i18n("terrain.heightmap.path"),
                            s.heightmap_path, sizeof(s.heightmap_path),
                            JcePathKind::FileAbs);

        if (ImGui::Button(jce_editor_i18n("terrain.heightmap.import"), ImVec2(-1, 0))) {
            char resolved[1024];
            const char *load_path = s.heightmap_path;
            if (jce_editor_resolve_asset_path(s.heightmap_path, resolved, sizeof(resolved)))
                load_path = resolved;
            /* Import mutates the height grid: snapshot first so it is undoable. */
            const bool captured = push_undo(JCE_TERRAIN_EDIT_HEIGHTS);
            bool ok = jce_terrain_import_heightmap_file(s.terrain, load_path);
            if (ok) {
                if (captured) (void)commit_terrain_history();
                s.preview_dirty = true;
                notify_terrain_changed_full();
            } else if (captured) {
                /* Import failed: drop the undo snapshot we just pushed. */
                drop_last_undo();
            }
            s.status = ok
                ? std::string(jce_editor_i18n("terrain.heightmap.imported")) + load_path
                : std::string(jce_editor_i18n("terrain.heightmap.importFailed")) + load_path;
        }

        if (ImGui::Button(jce_editor_i18n("terrain.heightmap.export"), ImVec2(-1, 0))) {
            char resolved[1024];
            const char *save_path = s.heightmap_path;
            if (jce_editor_resolve_asset_path(s.heightmap_path, resolved, sizeof(resolved)))
                save_path = resolved;
            int w = jce_terrain_width(s.terrain);
            int h = jce_terrain_height(s.terrain);
            size_t n = (size_t)w * (size_t)h;
            bool ok = false;
            if (n > 0) {
                std::vector<uint16_t> r16(n);
                if (jce_terrain_export_heightmap_r16(s.terrain, r16.data(), r16.size())) {
                    ok = jce_fs_host_write_all(save_path, r16.data(),
                                               (uint64_t)(n * sizeof(uint16_t)));
                }
            }
            s.status = ok
                ? std::string(jce_editor_i18n("terrain.heightmap.exported")) + save_path
                : std::string(jce_editor_i18n("terrain.heightmap.exportFailed")) + save_path;
        }
        ImGui::TextDisabled("%s", jce_editor_i18n("terrain.heightmap.exportNote"));
    }
}

void draw_brush_section()
{
    /* One-time restore of the persisted brush knobs (user-global, so the
     * brush feel carries across projects and restarts). */
    static bool s_brush_loaded = false;
    if (!s_brush_loaded) {
        s_brush_loaded = true;
        s.tool           = (ToolMode)jce_editor_ui_state_load_int(
                               "brush.terrain.tool", (int)s.tool, 0, 2);
        s.sculpt_mode    = jce_editor_ui_state_load_int(
                               "brush.terrain.sculpt_mode", s.sculpt_mode, 0, 3);
        s.splat_layer    = jce_editor_ui_state_load_int(
                               "brush.terrain.layer", s.splat_layer, 0, 3);
        s.hole_erase     = jce_editor_ui_state_load_int(
                               "brush.terrain.hole_erase", 0, 0, 1) != 0;
        s.paint_in_scene = jce_editor_ui_state_load_int(
                               "brush.terrain.paint_in_scene", 0, 0, 1) != 0;
        s.brush_radius   = jce_editor_ui_state_load_float(
                               "brush.terrain.radius", s.brush_radius, 0.5f, 64.0f);
        s.brush_strength = jce_editor_ui_state_load_float(
                               "brush.terrain.strength", s.brush_strength, 0.1f, 64.0f);
        s.brush_rotation = jce_editor_ui_state_load_float(
                               "brush.terrain.rotation", 0.0f, -180.0f, 180.0f);
        s.brush_hardness = jce_editor_ui_state_load_float(
                               "brush.terrain.hardness", 0.0f, 0.0f, 1.0f);
        s.brush_spacing  = jce_editor_ui_state_load_float(
                               "brush.terrain.spacing", 0.25f, 0.05f, 1.0f);
        s.brush_scatter  = jce_editor_ui_state_load_float(
                               "brush.terrain.scatter", 0.0f, 0.0f, 1.0f);
        s.brush_rotation_jitter = jce_editor_ui_state_load_float(
                               "brush.terrain.rotation_jitter", 0.0f,
                               0.0f, 180.0f);
        jce_editor_ui_state_load_str("brush.terrain.mask_path",
                                     s.brush_mask_path,
                                     sizeof(s.brush_mask_path), "");
        if (s.brush_mask_path[0] != '\0') load_brush_mask(false);
    }

    if (!s.terrain) {
        ImGui::TextDisabled("%s", jce_editor_i18n("terrain.brush.needTerrain"));
        return;
    }
    if (!jce_terrain_heights(s.terrain)) {
        ImGui::TextDisabled("%s",
                            jce_editor_i18n("terrain.brush.needResidentTerrain"));
        return;
    }
    if (ImGui::CollapsingHeader(jce_editor_i18n("terrain.gen.header"))) {
        ImGui::TextDisabled("%s", jce_editor_i18n("terrain.gen.hint"));

        /* ── Gully erosion ──────────────────────────────────────────── */
        ImGui::SeparatorText(jce_editor_i18n("terrain.gen.gully"));
        int seed_i = (int)s.gen_seed;
        if (ImGui::InputInt(jce_editor_i18n_id("terrain.gen.seed", "##tgenseed"), &seed_i))
            s.gen_seed = (unsigned)(seed_i < 0 ? 0 : seed_i);
        ImGui::SliderInt(jce_editor_i18n("terrain.gen.octaves"), &s.gen_octaves, 0, 8);
        ImGui::SliderFloat(jce_editor_i18n("terrain.gen.strength"), &s.gen_strength, 0.0f, 40.0f);
        ImGui::SliderFloat(jce_editor_i18n("terrain.gen.detail"), &s.gen_detail, 0.0f, 4.0f);
        if (ImGui::Button(jce_editor_i18n("terrain.gen.applyErosion"))) {
            const bool captured = push_undo(JCE_TERRAIN_EDIT_HEIGHTS);
            JceTerrainErosionParams ep;
            memset(&ep, 0, sizeof ep);
            ep.seed      = s.gen_seed;
            ep.octaves   = s.gen_octaves;
            ep.strength  = s.gen_strength;
            ep.detail    = s.gen_detail;
            /* Capture the ridge mask too: it is nearly free here and is what
             * splat weights and foliage density want instead of a hand-painted
             * mask.  Sized to the grid so a later cook can ship it. */
            const size_t cells = (size_t)jce_terrain_width(s.terrain) *
                                 (size_t)jce_terrain_height(s.terrain);
            s.ridge_field.assign(cells, 0.0f);
            if (jce_terrain_apply_erosion(s.terrain, &ep, s.ridge_field.data())) {
                if (captured) (void)commit_terrain_history();
                s.preview_dirty = true;
                notify_terrain_changed_full();
            } else {
                /* Refused (e.g. a tiled terrain with no resident grid): drop
                 * the undo entry we just pushed rather than leaving a
                 * no-op step in the stack. */
                if (captured) drop_last_undo();
                s.ridge_field.clear();
            }
        }

        /* ── Thermal / talus ────────────────────────────────────────── */
        ImGui::SeparatorText(jce_editor_i18n("terrain.gen.thermal"));
        ImGui::SliderFloat(jce_editor_i18n("terrain.gen.reposeAngle"), &s.talus_angle, 5.0f, 80.0f, "%.0f");
        ImGui::SliderInt(jce_editor_i18n("terrain.gen.iterations"), &s.talus_iters, 1, 200);
        ImGui::SliderFloat(jce_editor_i18n("terrain.gen.rate"), &s.talus_strength, 0.05f, 1.0f);
        if (ImGui::Button(jce_editor_i18n("terrain.gen.applyThermal"))) {
            const bool captured = push_undo(JCE_TERRAIN_EDIT_HEIGHTS);
            JceTerrainThermalParams tp;
            memset(&tp, 0, sizeof tp);
            tp.talus_angle_deg = s.talus_angle;
            tp.iterations      = s.talus_iters;
            tp.strength        = s.talus_strength;
            if (jce_terrain_apply_thermal(s.terrain, &tp)) {
                if (captured) (void)commit_terrain_history();
                s.preview_dirty = true;
                notify_terrain_changed_full();
            } else if (captured) {
                drop_last_undo();
            }
        }

        /* ── Sky occlusion ──────────────────────────────────────────── */
        ImGui::SeparatorText(jce_editor_i18n("terrain.gen.skyOcclusion"));
        ImGui::SliderInt(jce_editor_i18n("terrain.gen.directions"), &s.occl_dirs, 4, 64);
        if (ImGui::Button(jce_editor_i18n("terrain.gen.bakeOcclusion"))) {
            const size_t cells = (size_t)jce_terrain_width(s.terrain) *
                                 (size_t)jce_terrain_height(s.terrain);
            s.sky_visibility.assign(cells, 1.0f);
            /* Read-only: no undo entry, because it does not touch heights. */
            if (!jce_terrain_bake_sky_occlusion(s.terrain, s.occl_dirs,
                                                s.sky_visibility.data(), NULL))
                s.sky_visibility.clear();
        }
        if (!s.sky_visibility.empty()) {
            double sum = 0.0;
            float lo = 1.0f, hi = 0.0f;
            for (float v : s.sky_visibility) {
                sum += v;
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            ImGui::Text(jce_editor_i18n("terrain.gen.visibilityStats"),
                        (double)lo, (double)hi,
                        sum / (double)s.sky_visibility.size());
        }
        if (!s.ridge_field.empty())
            ImGui::Text(jce_editor_i18n("terrain.gen.ridgeSamples"), (int)s.ridge_field.size());
    }

    if (ImGui::CollapsingHeader(jce_editor_i18n("terrain.brush.header"), ImGuiTreeNodeFlags_DefaultOpen)) {
        int tool = (int)s.tool;
        if (ImGui::Combo(jce_editor_i18n("terrain.brush.tool"), &tool, "Sculpt\0Splat\0Holes\0\0")) {
            s.tool = (ToolMode)tool;
            jce_editor_ui_state_save_int("brush.terrain.tool", tool);
        }
        if (s.tool == ToolMode::Sculpt) {
            if (ImGui::Combo(jce_editor_i18n("terrain.brush.mode"), &s.sculpt_mode,
                             "Raise\0Lower\0Smooth\0Flatten\0\0"))
                jce_editor_ui_state_save_int("brush.terrain.sculpt_mode",
                                             s.sculpt_mode);
        } else if (s.tool == ToolMode::Splat) {
            if (ImGui::Combo(jce_editor_i18n("terrain.brush.layer"), &s.splat_layer,
                             "Layer 0\0Layer 1\0Layer 2\0Layer 3\0\0"))
                jce_editor_ui_state_save_int("brush.terrain.layer", s.splat_layer);
        } else {
            if (ImGui::Checkbox(jce_editor_i18n_id(
                    "terrain.brush.holeErase", "Erase (fill holes back)"),
                    &s.hole_erase))
                jce_editor_ui_state_save_int("brush.terrain.hole_erase",
                                             s.hole_erase ? 1 : 0);
            ImGui::TextDisabled("%s", jce_editor_i18n_id("terrain.brush.holeHint",
                "Cuts cells from render + collision (caves, tunnels, interiors)"));
        }
        if (ImGui::SliderFloat(jce_editor_i18n("terrain.brush.radius"),   &s.brush_radius,   0.5f, 64.0f))
            jce_editor_ui_state_save_float("brush.terrain.radius", s.brush_radius);
        if (ImGui::SliderFloat(jce_editor_i18n("terrain.brush.strength"), &s.brush_strength, 0.1f, 64.0f))
            jce_editor_ui_state_save_float("brush.terrain.strength", s.brush_strength);

        if (s.tool != ToolMode::Holes) {
            if (jce_draw_path_input(jce_editor_i18n("terrain.brush.maskPath"),
                                    s.brush_mask_path,
                                    sizeof(s.brush_mask_path),
                                    JcePathKind::FileAbs))
                jce_editor_ui_state_save_str("brush.terrain.mask_path",
                                             s.brush_mask_path);
            if (ImGui::BeginTable("terrain_brush_mask_buttons", 2,
                                  ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextColumn();
                if (ImGui::Button(jce_editor_i18n("terrain.brush.maskLoad"),
                                  ImVec2(-1, 0)))
                    load_brush_mask(true);
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(s.brush_mask.empty() &&
                                     s.brush_mask_path[0] == '\0');
                if (ImGui::Button(jce_editor_i18n("terrain.brush.maskClear"),
                                  ImVec2(-1, 0)))
                    clear_brush_mask();
                ImGui::EndDisabled();
                ImGui::EndTable();
            }
            if (!s.brush_mask.empty())
                ImGui::TextDisabled(jce_editor_i18n("terrain.brush.maskActive"),
                                    s.brush_mask_w, s.brush_mask_h);

            ImGui::BeginDisabled(s.brush_mask.empty());
            if (ImGui::SliderFloat(jce_editor_i18n("terrain.brush.rotation"),
                                   &s.brush_rotation, -180.0f, 180.0f, "%.0f deg"))
                jce_editor_ui_state_save_float("brush.terrain.rotation",
                                               s.brush_rotation);
            if (ImGui::SliderFloat(
                    jce_editor_i18n("terrain.brush.rotationJitter"),
                    &s.brush_rotation_jitter, 0.0f, 180.0f, "%.0f deg"))
                jce_editor_ui_state_save_float(
                    "brush.terrain.rotation_jitter", s.brush_rotation_jitter);
            ImGui::EndDisabled();

            if (ImGui::SliderFloat(jce_editor_i18n("terrain.brush.hardness"),
                                   &s.brush_hardness, 0.0f, 1.0f))
                jce_editor_ui_state_save_float("brush.terrain.hardness",
                                               s.brush_hardness);
        }
        if (ImGui::SliderFloat(jce_editor_i18n("terrain.brush.spacing"),
                               &s.brush_spacing, 0.05f, 1.0f))
            jce_editor_ui_state_save_float("brush.terrain.spacing",
                                           s.brush_spacing);
        if (ImGui::SliderFloat(jce_editor_i18n("terrain.brush.scatter"),
                               &s.brush_scatter, 0.0f, 1.0f))
            jce_editor_ui_state_save_float("brush.terrain.scatter",
                                           s.brush_scatter);
        if (ImGui::Checkbox(jce_editor_i18n("terrain.brush.paintInScene"),
                            &s.paint_in_scene)) {
            jce_editor_ui_state_save_int("brush.terrain.paint_in_scene",
                                         s.paint_in_scene ? 1 : 0);
            if (s.paint_in_scene) jce_foliage_brush_set_armed(false);
        }
        ImGui::DragFloat(jce_editor_i18n("terrain.brush.cursorX"), &s.cursor_x, 0.5f);
        ImGui::DragFloat(jce_editor_i18n("terrain.brush.cursorZ"), &s.cursor_z, 0.5f);

        if (ImGui::Button(jce_editor_i18n("terrain.brush.stamp"), ImVec2(-1, 0))) {
            const float dt = 0.1f;
            const uint32_t flags = s.tool == ToolMode::Splat
                ? JCE_TERRAIN_EDIT_SPLAT
                : (s.tool == ToolMode::Holes
                    ? JCE_TERRAIN_EDIT_HOLES : JCE_TERRAIN_EDIT_HEIGHTS);
            const bool captured = push_undo_region(
                s.cursor_x - s.brush_radius, s.cursor_z - s.brush_radius,
                s.cursor_x + s.brush_radius, s.cursor_z + s.brush_radius,
                flags);
            bool lock_flatten = s.tool == ToolMode::Sculpt &&
                                s.sculpt_mode == JCE_TERRAIN_SCULPT_FLATTEN;
            float target = jce_terrain_sample_height(s.terrain,
                                                     s.cursor_x, s.cursor_z);
            apply_brush_stamp(s.cursor_x, s.cursor_z, dt, false,
                              lock_flatten, target);
            if (captured) (void)commit_terrain_history();
            s.preview_dirty = true;
            notify_terrain_changed_full();
            notify_terrain_changed_region(
                s.cursor_x - s.brush_radius, s.cursor_z - s.brush_radius,
                s.cursor_x + s.brush_radius, s.cursor_z + s.brush_radius,
                flags);
        }

        if (ImGui::Button(jce_editor_i18n("terrain.brush.clear"), ImVec2(-1, 0))) {
            const bool captured = push_undo(JCE_TERRAIN_EDIT_HEIGHTS);
            int w = jce_terrain_width(s.terrain);
            int h = jce_terrain_height(s.terrain);
            float *heights = const_cast<float *>(jce_terrain_heights(s.terrain));
            std::memset(heights, 0, (size_t)w * (size_t)h * sizeof(float));
            if (captured) (void)commit_terrain_history();
            s.preview_dirty = true;
            notify_terrain_changed_full();
        }

        ImGui::Separator();
        ImGui::BeginDisabled(!jce_state_can_undo());
        if (ImGui::Button(jce_editor_i18n("terrain.brush.undo")))
            jce_state_undo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!jce_state_can_redo());
        if (ImGui::Button(jce_editor_i18n("terrain.brush.redo")))
            jce_state_redo();
        ImGui::EndDisabled();
    }
}

void draw_info_section()
{
    if (!s.terrain) return;
    if (ImGui::CollapsingHeader(jce_editor_i18n("terrain.info.header"), ImGuiTreeNodeFlags_DefaultOpen)) {
        int w  = jce_terrain_width(s.terrain);
        int h  = jce_terrain_height(s.terrain);
        int cs = jce_terrain_chunk_size(s.terrain);
        int cx = jce_terrain_chunk_count_x(s.terrain);
        int cz = jce_terrain_chunk_count_z(s.terrain);
        float wx = jce_terrain_world_size_x(s.terrain);
        float wz = jce_terrain_world_size_z(s.terrain);
        float mh = jce_terrain_max_height(s.terrain);

        ImGui::Text(jce_editor_i18n("terrain.info.vertices"), w, h, w * h);
        ImGui::Text(jce_editor_i18n("terrain.info.worldSize"), wx, wz);
        ImGui::Text(jce_editor_i18n("terrain.info.maxHeight"), mh);
        ImGui::Text(jce_editor_i18n("terrain.info.chunks"), cx, cz, cs);

        /* Sampled height / splat at cursor. */
        float hh = jce_terrain_sample_height(s.terrain, s.cursor_x, s.cursor_z);
        float sw[4]; jce_terrain_sample_splat(s.terrain, s.cursor_x, s.cursor_z, sw);
        ImGui::Separator();
        ImGui::Text(jce_editor_i18n("terrain.info.cursorY"), hh);
        ImGui::Text(jce_editor_i18n("terrain.info.splatW"),
                    sw[0], sw[1], sw[2], sw[3]);
    }
}

void draw_preview_section()
{
    if (!s.terrain) return;
    if (s.preview_dirty) rebuild_preview();
    if (ImGui::CollapsingHeader(jce_editor_i18n("terrain.preview.header"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("%s", jce_editor_i18n("terrain.preview.note"));
        if (s.preview_w > 0 && s.preview_h > 0) {
            const int max_rows = 24, max_cols = 60;
            int row_step = std::max(1, s.preview_h / max_rows);
            int col_step = std::max(1, s.preview_w / max_cols);
            const char *ramp = " .-:=+*#%@";
            int ramp_n = 9;
            for (int j = 0; j < s.preview_h; j += row_step) {
                std::string row;
                row.reserve((size_t)(s.preview_w / col_step) + 1);
                for (int i = 0; i < s.preview_w; i += col_step) {
                    uint32_t px = s.preview_pixels[(size_t)j * s.preview_w + i];
                    uint8_t g = (uint8_t)((px >> 8) & 0xFFu);
                    int idx = (g * ramp_n) / 256;
                    row.push_back(ramp[idx]);
                }
                ImGui::TextUnformatted(row.c_str());
            }
        }
        if (ImGui::Button(jce_editor_i18n("terrain.preview.refresh"))) s.preview_dirty = true;
    }
}

} // namespace

extern "C" void jce_editor_panel_terrain(void)
{
    ensure_history_provider_registered();
    validate_scene_owned_terrain();
    bool *p_open = jce_editor_panel_visible_ptr(JCE_PANEL_TERRAIN);
    if (!p_open || !*p_open) return;
    (void)publish_terrain_to_scene();
    ImGui::SetNextWindowSize(ImVec2(420, 720), ImGuiCond_FirstUseEver);
    char _wt[96];
    snprintf(_wt, sizeof(_wt), "%s###jce_terrain", jce_editor_i18n("terrain.title"));
    if (!ImGui::Begin(_wt, p_open, ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return;
    }

    draw_toolbar();
    ImGui::Separator();
    draw_create_section();
    ImGui::Separator();
    draw_heightmap_io_section();
    ImGui::Separator();
    draw_brush_section();
    ImGui::Separator();
    draw_info_section();
    ImGui::Separator();
    draw_preview_section();

    if (!s.status.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.8f, 0.9f, 0.6f, 1.0f), "%s", s.status.c_str());
    }
    ImGui::End();
}

extern "C" bool jce_terrain_panel_open_asset(const char *path)
{
    if (!path || !path[0]) return false;
    ensure_history_provider_registered();
    validate_scene_owned_terrain();

    JceScene *scene = jce_state_get_scene();
    if (!(s.scene_owns_terrain && scene == s.terrain_scene &&
          std::strcmp(path, s.terrain_scene_path) == 0)) {
        release_panel_terrain();
        clear_undo_history();
        std::snprintf(s.io_path, sizeof(s.io_path), "%s", path);

        if (scene) {
            s.terrain = jce_scene_peek_terrain(scene, path);
            if (s.terrain) {
                s.terrain_scene = scene;
                s.scene_owns_terrain = true;
                std::snprintf(s.terrain_scene_path,
                              sizeof(s.terrain_scene_path), "%s", path);
            }
        }
        if (!s.terrain) {
            char resolved[1024];
            const char *load_path = path;
            if (jce_editor_resolve_asset_path(path, resolved,
                                              sizeof(resolved)))
                load_path = resolved;
            s.terrain = jce_terrain_load_file(load_path);
            if (s.terrain) (void)publish_terrain_to_scene();
        }
        s.preview_dirty = true;
        if (s.terrain) {
            jce_editor_pstate_set_str("doc.terrain.last", path);
            s.status = jce_editor_i18n("terrain.status.loaded");
        } else {
            s.status = jce_editor_i18n("terrain.status.loadFailed");
        }
    }

    bool *visible = jce_editor_panel_visible_ptr(JCE_PANEL_TERRAIN);
    if (visible) *visible = true;
    jce_editor_panel_request_focus("###jce_terrain");
    return s.terrain != nullptr;
}

/* ── Scene-View brush bridge ───────────────────────────────────────
 *  Called from jce_panel_scene_view.cpp when LMB drags inside the
 *  viewport.  Returns whether the brush mode is "armed" (i.e. the
 *  user opted in via the panel) AND a terrain is loaded.            */
extern "C" bool jce_terrain_panel_brush_armed(void)
{
    validate_scene_owned_terrain();
    if (!s.paint_in_scene || !s.terrain ||
        !jce_terrain_heights(s.terrain))
        return false;
    (void)publish_terrain_to_scene();
    JceEntity entity = 0;
    return find_terrain_binding(jce_state_get_scene(), &entity, nullptr, 0);
}

extern "C" struct JceTerrain *jce_terrain_panel_get_terrain(void)
{
    validate_scene_owned_terrain();
    return s.terrain;
}

extern "C" void jce_terrain_panel_set_brush_armed(bool armed)
{
    if (!armed) end_stroke();
    s.paint_in_scene = armed;
    jce_editor_ui_state_save_int("brush.terrain.paint_in_scene", armed ? 1 : 0);
}

extern "C" float jce_terrain_panel_brush_radius(void)
{
    return s.brush_radius;
}

extern "C" uint32_t jce_terrain_panel_brush_edit_flags(void)
{
    if (s.tool == ToolMode::Splat) return JCE_TERRAIN_EDIT_SPLAT;
    if (s.tool == ToolMode::Holes) return JCE_TERRAIN_EDIT_HOLES;
    return JCE_TERRAIN_EDIT_HEIGHTS;
}

extern "C" void jce_terrain_panel_apply_brush_local(float local_x,
                                                       float local_z,
                                                       float dt)
{
    if (!s.terrain || !jce_terrain_heights(s.terrain)) return;
    /* One undo entry per drag: snapshot on the first applied frame of the
     * stroke; jce_terrain_panel_end_brush_stroke() (called by the Scene
     * View on mouse release) re-arms capture for the next stroke. */
    begin_stroke(local_x, local_z);
    const bool had_previous = s.stroke_has_point;
    const float previous_x = s.stroke_prev_x;
    const float previous_z = s.stroke_prev_z;
    const float spread = s.brush_radius * (1.0f + s.brush_scatter);
    const float min_x = std::min(local_x,
                                 had_previous ? previous_x : local_x) - spread;
    const float min_z = std::min(local_z,
                                 had_previous ? previous_z : local_z) - spread;
    const float max_x = std::max(local_x,
                                 had_previous ? previous_x : local_x) + spread;
    const float max_z = std::max(local_z,
                                 had_previous ? previous_z : local_z) + spread;
    const uint32_t edit_flags = jce_terrain_panel_brush_edit_flags();
    (void)s_terrain_history.capture_region(
        min_x, min_z, max_x, max_z, edit_flags);
    s.cursor_x = local_x;
    s.cursor_z = local_z;
    if (dt <= 0.0f) dt = 1.0f / 60.0f;
    apply_continuous_stroke(local_x, local_z, dt);
    s.preview_dirty = true;
    s_stroke_changed = true;

    notify_terrain_changed_region(min_x, min_z, max_x, max_z, edit_flags);
    accumulate_stroke_region(min_x, min_z, max_x, max_z, edit_flags);
}

/* Called by the Scene View when the brush LMB is released, ending the
 * current drag so the next drag captures a fresh undo snapshot. */
extern "C" void jce_terrain_panel_end_brush_stroke(void)
{
    end_stroke();
}
