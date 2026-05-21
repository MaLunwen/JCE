/*
 * jce_panel_inspector.cpp  Inspector panel (entity properties) — dispatcher.
 *
 * All draw_comp_* functions have been moved to domain TUs:
 *   inspector_transform.cpp  inspector_lighting.cpp  inspector_render.cpp
 *   inspector_physics.cpp    inspector_physics2d.cpp inspector_animation.cpp
 *   inspector_audio.cpp      inspector_gameplay.cpp  inspector_ui.cpp
 * Shared state and small helpers live in jce_panel_inspector_common.cpp.
 */

#include "jce_panel_inspector_common.h"

static void *comp_get_ptr_and_size(JceScene *scene, JceEntity e,
                                   uint64_t flag, size_t *out_size);

void *jce_inspector_comp_blob(JceScene *scene, JceEntity e,
                              uint64_t flag, size_t *out_size)
{
    return comp_get_ptr_and_size(scene, e, flag, out_size);
}

static void *comp_get_ptr_and_size(JceScene *scene, JceEntity e,
                                   uint64_t flag, size_t *out_size)
{
    if (!scene) return NULL;
    switch (flag) {
    case JCE_COMP_FLAG_TRANSFORM:
        *out_size = sizeof(JceTransform);
        return jce_scene_get_transform(scene, e);
    case JCE_COMP_FLAG_MESH_RENDERER:
        *out_size = sizeof(JceMeshRenderer);
        return jce_scene_get_mesh_renderer(scene, e);
    case JCE_COMP_FLAG_CAMERA:
        *out_size = sizeof(JceCameraComponent);
        return jce_scene_get_camera(scene, e);
    case JCE_COMP_FLAG_RIGIDBODY:
        *out_size = sizeof(JceRigidBodyComponent);
        return jce_scene_get_rigidbody(scene, e);
    case JCE_COMP_FLAG_BOX_COLLIDER:
        *out_size = sizeof(JceBoxColliderComponent);
        return jce_scene_get_box_collider(scene, e);
    case JCE_COMP_FLAG_SPHERE_COLLIDER:
        *out_size = sizeof(JceSphereColliderComponent);
        return jce_scene_get_sphere_collider(scene, e);
    case JCE_COMP_FLAG_AUDIO_SOURCE:
        *out_size = sizeof(JceAudioSourceComponent);
        return jce_scene_get_audio_source(scene, e);
    case JCE_COMP_FLAG_RIGIDBODY_2D:
        *out_size = sizeof(JceRigidBody2DComponent);
        return jce_scene_get_rigidbody2d(scene, e);
    case JCE_COMP_FLAG_PARTICLE_EMITTER:
        *out_size = sizeof(JceParticleEmitterComponent);
        return jce_scene_get_particle_emitter(scene, e);
    case JCE_COMP_FLAG_BEHAVIOR_TREE:
        *out_size = sizeof(JceBehaviorTree);
        return jce_scene_get_behavior_tree(scene, e);
    case JCE_COMP_FLAG_LOD_GROUP:
        *out_size = sizeof(JceLodGroupComponent);
        return jce_scene_get_lod_group(scene, e);
    case JCE_COMP_FLAG_VIRTUAL_CAMERA:
        *out_size = sizeof(JceVirtualCameraComponent);
        return jce_scene_get_virtual_camera(scene, e);
    case JCE_COMP_FLAG_TRIGGER_VOLUME:
        *out_size = sizeof(JceTriggerVolumeComponent);
        return jce_scene_get_trigger_volume(scene, e);
    case JCE_COMP_FLAG_CAPSULE_COLLIDER:
        *out_size = sizeof(JceCapsuleColliderComponent);
        return jce_scene_get_capsule_collider(scene, e);
    case JCE_COMP_FLAG_MESH_COLLIDER:
        *out_size = sizeof(JceMeshColliderComponent);
        return jce_scene_get_mesh_collider(scene, e);
    case JCE_COMP_FLAG_COLLIDER_2D:
        *out_size = sizeof(JceCollider2DComponent);
        return jce_scene_get_collider2d(scene, e);
    case JCE_COMP_FLAG_TRAIL_RENDERER:
        *out_size = sizeof(JceTrailRendererComponent);
        return jce_scene_get_trail_renderer(scene, e);
    case JCE_COMP_FLAG_LINE_RENDERER:
        *out_size = sizeof(JceLineRendererComponent);
        return jce_scene_get_line_renderer(scene, e);
    case JCE_COMP_FLAG_REFLECTION_PROBE:
        *out_size = sizeof(JceReflectionProbeComponent);
        return jce_scene_get_reflection_probe(scene, e);
    case JCE_COMP_FLAG_DECAL:
        *out_size = sizeof(JceDecalComponent);
        return jce_scene_get_decal(scene, e);
    case JCE_COMP_FLAG_LIGHT_PROBE_GROUP:
        *out_size = sizeof(JceLightProbeGroupComponent);
        return jce_scene_get_light_probe_group(scene, e);
    case JCE_COMP_FLAG_AUDIO_LISTENER:
        *out_size = sizeof(JceAudioListenerComponent);
        return jce_scene_get_audio_listener(scene, e);
    case JCE_COMP_FLAG_AUDIO_REVERB_ZONE:
        *out_size = sizeof(JceAudioReverbZoneComponent);
        return jce_scene_get_audio_reverb_zone(scene, e);
    case JCE_COMP_FLAG_AUDIO_OCCLUSION:
        *out_size = sizeof(JceAudioOcclusionComponent);
        return jce_scene_get_audio_occlusion(scene, e);
    case JCE_COMP_FLAG_SPAWN_MANAGER:
        *out_size = sizeof(JceSpawnManagerComponent);
        return jce_scene_get_spawn_manager(scene, e);
    case JCE_COMP_FLAG_WEAPON:
        *out_size = sizeof(JceWeaponComponent);
        return jce_scene_get_weapon(scene, e);
    case JCE_COMP_FLAG_SAVE_POINT:
        *out_size = sizeof(JceSavePointComponent);
        return jce_scene_get_save_point(scene, e);
    case JCE_COMP_FLAG_WHEEL_COLLIDER:
        *out_size = sizeof(JceWheelColliderComponent);
        return jce_scene_get_wheel_collider(scene, e);
    case JCE_COMP_FLAG_CONSTANT_FORCE:
        *out_size = sizeof(JceConstantForceComponent);
        return jce_scene_get_constant_force(scene, e);
    case JCE_COMP_FLAG_CONFIGURABLE_JOINT:
        *out_size = sizeof(JceConfigurableJointComponent);
        return jce_scene_get_configurable_joint(scene, e);
    case JCE_COMP_FLAG_JOINT_2D:
        *out_size = sizeof(JceJoint2DComponent);
        return jce_scene_get_joint2d(scene, e);
    case JCE_COMP_FLAG_BILLBOARD_RENDERER:
        *out_size = sizeof(JceBillboardRendererComponent);
        return jce_scene_get_billboard_renderer(scene, e);
    case JCE_COMP_FLAG_CANVAS:
        *out_size = sizeof(JceCanvasComponent);
        return jce_scene_get_canvas(scene, e);
    case JCE_COMP_FLAG_CANVAS_GROUP:
        *out_size = sizeof(JceCanvasGroupComponent);
        return jce_scene_get_canvas_group(scene, e);
    case JCE_COMP_FLAG_LAYOUT_GROUP:
        *out_size = sizeof(JceLayoutGroupComponent);
        return jce_scene_get_layout_group(scene, e);
    case JCE_COMP_FLAG_UI_IMAGE:
        *out_size = sizeof(JceUIImageComponent);
        return jce_scene_get_ui_image(scene, e);
    case JCE_COMP_FLAG_UI_TEXT:
        *out_size = sizeof(JceUITextComponent);
        return jce_scene_get_ui_text(scene, e);
    case JCE_COMP_FLAG_UI_BUTTON:
        *out_size = sizeof(JceUIButtonComponent);
        return jce_scene_get_ui_button(scene, e);
    case JCE_COMP_FLAG_CLOTH:
        *out_size = sizeof(JceClothComponent);
        return jce_scene_get_cloth(scene, e);
    case JCE_COMP_FLAG_NET_TRANSFORM:
        *out_size = sizeof(JceNetTransformComponent);
        return jce_scene_get_net_transform(scene, e);
    case JCE_COMP_FLAG_NET_ANIMATOR:
        *out_size = sizeof(JceNetAnimatorComponent);
        return jce_scene_get_net_animator(scene, e);
    case JCE_COMP_FLAG_NET_RIGIDBODY:
        *out_size = sizeof(JceNetRigidbodyComponent);
        return jce_scene_get_net_rigidbody(scene, e);
    case JCE_COMP_FLAG_VFX_GRAPH:
        *out_size = sizeof(JceVfxGraphComponent);
        return jce_scene_get_vfx_graph(scene, e);
    case JCE_COMP_FLAG_TILEMAP:
        *out_size = sizeof(JceTilemapComponent);
        return jce_scene_get_tilemap(scene, e);
    case JCE_COMP_FLAG_TILEMAP_COLLIDER_2D:
        *out_size = sizeof(JceTilemapCollider2DComponent);
        return jce_scene_get_tilemap_collider2d(scene, e);
    case JCE_COMP_FLAG_AVATAR:
        *out_size = sizeof(JceAvatarComponent);
        return jce_scene_get_avatar(scene, e);
    case JCE_COMP_FLAG_VOLUME:
        *out_size = sizeof(JceVolumeComponent);
        return jce_scene_get_volume(scene, e);
    case JCE_COMP_FLAG_OCCLUSION_PORTAL:
        *out_size = sizeof(JceOcclusionPortalComponent);
        return jce_scene_get_occlusion_portal(scene, e);
    default:
        *out_size = 0;
        return NULL;
    }
}

/* ── Tag colors (display data) ────────────────────────────────────── */

static const ImVec4 s_tag_colors[JCE_TAG_COLOR_COUNT] = {
    ImVec4(0, 0, 0, 0),
    JCE_COLOR_TAG_RED,
    JCE_COLOR_TAG_ORANGE,
    JCE_COLOR_TAG_YELLOW,
    JCE_COLOR_TAG_GREEN,
    JCE_COLOR_TAG_BLUE,
    JCE_COLOR_TAG_PURPLE,
    JCE_COLOR_TAG_GRAY,
};

/* ── Inspector state ──────────────────────────────────────────────── */

static struct {
    char name_buf[JCE_MAX_ENTITY_NAME];
    char tag_buf[JCE_MAX_TAG_STRING];
    bool needs_sync;
    bool initialized;
    bool delete_requested;
    uint32_t delete_entity_ids[JCE_MAX_SELECTED];
    int delete_entity_count;
} s_insp;

static void ensure_init(void)
{
    if (s_insp.initialized) return;
    memset(&s_insp, 0, sizeof(s_insp));
    s_insp.needs_sync  = true;
    s_insp.initialized = true;
}

void jce_editor_inspector_request_sync(void)
{
    s_insp.needs_sync = true;
}

void jce_editor_inspector_request_delete_confirm(uint32_t entity_id)
{
    ensure_init();
    if (entity_id == 0) return;
    s_insp.delete_entity_ids[0] = entity_id;
    s_insp.delete_entity_count = 1;
    s_insp.delete_requested = true;
}

void jce_editor_inspector_request_delete_confirm_many(const uint32_t *entity_ids,
                                                      int entity_count)
{
    ensure_init();
    if (!entity_ids || entity_count <= 0) return;

    if (entity_count > JCE_MAX_SELECTED)
        entity_count = JCE_MAX_SELECTED;

    int write_count = 0;
    for (int i = 0; i < entity_count; i++) {
        uint32_t id = entity_ids[i];
        if (id == 0) continue;

        bool duplicate = false;
        for (int j = 0; j < write_count; j++) {
            if (s_insp.delete_entity_ids[j] == id) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        s_insp.delete_entity_ids[write_count++] = id;
    }

    if (write_count <= 0) return;

    s_insp.delete_entity_count = write_count;
    s_insp.delete_requested = true;
}

bool jce_editor_inspector_delete_dialog_open(void)
{
    return s_insp.delete_requested;
}

/* ── Component header / settings popup helper ─────────────────────── */

/* Returns true if the component's body should be drawn this frame.
 * Updates sidecar.expanded_flags fold state.  Handles the "..." popup
 * with a Remove menu (disabled when not removable, e.g. Transform). */
static bool comp_section_begin(uint32_t entity_id,
                               EditorEntitySidecar &sidecar,
                               uint64_t flag,
                               const char *display_name,
                               bool removable)
{
    ImGui::PushID((int)(flag ^ (flag >> 32)));

    bool was_open = (sidecar.expanded_flags & flag) != 0;
    int tn_flags = ImGuiTreeNodeFlags_AllowOverlap |
                   (was_open ? ImGuiTreeNodeFlags_DefaultOpen : 0);

    /* Inspector collapsing-header tint:
       - Dark themes: keep the slate slate-blue accent (#323744) so it
         reads as a distinct band over the dark window bg.
       - Light themes: defer to ImGuiCol_Header so the bar tracks the
         active palette (avoids a near-black strip on white). */
    ImGui::PushStyleColor(ImGuiCol_Header, jce_theme::inspector_header_color());
    bool open = ImGui::CollapsingHeader(display_name, tn_flags);
    if (open) sidecar.expanded_flags |= flag;
    else      sidecar.expanded_flags &= ~flag;

    /* Drag-reorder: pressing & dragging a header begins a drag; while
     * active, hovering another header records it as the drop target. On
     * mouse release the loop applies the move. */
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
        if (!s_drag.active) {
            s_drag.active   = true;
            s_drag.entity_id = entity_id;
            s_drag.src_flag  = flag;
        }
    }
    if (s_drag.active && s_drag.entity_id == entity_id && ImGui::IsItemHovered()) {
        s_drag.hover_flag = flag;
        /* Visual cue: thin line above the hovered header. */
        ImVec2 mn = ImGui::GetItemRectMin();
        ImVec2 mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(mn.x, mn.y), ImVec2(mx.x, mn.y),
            ImGui::GetColorU32(ImGuiCol_DragDropTarget), 2.0f);
    }

    float header_w = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(header_w - 20);
    if (ImGui::SmallButton("..."))
        ImGui::OpenPopup("ComponentSettings");

    if (ImGui::BeginPopup("ComponentSettings")) {
        JceScene *_cs = jce_state_get_scene();
        JceEntity _ce = jce_state_to_ecs_entity(entity_id);
        size_t _csz = 0;
        void *_cptr = comp_get_ptr_and_size(_cs, _ce, flag, &_csz);

        if (ImGui::MenuItem(jce_editor_i18n("inspector.copyComponent"),
                            NULL, false, _cptr != NULL && _csz > 0 && _csz <= sizeof(s_comp_clipboard.data))) {
            s_comp_clipboard.flag = flag;
            s_comp_clipboard.data_size = _csz;
            memcpy(s_comp_clipboard.data, _cptr, _csz);
        }
        bool can_paste = (s_comp_clipboard.flag == flag && s_comp_clipboard.data_size > 0
                          && _cptr != NULL && _csz == s_comp_clipboard.data_size);
        if (!can_paste) ImGui::BeginDisabled();
        if (ImGui::MenuItem(jce_editor_i18n("inspector.pasteComponentValues"))) {
            jce_state_begin_batch_edit();
            memcpy(_cptr, s_comp_clipboard.data, s_comp_clipboard.data_size);
            jce_state_end_batch_edit();
        }
        if (!can_paste) ImGui::EndDisabled();

        ImGui::Separator();

        /* Move Up / Move Down — defer to end-of-frame loop. */
        if (ImGui::MenuItem(jce_editor_i18n("inspector.moveUp"))) {
            s_pending_move.entity_id = entity_id;
            s_pending_move.src_flag  = flag;
            s_pending_move.dir       = -1;
            s_pending_move.pending   = true;
        }
        if (ImGui::MenuItem(jce_editor_i18n("inspector.moveDown"))) {
            s_pending_move.entity_id = entity_id;
            s_pending_move.src_flag  = flag;
            s_pending_move.dir       = +1;
            s_pending_move.pending   = true;
        }

        ImGui::Separator();

        /* Preset submenu — save the focused entity's component values to
         * a named preset, or apply a previously saved preset. The actual
         * OpenPopup must run outside BeginMenu (different ID-stack) so the
         * matching BeginPopup below can find it. */
        bool open_preset_save = false;
        if (ImGui::BeginMenu(jce_editor_i18n("inspector.preset"))) {
            if (ImGui::MenuItem(jce_editor_i18n("inspector.preset.saveAs"))) {
                open_preset_save = true;
            }
            ImGui::Separator();
            std::vector<std::string> names = jce_preset_list(flag);
            if (names.empty()) {
                ImGui::TextDisabled("%s", jce_editor_i18n("inspector.preset.empty"));
            } else {
                std::string pending_delete;
                for (const auto &n : names) {
                    if (ImGui::BeginMenu(n.c_str())) {
                        if (ImGui::MenuItem(jce_editor_i18n("inspector.preset.apply"))) {
                            jce_preset_apply(flag, n.c_str(), _cs, _ce);
                        }
                        ImGui::Separator();
                        ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_ERROR);
                        if (ImGui::MenuItem(jce_editor_i18n("inspector.preset.delete"))) {
                            pending_delete = n;
                        }
                        ImGui::PopStyleColor();
                        ImGui::EndMenu();
                    }
                }
                if (!pending_delete.empty()) {
                    jce_preset_delete(flag, pending_delete.c_str());
                }
            }
            ImGui::EndMenu();
        }
        if (open_preset_save) {
            s_preset_save_buf[0] = '\0';
            ImGui::OpenPopup("##preset_save_popup");
        }

        ImGui::Separator();

        if (ImGui::MenuItem(jce_editor_i18n("transform.reset"))) {
            if (_cptr && _csz > 0) {
                jce_state_begin_batch_edit();
                if (flag == JCE_COMP_FLAG_TRANSFORM) {
                    JceTransform *t = (JceTransform *)_cptr;
                    t->position = { 0.0f, 0.0f, 0.0f };
                    t->rotation = jce_q_identity();
                    t->scale    = { 1.0f, 1.0f, 1.0f };
                } else {
                    memset(_cptr, 0, _csz);
                }
                jce_state_end_batch_edit();
            }
        }

        ImGui::Separator();

        if (!removable) {
            ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_ERROR);
            ImGui::MenuItem(jce_editor_i18n("inspector.removeComponent"), NULL, false, false);
            ImGui::PopStyleColor();
            ImGui::EndDisabled();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_ERROR);
            if (ImGui::MenuItem(jce_editor_i18n("inspector.removeComponent"))) {
                s_pending_remove.entity_id = entity_id;
                s_pending_remove.flag      = flag;
                s_pending_remove.pending   = true;
            }
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();

    /* Preset save modal — shared per-section so opening one closes others. */
    if (ImGui::BeginPopup("##preset_save_popup")) {
        ImGui::Text(jce_editor_i18n("inspector.preset.savePromptFmt"),
                    jce_comp_flag_display_name(flag));
        ImGui::SetNextItemWidth(220);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool commit = ImGui::InputText("##preset_name", s_preset_save_buf,
                                       sizeof(s_preset_save_buf),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button(jce_editor_i18n("dialog.save")) || commit) {
            if (s_preset_save_buf[0]) {
                JceScene *_ps = jce_state_get_scene();
                JceEntity _pe = jce_state_to_ecs_entity(entity_id);
                jce_preset_save(flag, s_preset_save_buf, _ps, _pe);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("dialog.cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    return open;
}

static void comp_section_end(void)
{
    ImGui::Spacing();
    ImGui::PopID();
}

/* ══════════════════════════════════════════════════════════════════════
 *  COMPONENT DISPLAY ORDER + DISPATCH
 *
 *  Inspector iterates components in user-controlled order stored in
 *  EditorEntitySidecar.component_order.  We use a synthetic "Light Group"
 *  flag (high bit) so the unified Light section (dir/point/spot) gets
 *  one slot that survives switching light type.
 * ══════════════════════════════════════════════════════════════════════ */

/* LIGHT_GROUP_BIT is a synthetic editor-only slot that groups all three light
 * types (dir/point/spot) into a single ordered entry.  It must never equal
 * any real JCE_COMP_FLAG_* value (all of which have exactly one bit set).
 * Using UINT64_MAX is safe because it has all 64 bits set. */
static constexpr uint64_t LIGHT_GROUP_BIT = UINT64_C(0xFFFFFFFFFFFFFFFF);
static constexpr uint64_t LIGHT_MASK      = JCE_COMP_FLAG_DIR_LIGHT |
                                            JCE_COMP_FLAG_POINT_LIGHT |
                                            JCE_COMP_FLAG_SPOT_LIGHT;

/* Default ordering follows the historic hard-coded layout. */
static const uint64_t kDefaultComponentOrder[] = {
    JCE_COMP_FLAG_TRANSFORM,
    LIGHT_GROUP_BIT,
    JCE_COMP_FLAG_CAMERA,
    JCE_COMP_FLAG_MESH_RENDERER,
    JCE_COMP_FLAG_SPRITE_RENDERER,
    JCE_COMP_FLAG_ANIMATOR,
    JCE_COMP_FLAG_SKELETAL_ANIMATOR,
    JCE_COMP_FLAG_RIGIDBODY,
    JCE_COMP_FLAG_BOX_COLLIDER,
    JCE_COMP_FLAG_SPHERE_COLLIDER,
    JCE_COMP_FLAG_CHARACTER_CONTROLLER,
    JCE_COMP_FLAG_AUDIO_SOURCE,
    JCE_COMP_FLAG_SCRIPT,
    JCE_COMP_FLAG_SKYBOX,
    JCE_COMP_FLAG_SPRITE_ANIMATOR,
    JCE_COMP_FLAG_CONSTRAINT,
    JCE_COMP_FLAG_TERRAIN,
    JCE_COMP_FLAG_RIGIDBODY_2D,
    JCE_COMP_FLAG_PARTICLE_EMITTER,
    JCE_COMP_FLAG_BEHAVIOR_TREE,
    JCE_COMP_FLAG_LOD_GROUP,
    JCE_COMP_FLAG_VIRTUAL_CAMERA,
    JCE_COMP_FLAG_TRIGGER_VOLUME,
    JCE_COMP_FLAG_CAPSULE_COLLIDER,
    JCE_COMP_FLAG_MESH_COLLIDER,
    JCE_COMP_FLAG_COLLIDER_2D,
    JCE_COMP_FLAG_TRAIL_RENDERER,
    JCE_COMP_FLAG_LINE_RENDERER,
    JCE_COMP_FLAG_REFLECTION_PROBE,
    JCE_COMP_FLAG_DECAL,
    JCE_COMP_FLAG_LIGHT_PROBE_GROUP,
    JCE_COMP_FLAG_AUDIO_LISTENER,
    JCE_COMP_FLAG_AUDIO_REVERB_ZONE,
    JCE_COMP_FLAG_AUDIO_OCCLUSION,
    JCE_COMP_FLAG_SPAWN_MANAGER,
    JCE_COMP_FLAG_WEAPON,
    JCE_COMP_FLAG_SAVE_POINT,
    JCE_COMP_FLAG_WHEEL_COLLIDER,
    JCE_COMP_FLAG_CONSTANT_FORCE,
    JCE_COMP_FLAG_CONFIGURABLE_JOINT,
    JCE_COMP_FLAG_JOINT_2D,
    JCE_COMP_FLAG_BILLBOARD_RENDERER,
    JCE_COMP_FLAG_CANVAS,
    JCE_COMP_FLAG_CANVAS_GROUP,
    JCE_COMP_FLAG_LAYOUT_GROUP,
    JCE_COMP_FLAG_UI_IMAGE,
    JCE_COMP_FLAG_UI_TEXT,
    JCE_COMP_FLAG_UI_BUTTON,
    JCE_COMP_FLAG_CLOTH,
    JCE_COMP_FLAG_NET_TRANSFORM,
    JCE_COMP_FLAG_NET_ANIMATOR,
    JCE_COMP_FLAG_NET_RIGIDBODY,
    JCE_COMP_FLAG_VFX_GRAPH,
    JCE_COMP_FLAG_TILEMAP,
    JCE_COMP_FLAG_TILEMAP_COLLIDER_2D,
    JCE_COMP_FLAG_AVATAR,
    JCE_COMP_FLAG_VOLUME,
    JCE_COMP_FLAG_OCCLUSION_PORTAL,
};

/* Ensures sidecar.component_order contains exactly the slots we want to
 * draw, given the entity's currently-set component flags:
 *   - Removes entries no longer present (component was removed).
 *   - Appends new entries in default-order positions (component added).
 *   - Light flags collapse into the synthetic LIGHT_GROUP_BIT slot. */
static void sync_component_order(EditorEntitySidecar &sidecar, uint64_t flags)
{
    auto wanted = [&](uint64_t entry) -> bool {
        if (entry == LIGHT_GROUP_BIT) return (flags & LIGHT_MASK) != 0;
        /* Skip raw light flags — they live under LIGHT_GROUP_BIT. */
        if (entry & LIGHT_MASK) return false;
        return (flags & entry) != 0;
    };

    /* Drop stale entries while preserving order of survivors. */
    auto &v = sidecar.component_order;
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](uint64_t f) { return !wanted(f); }),
            v.end());

    /* Add any missing entries by walking the default order. */
    for (uint64_t def : kDefaultComponentOrder) {
        if (!wanted(def)) continue;
        if (std::find(v.begin(), v.end(), def) == v.end())
            v.push_back(def);
    }
}

static void draw_one_component_section(uint32_t focused,
                                       EditorEntitySidecar &sidecar,
                                       JceScene *scene,
                                       JceEntity ecs_e,
                                       uint64_t flag);

/* ══════════════════════════════════════════════════════════════════════
 *  MULTI-OBJECT EDITING
 *  component flag, edits made through the focused entity's section are
 *  byte-mirrored onto the others. We restrict the broadcast to a
 *  whitelist of pure value-type components — string buffers and asset
 *  handles inside other components must be edited per-entity.
 * ══════════════════════════════════════════════════════════════════════ */

static bool multi_edit_supported(uint64_t flag)
{
    switch (flag) {
        case JCE_COMP_FLAG_TRANSFORM:
        case JCE_COMP_FLAG_CAMERA:
        case JCE_COMP_FLAG_MESH_RENDERER:
        case JCE_COMP_FLAG_SPRITE_RENDERER:
        case JCE_COMP_FLAG_RIGIDBODY:
        case JCE_COMP_FLAG_BOX_COLLIDER:
        case JCE_COMP_FLAG_SPHERE_COLLIDER:
        case JCE_COMP_FLAG_CAPSULE_COLLIDER:
        case JCE_COMP_FLAG_MESH_COLLIDER:
        case JCE_COMP_FLAG_AUDIO_SOURCE:
        case JCE_COMP_FLAG_CONSTRAINT:
        case JCE_COMP_FLAG_SKELETAL_ANIMATOR:
            return true;
        default:
            return false;
    }
}

static void *multi_get_comp_ptr(JceScene *scene, JceEntity e,
                                uint64_t flag, size_t *out_size)
{
#define M(F, GETTER, T)                                                   \
    case F: {                                                             \
        T *p = GETTER(scene, e);                                          \
        if (out_size) *out_size = sizeof(T);                              \
        return (void *)p;                                                 \
    }
    switch (flag) {
        M(JCE_COMP_FLAG_TRANSFORM,         jce_scene_get_transform,         JceTransform)
        M(JCE_COMP_FLAG_CAMERA,            jce_scene_get_camera,            JceCameraComponent)
        M(JCE_COMP_FLAG_MESH_RENDERER,     jce_scene_get_mesh_renderer,     JceMeshRenderer)
        M(JCE_COMP_FLAG_SPRITE_RENDERER,   jce_scene_get_sprite_renderer,   JceSpriteRendererComponent)
        M(JCE_COMP_FLAG_RIGIDBODY,         jce_scene_get_rigidbody,         JceRigidBodyComponent)
        M(JCE_COMP_FLAG_BOX_COLLIDER,      jce_scene_get_box_collider,      JceBoxColliderComponent)
        M(JCE_COMP_FLAG_SPHERE_COLLIDER,   jce_scene_get_sphere_collider,   JceSphereColliderComponent)
        M(JCE_COMP_FLAG_CAPSULE_COLLIDER,  jce_scene_get_capsule_collider,  JceCapsuleColliderComponent)
        M(JCE_COMP_FLAG_MESH_COLLIDER,     jce_scene_get_mesh_collider,     JceMeshColliderComponent)
        M(JCE_COMP_FLAG_AUDIO_SOURCE,      jce_scene_get_audio_source,      JceAudioSourceComponent)
        M(JCE_COMP_FLAG_CONSTRAINT,        jce_scene_get_constraint,        JceConstraintComponent)
        M(JCE_COMP_FLAG_SKELETAL_ANIMATOR, jce_scene_get_skeletal_animator, JceSkeletalAnimatorComponent)
        default:
            if (out_size) *out_size = 0;
            return nullptr;
    }
#undef M
}

/* Wrap a single-component draw with a before/after byte diff and broadcast
 * the diff to every other selected entity that holds the same flag. */
static void draw_section_with_multi_broadcast(uint32_t focused,
                                              EditorEntitySidecar &sidecar,
                                              JceScene *scene,
                                              JceEntity ecs_e,
                                              uint64_t entry)
{
    int sel_count = 0;
    const uint32_t *sel = jce_state_get_selection(&sel_count);
    bool multi = (sel_count > 1) && multi_edit_supported(entry);

    void *focused_ptr = nullptr;
    size_t comp_size = 0;
    std::vector<uint8_t> before;
    if (multi) {
        focused_ptr = multi_get_comp_ptr(scene, ecs_e, entry, &comp_size);
        if (focused_ptr && comp_size > 0)
            before.assign((uint8_t *)focused_ptr,
                          (uint8_t *)focused_ptr + comp_size);
    }

    draw_one_component_section(focused, sidecar, scene, ecs_e, entry);

    if (!multi || !focused_ptr || comp_size == 0) return;
    if (memcmp(focused_ptr, before.data(), comp_size) == 0) return;

    /* Focused changed during this draw — broadcast the new bytes to peers. */
    for (int i = 0; i < sel_count; ++i) {
        uint32_t other = sel[i];
        if (other == focused) continue;
        JceEntity oe = jce_state_to_ecs_entity(other);
        if (!oe) continue;
        uint64_t oflags = jce_scene_get_component_flags(scene, oe);
        if (!(oflags & entry)) continue;
        size_t osize = 0;
        void *optr = multi_get_comp_ptr(scene, oe, entry, &osize);
        if (optr && osize == comp_size)
            memcpy(optr, focused_ptr, comp_size);
    }
}

/* Apply a pending Move Up / Move Down menu action or drag-reorder drop.
 * Called once per inspector frame after the iteration loop. */
static void apply_pending_reorder(uint32_t focused_entity,
                                  EditorEntitySidecar &sidecar)
{
    /* Drag drop: on mouse release, move src before/after hover. */
    if (s_drag.active && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (s_drag.entity_id == focused_entity &&
            s_drag.src_flag != s_drag.hover_flag &&
            s_drag.hover_flag != 0) {
            auto &v = sidecar.component_order;
            auto it_src = std::find(v.begin(), v.end(), s_drag.src_flag);
            auto it_dst = std::find(v.begin(), v.end(), s_drag.hover_flag);
            if (it_src != v.end() && it_dst != v.end()) {
                jce_state_begin_batch_edit();
                uint64_t f = *it_src;
                size_t dst_idx = (size_t)(it_dst - v.begin());
                v.erase(it_src);
                if (dst_idx > (size_t)(it_src - v.begin())) dst_idx--;
                v.insert(v.begin() + dst_idx, f);
                jce_state_end_batch_edit();
            }
        }
        s_drag = { 0, 0, 0, false };
    }

    if (!s_pending_move.pending) return;
    if (s_pending_move.entity_id != focused_entity) {
        s_pending_move.pending = false;
        return;
    }

    auto &v = sidecar.component_order;
    auto it = std::find(v.begin(), v.end(), s_pending_move.src_flag);
    if (it != v.end()) {
        size_t idx = (size_t)(it - v.begin());
        if (s_pending_move.dir < 0 && idx > 0) {
            jce_state_begin_batch_edit();
            std::swap(v[idx], v[idx - 1]);
            jce_state_end_batch_edit();
        } else if (s_pending_move.dir > 0 && idx + 1 < v.size()) {
            jce_state_begin_batch_edit();
            std::swap(v[idx], v[idx + 1]);
            jce_state_end_batch_edit();
        }
    }
    s_pending_move = { 0, 0, 0, 0, false };
}

/* Single dispatch from a flag value to the matching draw_comp_X call.
 * Mirrors the historic per-flag if-block sequence verbatim so behaviour
 * is byte-identical except for ordering. Light is handled inline by the
 * caller (LIGHT_GROUP_BIT path). */
static void draw_one_component_section(uint32_t focused,
                                       EditorEntitySidecar &sidecar,
                                       JceScene *scene,
                                       JceEntity ecs_e,
                                       uint64_t flag)
{
    const char *nm = jce_comp_flag_display_name(flag);
    bool removable = (flag != JCE_COMP_FLAG_TRANSFORM);

#define JCE_DRAW(F, EXPR)                                                  \
    case F:                                                                \
        if (comp_section_begin(focused, sidecar, F, nm, removable)) EXPR;  \
        comp_section_end();                                                \
        break

    switch (flag) {
        JCE_DRAW(JCE_COMP_FLAG_TRANSFORM,
                 draw_comp_transform(focused, jce_scene_get_transform(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_CAMERA,
                 draw_comp_camera(jce_scene_get_camera(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_MESH_RENDERER,
                 draw_comp_mesh_renderer(jce_scene_get_mesh_renderer(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_SPRITE_RENDERER,
                 draw_comp_sprite_renderer(jce_scene_get_sprite_renderer(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_ANIMATOR,
                 draw_comp_animator(jce_scene_get_animator(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_SKELETAL_ANIMATOR,
                 draw_comp_skeletal_animator(jce_scene_get_skeletal_animator(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_RIGIDBODY,
                 draw_comp_rigidbody(jce_scene_get_rigidbody(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_BOX_COLLIDER,
                 draw_comp_box_collider(jce_scene_get_box_collider(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_SPHERE_COLLIDER,
                 draw_comp_sphere_collider(jce_scene_get_sphere_collider(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_CHARACTER_CONTROLLER,
                 draw_comp_character_controller(jce_scene_get_character_controller(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_AUDIO_SOURCE,
                 draw_comp_audio_source(jce_scene_get_audio_source(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_SCRIPT,
                 draw_comp_script(jce_scene_get_script(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_SKYBOX,
                 draw_comp_skybox(jce_scene_get_skybox(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_SPRITE_ANIMATOR,
                 draw_comp_sprite_animator(jce_scene_get_sprite_animator(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_CONSTRAINT,
                 draw_comp_constraint(jce_scene_get_constraint(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_TERRAIN,
                 draw_comp_terrain(jce_scene_get_terrain(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_RIGIDBODY_2D,
                 draw_comp_rigidbody2d(jce_scene_get_rigidbody2d(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_PARTICLE_EMITTER,
                 draw_comp_particle_emitter(jce_scene_get_particle_emitter(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_BEHAVIOR_TREE,
                 draw_comp_behavior_tree(jce_scene_get_behavior_tree(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_LOD_GROUP,
                 draw_comp_lod_group(jce_scene_get_lod_group(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_VIRTUAL_CAMERA,
                 draw_comp_virtual_camera(jce_scene_get_virtual_camera(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_TRIGGER_VOLUME,
                 draw_comp_trigger_volume(jce_scene_get_trigger_volume(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_CAPSULE_COLLIDER,
                 draw_comp_capsule_collider(jce_scene_get_capsule_collider(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_MESH_COLLIDER,
                 draw_comp_mesh_collider(jce_scene_get_mesh_collider(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_COLLIDER_2D,
                 draw_comp_collider2d(jce_scene_get_collider2d(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_TRAIL_RENDERER,
                 draw_comp_trail_renderer(jce_scene_get_trail_renderer(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_LINE_RENDERER,
                 draw_comp_line_renderer(jce_scene_get_line_renderer(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_REFLECTION_PROBE,
                 draw_comp_reflection_probe(jce_scene_get_reflection_probe(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_DECAL,
                 draw_comp_decal(jce_scene_get_decal(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_LIGHT_PROBE_GROUP,
                 draw_comp_light_probe_group(jce_scene_get_light_probe_group(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_AUDIO_LISTENER,
                 draw_comp_audio_listener(jce_scene_get_audio_listener(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_AUDIO_REVERB_ZONE,
                 draw_comp_audio_reverb_zone(jce_scene_get_audio_reverb_zone(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_AUDIO_OCCLUSION,
                 draw_comp_audio_occlusion(jce_scene_get_audio_occlusion(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_SPAWN_MANAGER,
                 draw_comp_spawn_manager(jce_scene_get_spawn_manager(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_WEAPON,
                 draw_comp_weapon(jce_scene_get_weapon(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_SAVE_POINT,
                 draw_comp_save_point(jce_scene_get_save_point(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_WHEEL_COLLIDER,
                 draw_comp_wheel_collider(jce_scene_get_wheel_collider(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_CONSTANT_FORCE,
                 draw_comp_constant_force(jce_scene_get_constant_force(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_CONFIGURABLE_JOINT,
                 draw_comp_configurable_joint(jce_scene_get_configurable_joint(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_JOINT_2D,
                 draw_comp_joint2d(jce_scene_get_joint2d(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_BILLBOARD_RENDERER,
                 draw_comp_billboard_renderer(jce_scene_get_billboard_renderer(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_CANVAS,
                 draw_comp_canvas(jce_scene_get_canvas(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_CANVAS_GROUP,
                 draw_comp_canvas_group(jce_scene_get_canvas_group(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_LAYOUT_GROUP,
                 draw_comp_layout_group(jce_scene_get_layout_group(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_UI_IMAGE,
                 draw_comp_ui_image(jce_scene_get_ui_image(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_UI_TEXT,
                 draw_comp_ui_text(jce_scene_get_ui_text(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_UI_BUTTON,
                 draw_comp_ui_button(jce_scene_get_ui_button(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_CLOTH,
                 draw_comp_cloth(jce_scene_get_cloth(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_NET_TRANSFORM,
                 draw_comp_net_transform(jce_scene_get_net_transform(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_NET_ANIMATOR,
                 draw_comp_net_animator(jce_scene_get_net_animator(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_NET_RIGIDBODY,
                 draw_comp_net_rigidbody(jce_scene_get_net_rigidbody(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_VFX_GRAPH,
                 draw_comp_vfx_graph(jce_scene_get_vfx_graph(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_TILEMAP,
                 draw_comp_tilemap(jce_scene_get_tilemap(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_TILEMAP_COLLIDER_2D,
                 draw_comp_tilemap_collider2d(jce_scene_get_tilemap_collider2d(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_AVATAR,
                 draw_comp_avatar(jce_scene_get_avatar(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_VOLUME,
                 draw_comp_volume(jce_scene_get_volume(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_OCCLUSION_PORTAL,
                 draw_comp_occlusion_portal(jce_scene_get_occlusion_portal(scene, ecs_e)));
        default: break;
    }
#undef JCE_DRAW
}

/* ── Material file sync ───────────────────────────────────────────── */

void jce_editor_inspector_reload_material(const char *material_path)
{
    if (!material_path || material_path[0] == '\0') return;

    JceScene *scene = jce_state_get_scene();
    if (!scene) return;

    int total = jce_state_get_entity_count();
    for (int i = 0; i < total; i++) {
        uint32_t id = jce_state_get_entity_id_by_index(i);
        if (!id) continue;
        JceEntity e = jce_state_to_ecs_entity(id);
        if (!jce_scene_has_mesh_renderer(scene, e)) continue;
        JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
        if (mr && strcmp(mr->material_path, material_path) == 0)
            load_material_into_renderer(mr);
    }
}

/* ── Add Component options + popup live in inspector_add_component.cpp ── */

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_inspector_content(void)
{
    ensure_init();

    JceScene *scene = jce_state_get_scene();

    /* Play-mode warning: edits in PLAY mode will be reverted on STOP. */
    {
        JcePlayState ps = jce_state_get_play_state();
        if (ps == JCE_PLAY_PLAYING || ps == JCE_PLAY_PAUSED) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.55f, 0.10f, 1.0f));
            ImGui::TextWrapped("%s", jce_editor_i18n("inspector.playModeWarning"));
            ImGui::PopStyleColor();
            ImGui::Separator();
        }
    }

    /* ── Multi-entity selection short-circuit ──────────────────────── */
    if (insp_draw_multi_select_view(scene))
        return;

    uint32_t focused = jce_state_get_focused();
    if (!focused || !jce_state_entity_exists(focused) || !scene) {
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.noSelection"));
        return;
    }

    const char *ent_name    = jce_state_entity_name(focused);
    bool        ent_enabled = jce_state_entity_enabled(focused);
    const char *ent_tag     = jce_state_entity_tag(focused);
    JceTagColor ent_tcolor  = jce_state_entity_tag_color(focused);

    if (s_insp.needs_sync) {
        snprintf(s_insp.name_buf, sizeof(s_insp.name_buf), "%s", ent_name ? ent_name : "");
        snprintf(s_insp.tag_buf,  sizeof(s_insp.tag_buf),  "%s", ent_tag  ? ent_tag  : "");
        s_insp.needs_sync = false;
    }

    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 50);
    if (ImGui::InputText("##name", s_insp.name_buf, sizeof(s_insp.name_buf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        jce_state_rename_entity(focused, s_insp.name_buf);
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
    if (ImGui::Button(jce_editor_i18n("inspector.delete")))
        jce_editor_inspector_request_delete_confirm(focused);
    ImGui::PopStyleColor(2);

    bool enabled = ent_enabled;
    {
        char _lbl[256];
        snprintf(_lbl, sizeof(_lbl), "%s###enabled", jce_editor_i18n("inspector.enabled"));
        if (ImGui::Checkbox(_lbl, &enabled))
            jce_state_set_entity_enabled(focused, enabled);
    }
    ImGui::SameLine();

    int tag_color = (int)ent_tcolor;
    float combo_w = ImGui::GetContentRegionAvail().x;
    ImGui::PushItemWidth(combo_w);
    {
        char _lbl[256];
        snprintf(_lbl, sizeof(_lbl), "%s###TagColor", jce_editor_i18n("inspector.tagColor"));
        static const char *kTagColors[] = {
            "tagColor.none", "tagColor.red", "tagColor.orange", "tagColor.yellow",
            "tagColor.green", "tagColor.blue", "tagColor.purple", "tagColor.gray"
        };
        if (ImGui::Combo(_lbl, &tag_color, jce_editor_i18n_combo(kTagColors, 8)))
            jce_state_set_entity_tag_color(focused, (JceTagColor)tag_color);
    }
    ImGui::PopItemWidth();

    ImGui::PushItemWidth(-1);
    {
        const JceProjectSettings *ps = jce_project_settings_current();
        char _lbl[256];
        snprintf(_lbl, sizeof(_lbl), "%s###tag", jce_editor_i18n("inspector.tag"));

        /* Tag field — InputText so users can type a new tag inline (Enter
           commits and registers it in project settings). A small "▾"
           button next to the field opens a dropdown of existing tags. */
        const float arrow_w = ImGui::GetFrameHeight();
        const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float label_w = ImGui::CalcTextSize(jce_editor_i18n("inspector.tag")).x;
        const float field_w = ImGui::GetContentRegionAvail().x - arrow_w - spacing - label_w - spacing;
        ImGui::PushItemWidth(field_w > 80.0f ? field_w : 80.0f);
        bool tag_commit = ImGui::InputTextWithHint(
            "##tagInput",
            jce_editor_i18n_or("inspector.tagAdd.untagged", "Untagged"),
            s_insp.tag_buf, sizeof(s_insp.tag_buf),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();
        if (tag_commit) {
            /* Register the typed tag if it's new. */
            if (s_insp.tag_buf[0] != '\0' && ps) {
                bool exists = false;
                for (int i = 0; i < ps->tags_layers.tag_count && i < JCE_PS_MAX_TAGS; i++) {
                    if (strcmp(ps->tags_layers.tags[i], s_insp.tag_buf) == 0) {
                        exists = true; break;
                    }
                }
                if (!exists && ps->tags_layers.tag_count < JCE_PS_MAX_TAGS) {
                    JceProjectSettings *pm = (JceProjectSettings *)ps;
                    snprintf(pm->tags_layers.tags[pm->tags_layers.tag_count],
                             JCE_PS_NAME_LEN, "%s", s_insp.tag_buf);
                    pm->tags_layers.tag_count++;
                    jce_project_settings_save(pm);
                }
            }
            jce_state_set_entity_tag(focused, s_insp.tag_buf);
            jce_scene_set_entity_tag_name(scene,
                jce_state_to_ecs_entity(focused), s_insp.tag_buf);
        }
        ImGui::SameLine(0.0f, spacing);
        if (ImGui::ArrowButton("##tagPick", ImGuiDir_Down))
            ImGui::OpenPopup("##tagDropdown");
        ImGui::SameLine(0.0f, spacing);
        ImGui::TextUnformatted(jce_editor_i18n("inspector.tag"));

        if (ImGui::BeginPopup("##tagDropdown")) {
            if (ImGui::Selectable(jce_editor_i18n("inspector.tagAdd.untagged"),
                                   s_insp.tag_buf[0] == '\0')) {
                s_insp.tag_buf[0] = '\0';
                jce_state_set_entity_tag(focused, s_insp.tag_buf);
                jce_scene_set_entity_tag_name(scene,
                    jce_state_to_ecs_entity(focused), s_insp.tag_buf);
            }
            if (ps) {
                for (int i = 0; i < ps->tags_layers.tag_count && i < JCE_PS_MAX_TAGS; i++) {
                    const char *t = ps->tags_layers.tags[i];
                    if (!t || !*t) continue;
                    bool sel = (strcmp(s_insp.tag_buf, t) == 0);
                    if (ImGui::Selectable(t, sel)) {
                        snprintf(s_insp.tag_buf, sizeof(s_insp.tag_buf), "%s", t);
                        jce_state_set_entity_tag(focused, s_insp.tag_buf);
                        jce_scene_set_entity_tag_name(scene,
                            jce_state_to_ecs_entity(focused), s_insp.tag_buf);
                    }
                }
            }
            ImGui::EndPopup();
        }

        /* Layer picker — combo from project layers (32 slots, 0..7 builtin). */
        JceEditorMeta *meta_for_layer = jce_scene_get_editor_meta(scene,
                                            jce_state_to_ecs_entity(focused));
        int cur_layer = meta_for_layer ? meta_for_layer->layer : 0;
        if (cur_layer < 0 || cur_layer > 31) cur_layer = 0;
        char layer_label[256];
        snprintf(layer_label, sizeof(layer_label), "%s###layer",
                 jce_editor_i18n_or("inspector.layer", "Layer"));
        const char *cur_layer_name = "Default";
        if (ps && ps->tags_layers.layers[cur_layer][0] != '\0')
            cur_layer_name = ps->tags_layers.layers[cur_layer];
        if (ImGui::BeginCombo(layer_label, cur_layer_name)) {
            /* Show only NAMED layers (Unity convention). Slot 0 falls
               back to "Default" even when its name field is empty;
               unnamed user slots are hidden — manage them in
               Project Settings → Tags & Layers. */
            for (int i = 0; i < 32; i++) {
                const char *nm = NULL;
                if (ps && ps->tags_layers.layers[i][0] != '\0')
                    nm = ps->tags_layers.layers[i];
                else if (i == 0)
                    nm = "Default";
                if (!nm) continue;
                if (ImGui::Selectable(nm, i == cur_layer)) {
                    jce_state_set_entity_layer(focused, i);
                    jce_scene_set_entity_layer(scene,
                        jce_state_to_ecs_entity(focused), (uint8_t)i);
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::PopItemWidth();

    ImGui::Separator();

    if (!ent_enabled) {
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.entityDisabled"));
        ImGui::Spacing();
    }

    ImGui::BeginDisabled(!ent_enabled);

    /* ── Multi-selection banner ──────────────────────────────────── */
    {
        int sel_count = 0;
        jce_state_get_selection(&sel_count);
        if (sel_count > 1) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::GetColorU32(ImGuiCol_HeaderHovered));
            ImGui::Text(jce_editor_i18n("inspector.multiEditTitle"), sel_count);
            ImGui::PopStyleColor();
            ImGui::TextDisabled("%s", jce_editor_i18n("inspector.multiEditHint"));
            ImGui::Separator();
        }
    }

    /* ── Components ───────────────────────────────────────────────── */
    JceEntity ecs_e = jce_state_to_ecs_entity(focused);
    uint64_t flags = jce_scene_get_component_flags(scene, ecs_e);
    EditorEntitySidecar &sidecar = g_entity_sidecar[focused];

    sync_component_order(sidecar, flags);

    /* Iterate components in user-defined display order. The dispatcher
     * delegates to the same comp_section_begin / draw_comp_X / end
     * sequence the previous code used per-flag. */
    bool light_drawn = false;
    for (uint64_t entry : sidecar.component_order) {
        if (entry == LIGHT_GROUP_BIT) {
            if (light_drawn) continue;
            if (!(flags & LIGHT_MASK)) continue;
            uint64_t lf = (flags & JCE_COMP_FLAG_DIR_LIGHT)   ? JCE_COMP_FLAG_DIR_LIGHT
                       : (flags & JCE_COMP_FLAG_POINT_LIGHT) ? JCE_COMP_FLAG_POINT_LIGHT
                                                              : JCE_COMP_FLAG_SPOT_LIGHT;
            if (comp_section_begin(focused, sidecar, lf, "Light", true))
                draw_comp_light(scene, ecs_e, flags);
            comp_section_end();
            light_drawn = true;
            continue;
        }
        if (!(flags & entry)) continue;
        draw_section_with_multi_broadcast(focused, sidecar, scene, ecs_e, entry);
    }

    apply_pending_reorder(focused, sidecar);


    insp_add_component_button_and_popup(focused, flags);

    ImGui::EndDisabled();

    /* Flush deferred component removal here, after all draw_comp_*
     * functions have returned (so no stale flecs pointer is in use). */
    if (s_pending_remove.pending) {
        uint32_t eid = s_pending_remove.entity_id;
        uint64_t fl  = s_pending_remove.flag;
        s_pending_remove.pending   = false;
        s_pending_remove.entity_id = 0;
        s_pending_remove.flag      = 0;
        jce_state_remove_component(eid, fl);
    }
}

/* ── Standalone wrapper ───────────────────────────────────────────── */

void jce_editor_panel_inspector(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_INSPECTOR);
    if (!*vis) return;

    char title[256];
    snprintf(title, sizeof(title), "%s###inspector", jce_editor_i18n("Inspector"));
    if (ImGui::Begin(title, vis, ImGuiWindowFlags_NoFocusOnAppearing))
        jce_editor_panel_inspector_content();
    ImGui::End();
}

/* ── Top-level delete confirmation dialog ─────────────────────────── */

void jce_editor_inspector_delete_dialog(void)
{
    if (!s_insp.delete_requested) return;
    if (s_insp.delete_entity_count <= 0) {
        s_insp.delete_requested = false;
        return;
    }

    const ImGuiViewport *vp = ImGui::GetMainViewport();

    const char *popup_id = "###ConfirmDeleteEntityDlg";
    if (s_insp.delete_requested && !ImGui::IsPopupOpen(popup_id))
        ImGui::OpenPopup(popup_id);

    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowViewport(vp->ID);

    bool keep_open = s_insp.delete_requested;

    char title[256];
    snprintf(title, sizeof(title), "%s%s", jce_editor_i18n("dialog.confirmDelete"), popup_id);
    if (!ImGui::BeginPopupModal(title,
                                        &keep_open,
                      ImGuiWindowFlags_NoCollapse
                    | ImGuiWindowFlags_NoDocking
                    | ImGuiWindowFlags_AlwaysAutoResize)) {
        s_insp.delete_requested = keep_open;
        return;
    }

    if (s_insp.delete_entity_count == 1)
        ImGui::TextUnformatted(jce_editor_i18n("inspector.confirmDelete"));
    else
        ImGui::Text("%s: %d",
                    jce_editor_i18n("inspector.confirmDeleteMultiple"),
                    s_insp.delete_entity_count);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float btn_w = 140.0f;
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.3f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
    if (ImGui::Button(jce_editor_i18n("inspector.yes"), ImVec2(btn_w, 0))) {
        uint32_t ids[JCE_MAX_SELECTED];
        int n = s_insp.delete_entity_count;
        if (n > JCE_MAX_SELECTED) n = JCE_MAX_SELECTED;
        for (int i = 0; i < n; i++)
            ids[i] = s_insp.delete_entity_ids[i];

        if (n > 1) jce_state_begin_batch_edit();
        for (int i = 0; i < n; i++) {
            /* Skip ids that were already cascade-deleted by a prior
             * iteration (when a parent in the selection took its
             * descendants with it via flecs ChildOf cascade). */
            if (!jce_state_entity_exists(ids[i])) continue;
            jce_state_delete_entity(ids[i]);
        }
        if (n > 1) jce_state_end_batch_edit();

        keep_open = false;
        ImGui::PopStyleColor(3);
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        s_insp.delete_requested = keep_open;
        if (!keep_open) s_insp.delete_entity_count = 0;
        return;
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("inspector.no"), ImVec2(btn_w, 0))
        || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        ImGui::CloseCurrentPopup();
        keep_open = false;
    }

    ImGui::EndPopup();
    s_insp.delete_requested = keep_open;
    if (!keep_open) s_insp.delete_entity_count = 0;
}
