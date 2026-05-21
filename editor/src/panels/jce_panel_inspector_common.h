/*
 * jce_panel_inspector_common.h
 *
 * Shared includes, state extern declarations, helper function declarations,
 * and the INSP_RESET_CTX macro for the split inspector translation units.
 *
 * Every inspector_*.cpp and the trimmed jce_panel_inspector.cpp include ONLY
 * this header.  Do not include engine headers or SDL/bgfx here.
 */

#pragma once

#include "ui/jce_editor_colors.h"
#include "ui/jce_theme_palette.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_presets.h"
#include "core/jce_editor_state_internal.h"
#include "core/jce_project_settings.h"
#include "core/jce_reflect.h"
#include "scene/jce_editor_scene_render.h"
#include "scene/jce_model_loader_assimp.h"
#include "dialogs/jce_path_input.h"
#include "core/jce_assetdb.h"
#include "core/jce_editor_quat.h"

#include <jce/tools/jce_imgui.hpp>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <string>

/* Per-entity euler cache shared with the scene-view gizmo. */
extern "C++" {
bool jce_editor_get_cached_euler_deg(uint32_t entity_id, jce_quat current_q, float out_deg[3]);
void jce_editor_set_cached_euler_deg(uint32_t entity_id, jce_quat q, const float deg[3]);
}

extern "C" {
#include <jce/middleware/animation/jce_animation.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_pbr_material.h>
}

/* ── Named struct types for shared mutable state ──────────────────── */

struct InspCompClipboard {
    uint64_t flag;
    char     data[4096];
    size_t   data_size;
};

struct InspPendingRemove {
    uint32_t entity_id;
    uint64_t flag;
    bool     pending;
};

struct InspPendingMove {
    uint32_t entity_id;
    uint64_t src_flag;
    int      dir;
    size_t   target_index;
    bool     pending;
};

struct InspDrag {
    uint32_t entity_id;
    uint64_t src_flag;
    uint64_t hover_flag;
    bool     active;
};

/* ── Shared state — defined in jce_panel_inspector_common.cpp ─────── */

extern InspCompClipboard s_comp_clipboard;
extern bool              s_insp_batch_open;
extern InspPendingRemove s_pending_remove;
extern char              s_preset_save_buf[64];
extern InspPendingMove   s_pending_move;
extern InspDrag          s_drag;

/* ── Field-level "Reset" right-click context menu ─────────────────── */

#define INSP_RESET_CTX(POPUP_ID, ...)                                       \
    do {                                                                    \
        if (ImGui::BeginPopupContextItem(POPUP_ID)) {                       \
            if (ImGui::MenuItem(jce_editor_i18n("inspector.resetField"))) { \
                jce_state_begin_batch_edit();                               \
                __VA_ARGS__;                                                \
                jce_state_end_batch_edit();                                 \
            }                                                               \
            ImGui::EndPopup();                                              \
        }                                                                   \
    } while (0)

/* ── Common helpers — defined in jce_panel_inspector_common.cpp ───── */

void insp_track_edit(void);
void insp_undo_bool(bool *value);
void insp_undo_int(int *value, int prev);
void draw_vec3_control(const char *label, float *values,
                       float speed = 0.1f, float reset_value = 0.0f);
void accept_asset_drop(char *buf, size_t buf_size);
void accept_mesh_drop_with_material(JceMeshRenderer *mr);

/* ── load_material_into_renderer — defined in inspector_render.cpp ── */
/* Forward-declared here because jce_editor_inspector_reload_material
 * (public C API in the dispatcher) calls it. */
void load_material_into_renderer(JceMeshRenderer *mr);

/* ── Per-domain drawer declarations ───────────────────────────────── */

/* inspector_transform.cpp */
void draw_comp_transform(uint32_t entity_id, JceTransform *t);

/* inspector_lighting.cpp */
void draw_comp_light(JceScene *scene, JceEntity e, uint64_t flags);
void draw_comp_camera(JceCameraComponent *cam);
void draw_comp_virtual_camera(JceVirtualCameraComponent *vc);
void draw_comp_reflection_probe(JceReflectionProbeComponent *r);
void draw_comp_light_probe_group(JceLightProbeGroupComponent *g);

/* inspector_render.cpp */
void draw_comp_mesh_renderer(JceMeshRenderer *mr);
void draw_comp_sprite_renderer(JceSpriteRendererComponent *sr);
void draw_comp_skybox(JceSkyboxComponent *sky);
void draw_comp_billboard_renderer(JceBillboardRendererComponent *b);
void draw_comp_trail_renderer(JceTrailRendererComponent *t);
void draw_comp_line_renderer(JceLineRendererComponent *l);
void draw_comp_decal(JceDecalComponent *d);
void draw_comp_lod_group(JceLodGroupComponent *lg);
void draw_comp_volume(JceVolumeComponent *v);
void draw_comp_occlusion_portal(JceOcclusionPortalComponent *op);

/* inspector_physics.cpp */
void draw_comp_rigidbody(JceRigidBodyComponent *rb);
void draw_comp_box_collider(JceBoxColliderComponent *bc);
void draw_comp_sphere_collider(JceSphereColliderComponent *sc);
void draw_comp_capsule_collider(JceCapsuleColliderComponent *cc);
void draw_comp_mesh_collider(JceMeshColliderComponent *mc);
void draw_comp_character_controller(JceCharacterControllerComponent *cc);
void draw_comp_constraint(JceConstraintComponent *con);
void draw_comp_wheel_collider(JceWheelColliderComponent *w);
void draw_comp_constant_force(JceConstantForceComponent *cf);
void draw_comp_configurable_joint(JceConfigurableJointComponent *cj);
void draw_comp_cloth(JceClothComponent *cl);

/* inspector_network.cpp */
void draw_comp_net_transform(JceNetTransformComponent *c);
void draw_comp_net_animator(JceNetAnimatorComponent *c);
void draw_comp_net_rigidbody(JceNetRigidbodyComponent *c);

/* inspector_physics2d.cpp */
void draw_comp_rigidbody2d(JceRigidBody2DComponent *rb);
void draw_comp_collider2d(JceCollider2DComponent *cd);
void draw_comp_joint2d(JceJoint2DComponent *j);

/* inspector_animation.cpp */
void draw_comp_animator(JceAnimatorComponent *anim);
void draw_comp_skeletal_animator(JceSkeletalAnimatorComponent *skel);
void draw_comp_sprite_animator(JceSpriteAnimatorComponent *sa);
void draw_comp_avatar(JceAvatarComponent *a);

/* inspector_vfx_tilemap.cpp */
void draw_comp_vfx_graph(JceVfxGraphComponent *v);
void draw_comp_tilemap(JceTilemapComponent *t);
void draw_comp_tilemap_collider2d(JceTilemapCollider2DComponent *c);

/* inspector_audio.cpp */
void draw_comp_audio_source(JceAudioSourceComponent *as);
void draw_comp_audio_listener(JceAudioListenerComponent *l);
void draw_comp_audio_reverb_zone(JceAudioReverbZoneComponent *r);
void draw_comp_audio_occlusion(JceAudioOcclusionComponent *o);

/* inspector_gameplay.cpp */
void draw_comp_behavior_tree(JceBehaviorTree *bt);
void draw_comp_spawn_manager(JceSpawnManagerComponent *m);
void draw_comp_weapon(JceWeaponComponent *w);
void draw_comp_save_point(JceSavePointComponent *sp);
void draw_comp_trigger_volume(JceTriggerVolumeComponent *tv);
void draw_comp_terrain(JceTerrainComponent *tc);
void draw_comp_particle_emitter(JceParticleEmitterComponent *pe);
void draw_comp_script(JceScriptComponent *scr);

/* inspector_ui.cpp */
void draw_comp_canvas(JceCanvasComponent *cv);
void draw_comp_canvas_group(JceCanvasGroupComponent *cg);
void draw_comp_layout_group(JceLayoutGroupComponent *lg);
void draw_comp_ui_image(JceUIImageComponent *im);
void draw_comp_ui_text(JceUITextComponent *tx);
void draw_comp_ui_button(JceUIButtonComponent *bt);

/* inspector_multi_select.cpp */
bool insp_draw_multi_select_view(JceScene *scene);

/* inspector_add_component.cpp */
void insp_add_component_button_and_popup(uint32_t focused, uint64_t flags);

/* Composite "light" group bit + mask, shared between the dispatcher,
 * sync_component_order, and the Add Component popup so they agree on
 * which raw light flags collapse into a single display slot. */
#define INSP_LIGHT_MASK (JCE_COMP_FLAG_DIR_LIGHT   |   \
                         JCE_COMP_FLAG_POINT_LIGHT |   \
                         JCE_COMP_FLAG_SPOT_LIGHT)
