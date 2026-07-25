/*
 * jce_panel_inspector_common.cpp
 *
 * Shared mutable state and helper functions used by all inspector TUs.
 */

#include "jce_panel_inspector_common.h"
#include "ui/jce_editor_dnd.h"

/* ── Shared mutable state ─────────────────────────────────────────── */

InspCompClipboard s_comp_clipboard = { JCE_COMP_ID_INVALID, {0}, 0 };
bool              s_insp_batch_open = false;
InspPendingRemove s_pending_remove  = { 0, JCE_COMP_ID_INVALID, false };
char              s_preset_save_buf[64] = { 0 };
InspPendingMove   s_pending_move    = { 0, JCE_COMP_ID_INVALID, 0, false };
InspDrag          s_drag            = { 0, JCE_COMP_ID_INVALID,
                                        JCE_COMP_ID_INVALID, false };

/* ── Undo tracking helpers ─────────────────────────────────────────── */

void insp_track_edit(void)
{
    if (ImGui::IsItemActivated() && !s_insp_batch_open) {
        jce_state_begin_batch_edit();
        s_insp_batch_open = true;
    }
    if (ImGui::IsItemDeactivated() && s_insp_batch_open) {
        jce_state_end_batch_edit();
        s_insp_batch_open = false;
    }
}

/* Honesty badge for components the engine does not simulate yet: the data
 * round-trips through the scene file but has no runtime effect, so warn at
 * the top of the section instead of letting users author into the void
 * (editor-coverage audit 2026-06-11). Remove the call when a component's
 * runtime consumer lands. */
void insp_unwired_badge(void)
{
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 180, 60, 255));
    ImGui::TextWrapped("%s", jce_editor_i18n("inspector.unwiredBadge"));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("inspector.unwiredBadge.tip"));
}

void insp_undo_bool(bool *value)
{
    bool now = *value;
    *value = !now;
    jce_state_begin_batch_edit();
    *value = now;
    jce_state_end_batch_edit();
}

void insp_undo_int(int *value, int prev)
{
    int now = *value;
    *value = prev;
    jce_state_begin_batch_edit();
    *value = now;
    jce_state_end_batch_edit();
}

/* ── Vec3 control (colored XYZ drag floats) ───────────────────────── */

void draw_vec3_control(const char *label,
                       float *values,
                       float speed,
                       float reset_value)
{
    ImGui::PushID(label);

    float line_h = ImGui::GetFrameHeight();
    ImVec2 btn_size = ImVec2(line_h + 3.0f, line_h);
    float width = (ImGui::CalcItemWidth() - btn_size.x * 3.0f -
                   ImGui::GetStyle().ItemInnerSpacing.x * 2.0f) / 3.0f;

    ImGui::PushStyleColor(ImGuiCol_Button,        JCE_COLOR_INSP_VEC_X);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
    if (ImGui::Button("X", btn_size)) {
        jce_state_begin_batch_edit();
        values[0] = reset_value;
        jce_state_end_batch_edit();
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::PushItemWidth(width);
    ImGui::DragFloat("##X", &values[0], speed);
    insp_track_edit();
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,        JCE_COLOR_INSP_VEC_Y);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.2f, 0.9f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.1f, 0.8f, 0.1f, 1.0f));
    if (ImGui::Button("Y", btn_size)) {
        jce_state_begin_batch_edit();
        values[1] = reset_value;
        jce_state_end_batch_edit();
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::PushItemWidth(width);
    ImGui::DragFloat("##Y", &values[1], speed);
    insp_track_edit();
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,        JCE_COLOR_INSP_VEC_Z);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.2f, 0.2f, 0.9f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.1f, 0.1f, 0.8f, 1.0f));
    if (ImGui::Button("Z", btn_size)) {
        jce_state_begin_batch_edit();
        values[2] = reset_value;
        jce_state_end_batch_edit();
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::PushItemWidth(width);
    ImGui::DragFloat("##Z", &values[2], speed);
    insp_track_edit();
    ImGui::PopItemWidth();

    ImGui::PopID();
}

/* ── Asset drag-drop target for path fields ───────────────────────── */

/* Drag-drop payloads carry the asset's absolute disk path, but every
 * inspector path field stores a project-relative VFS path (matching
 * the asset picker contract).  Normalize here so dropped paths never
 * leak the user's full disk layout into a scene/prefab. */
static void copy_payload_as_relative(char *buf, size_t buf_size,
                                     const void *payload_data)
{
    char rel[1024];
    jce_editor_path_to_relative(rel, sizeof(rel),
                                (const char *)payload_data);
    snprintf(buf, buf_size, "%s",
             rel[0] ? rel : (const char *)payload_data);
}

void accept_asset_drop(char *buf, size_t buf_size)
{
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *payload =
                ImGui::AcceptDragDropPayload(JCE_DND_ASSET_PATH)) {
            copy_payload_as_relative(buf, buf_size, payload->Data);
        }
        ImGui::EndDragDropTarget();
    }
}

static bool is_mesh_ext(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return false;
    static const char *const exts[] = {
        ".fbx", ".FBX", ".glb", ".GLB", ".gltf", ".GLTF",
        ".obj", ".OBJ", ".mesh", ".MESH", ".dae", ".DAE", NULL
    };
    for (int i = 0; exts[i]; i++)
        if (strcmp(ext, exts[i]) == 0) return true;
    return false;
}

/* Post-drop half of the old accept_mesh_drop_with_material(): everything that
 * has to happen once a model path has been dropped onto the mesh field.
 *
 * Split out because jce_draw_path_input now owns the JCE_DND_ASSET_PATH target
 * (a second BeginDragDropTarget at the call site would land on the trailing
 * browse button instead of the text field).  The widget hands back the raw
 * ABSOLUTE path via JcePathInputOpts::dropped_raw, which is what assimp needs —
 * mr->mesh_path itself has already been relativized by the widget. */
void apply_mesh_drop_material(JceMeshRenderer *mr, const char *abs_path)
{
    if (mr && abs_path && abs_path[0]) {
        {
            const char *path = abs_path;
            mr->mesh_shape = 0;

            if (is_mesh_ext(path)) {
                JceEditorMaterialInfo mat = {};
                if (jce_editor_model_extract_material(path, &mat)) {
                    /* Importer texture paths are CWD-relative — store the
                     * canonical project-relative form (see store_asset_ref). */
                    if (mat.albedo_tex[0])
                        jce_editor_path_store_asset_ref(mr->albedo_tex,
                            sizeof(mr->albedo_tex), mat.albedo_tex);
                    if (mat.mr_tex[0])
                        jce_editor_path_store_asset_ref(mr->mr_tex,
                            sizeof(mr->mr_tex), mat.mr_tex);
                    if (mat.normal_tex[0])
                        jce_editor_path_store_asset_ref(mr->normal_tex,
                            sizeof(mr->normal_tex), mat.normal_tex);
                    if (mat.ao_tex[0])
                        jce_editor_path_store_asset_ref(mr->ao_tex,
                            sizeof(mr->ao_tex), mat.ao_tex);
                    if (mat.emissive_tex[0])
                        jce_editor_path_store_asset_ref(mr->emissive_tex,
                            sizeof(mr->emissive_tex), mat.emissive_tex);

                    mr->base_color[0] = mat.base_color[0];
                    mr->base_color[1] = mat.base_color[1];
                    mr->base_color[2] = mat.base_color[2];
                    mr->base_color[3] = mat.base_color[3];
                    mr->metallic       = mat.metallic;
                    mr->roughness      = mat.roughness;
                    mr->emissive[0]    = mat.emissive[0];
                    mr->emissive[1]    = mat.emissive[1];
                    mr->emissive[2]    = mat.emissive[2];
                    mr->normal_scale   = mat.normal_scale;
                    mr->ao_strength    = mat.ao_strength;
                    mr->alpha_mode     = mat.alpha_mode;
                    mr->alpha_cutoff   = mat.alpha_cutoff;
                    mr->double_sided   = mat.double_sided;
                }
            }
        }
    }
}
