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
#include "core/jce_editor_component_registry.h"
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
#include <jce/middleware/scene/jce_scene_fullscreen_effect.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_pbr_material.h>
}

/* ── Named struct types for shared mutable state ──────────────────── */

/* Component copy/paste buffer — keyed by the dense engine comp_id (the
 * registry struct_size is the authoritative byte count).  8192 covers the
 * largest component (LightProbeGroup ≈ 7.7 KB, IkConstraints ≈ 4.3 KB);
 * the copy menu item stays disabled for anything larger. */
struct InspCompClipboard {
    int      comp_id;
    char     data[8192];
    size_t   data_size;
};

/* Deferred section actions — all keyed by the dense engine comp_id
 * (JCE_COMP_ID_INVALID = unset; 0 is a valid id, Transform). */
struct InspPendingRemove {
    uint32_t entity_id;
    int      comp_id;
    bool     pending;
};

struct InspPendingMove {
    uint32_t entity_id;
    int      src_comp;
    int      dir;
    bool     pending;
};

struct InspDrag {
    uint32_t entity_id;
    int      src_comp;
    int      hover_comp;
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

/* Which entity the component drawer currently running belongs to, or 0.
 *
 * insp_track_edit() runs deep inside every drawer, where the entity is not a
 * parameter -- but every drawer is invoked from one place (INSP_DRAWFN), so
 * the scope is set there and read here.  With it, an inspector value edit
 * records ONE ENTITY instead of serialising the whole scene twice: at 50k
 * entities the unscoped path costs ~1.4 s per slider release and its undo
 * clears the scene, dropping every handle and the selection.  0 restores the
 * unscoped behaviour exactly. */
void insp_set_edit_scope(uint32_t entity_id);

void insp_track_edit(void);
void insp_unwired_badge(void);
void insp_unwired_field_badge(void);
void insp_undo_bool(bool *value);
void insp_undo_int(int *value, int prev);

/* One undoable step for a DISCRETE edit whose widget wrote a TEMPORARY, not
 * the field.
 *
 * The dominant Inspector shape for a combo is:
 *
 *     int mode = cc->mode;                       // temp seeded from the field
 *     if (ImGui::Combo(lbl, &mode, items, n)) {  // true only on the change frame
 *         cc->mode = (uint8_t)mode;
 *         insp_track_edit();                     // <- runs on neither the
 *     }                                          //    activation nor the
 *                                                //    deactivation frame
 *
 * insp_track_edit() keys on IsItemActivated / IsItemDeactivated, and for a
 * combo those land on the frames the popup opens and closes -- never on the
 * frame the value changes.  So NEITHER half ran: the edit was not undoable and
 * did not mark the scene dirty, silently.  The fields behind these combos are
 * the discrete ones a designer sets once and trusts (physics layer, CCD mode,
 * collider mode, clear mode, volume shape).
 *
 * This helper is the fix for that shape.  It works because at the point the
 * body runs the FIELD still holds the pre-edit value -- the widget only wrote
 * the temp -- so a snapshot taken before the assignment is exactly the state
 * undo must return to.  No revert dance is needed, unlike insp_undo_bool /
 * insp_undo_int above, which exist for the OTHER shape: a widget handed
 * `&field` directly, so by the time they are called the new value is already
 * in place and has to be briefly rolled back to be snapshotted.
 *
 * Writing the same value is not an edit and pushes nothing.
 *
 * Two type parameters, not one: the field is routinely narrower than the temp
 * the widget drove (a uint8_t enum behind an `int` combo index), and a single
 * parameter would fail deduction at exactly those call sites and push the cast
 * back onto every one of them. */
template <typename T, typename U>
inline void insp_undo_set(T *field, U value)
{
    const T v = (T)value;
    if (!field || *field == v) return;
    jce_state_begin_batch_edit();   /* *field is still the pre-edit value */
    *field = v;
    jce_state_end_batch_edit();
}

/* One undoable step for a DISCRETE edit whose widget wrote THE FIELD directly.
 *
 * `ImGui::Combo(lbl, &cj->y_motion, ...)` hands the widget the field itself, so
 * by the time the body runs the pre-edit value is already gone and there is
 * nothing left to snapshot.  Hence the revert dance: briefly put `prev` back,
 * snapshot THAT, then re-apply.  insp_undo_bool and insp_undo_int are the
 * bool/int special cases of exactly this, with 122 call sites between them;
 * this is the same thing for every other field type.
 *
 * Pair it with INSP_UNDO_DIRECT below, which captures `prev` for you -- doing
 * it by hand needs a differently-named local at every site, and six of these
 * sit consecutively in one scope. */
template <typename T>
inline void insp_undo_committed(T *field, T prev)
{
    if (!field || *field == prev) return;
    const T now = *field;
    *field = prev;
    jce_state_begin_batch_edit();
    *field = now;
    jce_state_end_batch_edit();
}

/* INSP_UNDO_DIRECT(field, widget_call) — capture the pre-edit value, run the
 * widget, and commit one undo step when the widget reports a change.  Shaped
 * after INSP_RESET_CTX above: a scoped do/while so consecutive uses in one
 * function do not collide. */
#define INSP_UNDO_DIRECT(FIELD, WIDGET_CALL)                                \
    do {                                                                    \
        const auto _insp_prev = (FIELD);                                    \
        if (WIDGET_CALL) insp_undo_committed(&(FIELD), _insp_prev);         \
    } while (0)

/* One undoable step for a body that mutates MORE THAN ONE thing.
 *
 * insp_undo_set covers "widget wrote a temp, body copies it into one field".
 * The remaining shape is the same predicate wrapping a body that touches
 * several fields at once -- a checkbox that flips a bit AND seeds an array, a
 * button that appends a layer and initialises it, a drag that unpacks three
 * components of a vector plus a `dirty` flag:
 *
 *     bool bt = skel->use_blend_tree;
 *     if (ImGui::Checkbox(lbl, &bt)) {       // true only on the change frame
 *         INSP_UNDO_SCOPE();
 *         skel->use_blend_tree = bt;
 *         if (bt) seed_thresholds(skel);     // <- the second mutation
 *     }
 *
 * The widget wrote the temp, so every one of those fields still holds its
 * pre-edit value when the body starts; one batch around the whole body is the
 * correct "before".  Enumerating the fields into insp_undo_set calls instead
 * would push a separate undo step per field, which is wrong: the user made one
 * edit and expects one Ctrl+Z to take it back.
 *
 * RAII rather than a begin/end pair because these bodies are the ones that
 * contain `continue`, `break` and early `return` -- a paired call would leak an
 * open batch down those paths and swallow every later edit into it.
 *
 * Cost note: the batch serialises the scene twice, so it belongs INSIDE the
 * predicate (only on frames something actually changed), never around the
 * widget call itself.  history_end_edit() compares before/after and pops its
 * own snapshot when they match, so a body that turns out to change nothing
 * leaves no undo entry and does not mark the scene dirty. */
struct InspUndoScope {
    InspUndoScope() { jce_state_begin_batch_edit(); }
    ~InspUndoScope() { jce_state_end_batch_edit(); }
    InspUndoScope(const InspUndoScope &) = delete;
    InspUndoScope &operator=(const InspUndoScope &) = delete;
};
#define INSP_UNDO_SCOPE() InspUndoScope _insp_undo_scope_

void draw_vec3_control(const char *label, float *values,
                       float speed = 0.1f, float reset_value = 0.0f);
void accept_asset_drop(char *buf, size_t buf_size);
/* Apply the "a model was dropped on the mesh field" side effects: reset the
   primitive shape and, for a real mesh file, import its material/textures.
   `abs_path` must be the RAW absolute dropped path (assimp needs a real
   filesystem path); get it from JcePathInputOpts::dropped_raw. */
void apply_mesh_drop_material(JceMeshRenderer *mr, const char *abs_path);

/* ── load_material_into_renderer — defined in jce_panel_inspector_render.cpp ── */
/* Forward-declared here because jce_editor_inspector_reload_material
 * (public C API in the dispatcher) calls it. */
void load_material_into_renderer(JceMeshRenderer *mr);

/* ── Per-domain drawer declarations ───────────────────────────────── */

/* jce_panel_inspector_transform.cpp */
void draw_comp_transform(uint32_t entity_id, JceTransform *t);
void draw_comp_pivot(JceScene *scene, JceEntity e, JcePivotComponent *p);

/* jce_panel_inspector_lighting.cpp */
void draw_comp_light(JceScene *scene, JceEntity e, uint64_t flags);
void draw_comp_camera(JceCameraComponent *cam);
void draw_comp_virtual_camera(JceVirtualCameraComponent *vc);
void draw_comp_reflection_probe(JceScene *scene, JceEntity e,
                                JceReflectionProbeComponent *r);
void draw_comp_light_probe_group(JceLightProbeGroupComponent *g);

/* jce_panel_inspector_render.cpp */
void draw_comp_mesh_renderer(JceMeshRenderer *mr);
void draw_comp_sprite_renderer(JceSpriteRendererComponent *sr);
void draw_comp_skybox(JceSkyboxComponent *sky);
void draw_comp_billboard_renderer(JceBillboardRendererComponent *b);
void draw_comp_trail_renderer(JceTrailRendererComponent *t);
void draw_comp_line_renderer(JceLineRendererComponent *l);
void draw_comp_decal(JceDecalComponent *d);
void draw_comp_lod_group(JceLodGroupComponent *lg, JceScene *scene, JceEntity e);
void draw_comp_volume(JceVolumeComponent *v);
void draw_comp_fullscreen_effect(JceSceneFullscreenEffect *effect);
void draw_comp_occlusion_portal(JceOcclusionPortalComponent *op);

/* jce_panel_inspector_physics.cpp */
void draw_comp_rigidbody(JceRigidBodyComponent *rb);
void draw_comp_box_collider(JceBoxColliderComponent *bc);
void draw_comp_sphere_collider(JceSphereColliderComponent *sc);
void draw_comp_capsule_collider(JceCapsuleColliderComponent *cc);
void draw_comp_mesh_collider(JceMeshColliderComponent *mc);
void draw_comp_compound_collider(JceCompoundColliderComponent *cc);
void draw_comp_character_controller(JceCharacterControllerComponent *cc);
/* Collision-layer combo + "collides with" readout, shared by every component
 * that indexes the project Layer Collision Matrix.  Defined in
 * jce_panel_inspector_physics.cpp. */
void insp_physics_layer_combo(uint32_t *field, const char *id, bool two_d);
void draw_comp_constraint(JceConstraintComponent *con);
void draw_comp_wheel_collider(JceWheelColliderComponent *w);
void draw_comp_vehicle(JceVehicleComponent *v);
void draw_comp_soft_body(JceSoftBodyComponent *sb);
void draw_comp_fracture(JceFractureComponent *fr);
void draw_comp_constant_force(JceConstantForceComponent *cf);
void draw_comp_configurable_joint(JceConfigurableJointComponent *cj);
void draw_comp_cloth(JceClothComponent *cl);

/* jce_panel_inspector_network.cpp */
void draw_comp_network_object(JceNetworkObjectComponent *c);
void draw_comp_net_transform(JceNetTransformComponent *c);
void draw_comp_net_animator(JceNetAnimatorComponent *c);
void draw_comp_net_rigidbody(JceNetRigidbodyComponent *c);
void draw_comp_network_variable(JceNetworkVariableComponent *c);

/* jce_panel_inspector_physics2d.cpp */
void draw_comp_rigidbody2d(JceRigidBody2DComponent *rb);
void draw_comp_collider2d(JceCollider2DComponent *cd);
void draw_comp_joint2d(JceJoint2DComponent *j);

/* jce_panel_inspector_animation.cpp */
void draw_comp_animator(JceAnimatorComponent *anim);
void draw_comp_skeletal_animator(JceSkeletalAnimatorComponent *skel);
void draw_comp_sprite_animator(JceSpriteAnimatorComponent *sa);
void draw_comp_avatar(JceAvatarComponent *a);
void draw_comp_ik_constraints(JceIkConstraintComponent *ik);
void draw_comp_foot_ik(JceFootIkComponent *f);
void draw_comp_full_body_ik(JceFullBodyIkComponent *f);
void draw_comp_ragdoll(JceRagdollComponent *r);
void draw_comp_morph_weights(JceScene *scene, JceEntity e,
                             JceMorphWeightsComponent *mw);

/* jce_panel_inspector_tilemap.cpp */
void draw_comp_tilemap(JceTilemapComponent *t);
void draw_comp_tilemap_collider2d(JceTilemapCollider2DComponent *c);

/* jce_panel_inspector_audio.cpp */
void draw_comp_audio_source(JceAudioSourceComponent *as, JceEntity e);
void draw_comp_music_track(JceMusicTrackComponent *m);
void draw_comp_video_player(JceVideoPlayerComponent *vp);
void draw_comp_audio_listener(JceAudioListenerComponent *l);
void draw_comp_audio_reverb_zone(JceAudioReverbZoneComponent *r);
void draw_comp_audio_occlusion(JceAudioOcclusionComponent *o);

/* jce_panel_inspector_gameplay.cpp */
void draw_comp_behavior_tree(JceBehaviorTree *bt);
void draw_comp_spawn_manager(JceSpawnManagerComponent *m);
void draw_comp_weapon(JceWeaponComponent *w);
void draw_comp_save_point(JceSavePointComponent *sp);
void draw_comp_trigger_volume(JceTriggerVolumeComponent *tv);
void draw_comp_terrain(JceTerrainComponent *tc);
/* Takes the scene + entity because the placement BAKE needs the world
 * transform and the terrain, not just the component. */
void draw_comp_vegetation_scatter(JceVegetationScatterComponent *vs,
                                  JceScene *scene, JceEntity entity);
void draw_comp_foliage_cluster(JceFoliageClusterComponent *fc);
void draw_comp_grass_field(JceGrassFieldComponent *g);
void draw_comp_water(JceWaterComponent *w);
void draw_comp_buoyancy(JceBuoyancyComponent *b);
void draw_comp_particle_emitter(JceParticleEmitterComponent *pe);
void draw_comp_script(JceScriptComponent *scr);
void draw_comp_nav_agent(JceNavAgentComponent *na);
void draw_comp_sim_lod(JceSimLodComponent *sl);
void draw_comp_sequence_player(JceSequencePlayerComponent *sp);
void draw_comp_gas(JceGameplayAbilitySystemComponent *gas);

/* jce_panel_inspector_ui.cpp */
void draw_comp_canvas(JceCanvasComponent *cv);
void draw_comp_canvas_group(JceCanvasGroupComponent *cg);
void draw_comp_content_size_fitter(JceContentSizeFitterComponent *f);
void draw_comp_bone_attachment(JceBoneAttachmentComponent *a);
void draw_comp_layout_group(JceLayoutGroupComponent *lg);
void draw_comp_layout_element(JceLayoutElementComponent *le);
void draw_comp_ui_image(JceUIImageComponent *im);
void draw_comp_ui_text(JceUITextComponent *tx);
void draw_comp_ui_button(JceUIButtonComponent *bt);
void draw_comp_ui_slider(JceUISliderComponent *sl);
void draw_comp_ui_toggle(JceUIToggleComponent *tg);
void draw_comp_ui_input_field(JceUIInputFieldComponent *f);
void draw_comp_ui_scroll_view(JceUIScrollViewComponent *sv);
void draw_comp_ui_progress_bar(JceUIProgressBarComponent *p);
void draw_comp_ui_dropdown(JceUIDropdownComponent *d);

/* jce_panel_inspector_multi_select.cpp */
bool insp_draw_multi_select_view(JceScene *scene);

/* jce_panel_inspector_add_component.cpp */
void insp_add_component_button_and_popup(uint32_t focused, uint64_t flags);

/* Composite light mask, shared between the dispatcher and Add Component. */
#define INSP_LIGHT_MASK JCE_EDITOR_COMPONENT_LIGHT_MASK
