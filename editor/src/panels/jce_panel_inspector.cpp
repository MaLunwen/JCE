/*
 * jce_panel_inspector.cpp  Inspector panel (entity properties).
 * Extracted from jce_editor_panels.cpp.
 */

#include "jce_editor_panels.h"
#include "jce_editor_state.h"
#include "jce_editor_colors.h"
#include "jce_editor_i18n.h"

#include <imgui.h>
#include <stdio.h>
#include <string.h>

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

/* Called by hierarchy when selection changes. */
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

/* ── Vec3 control (colored XYZ drag floats) ───────────────────────── */

static void draw_vec3_control(const char *label, float *values, float speed = 0.1f)
{
    ImGui::PushID(label);

    float line_h = ImGui::GetFrameHeight();
    ImVec2 btn_size = ImVec2(line_h + 3.0f, line_h);
    float width = (ImGui::CalcItemWidth() - btn_size.x * 3.0f -
                   ImGui::GetStyle().ItemInnerSpacing.x * 2.0f) / 3.0f;

    /* X */
    ImGui::PushStyleColor(ImGuiCol_Button,        JCE_COLOR_INSP_VEC_X);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
    if (ImGui::Button("X", btn_size)) values[0] = 0.0f;
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::PushItemWidth(width);
    ImGui::DragFloat("##X", &values[0], speed);
    ImGui::PopItemWidth();

    /* Y */
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,        JCE_COLOR_INSP_VEC_Y);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.2f, 0.9f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.1f, 0.8f, 0.1f, 1.0f));
    if (ImGui::Button("Y", btn_size)) values[1] = 0.0f;
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::PushItemWidth(width);
    ImGui::DragFloat("##Y", &values[1], speed);
    ImGui::PopItemWidth();

    /* Z */
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,        JCE_COLOR_INSP_VEC_Z);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.2f, 0.2f, 0.9f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.1f, 0.1f, 0.8f, 1.0f));
    if (ImGui::Button("Z", btn_size)) values[2] = 0.0f;
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::PushItemWidth(width);
    ImGui::DragFloat("##Z", &values[2], speed);
    ImGui::PopItemWidth();

    ImGui::PopID();
}

/* ── Component editor ─────────────────────────────────────────────── */

static void draw_component(JceComponentInfo *comp, uint32_t entity_id)
{
    const char *name = jce_component_type_name(comp->type);
    ImGui::PushID((int)comp->type);

    ImGui::PushStyleColor(ImGuiCol_Header, JCE_COLOR_INSP_HEADER);
    bool open = ImGui::CollapsingHeader(name, ImGuiTreeNodeFlags_DefaultOpen |
                                               ImGuiTreeNodeFlags_AllowOverlap);

    float header_w = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(header_w - 20);
    if (ImGui::SmallButton("..."))
        ImGui::OpenPopup("ComponentSettings");

    if (ImGui::BeginPopup("ComponentSettings")) {
        if (ImGui::MenuItem(jce_editor_i18n("transform.reset")))
            jce_editor_console_log("Reset %s (stub)", name);
        if (comp->type != JCE_COMP_TRANSFORM) {
            ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_ERROR);
            if (ImGui::MenuItem(jce_editor_i18n("inspector.removeComponent")))
                jce_state_remove_component(entity_id, comp->type);
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();

    if (open) {
        switch (comp->type) {
        case JCE_COMP_TRANSFORM:
            ImGui::Text("%s", jce_editor_i18n("transform.position"));
            ImGui::SameLine(80);
            draw_vec3_control("Position", comp->data.transform.pos);
            ImGui::Text("%s", jce_editor_i18n("transform.rotation"));
            ImGui::SameLine(80);
            draw_vec3_control("Rotation", comp->data.transform.rot, 1.0f);
            ImGui::Text("%s", jce_editor_i18n("transform.scale"));
            ImGui::SameLine(80);
            draw_vec3_control("Scale", comp->data.transform.scale, 0.01f);
            break;

        case JCE_COMP_LIGHT:
            {
                char _lbl[256];
                snprintf(_lbl, sizeof(_lbl), "%s###Color", jce_editor_i18n("light.color"));
                ImGui::ColorEdit4(_lbl, comp->data.light.color);
            }
            {
                char _lbl[256];
                snprintf(_lbl, sizeof(_lbl), "%s###Intensity", jce_editor_i18n("light.intensity"));
                ImGui::DragFloat(_lbl, &comp->data.light.intensity, 0.1f, 0.0f, 100.0f);
            }
            {
                const char *light_types[] = { "Directional", "Point", "Spot" };
                char _lbl[256];
                snprintf(_lbl, sizeof(_lbl), "%s###Type", jce_editor_i18n("light.type"));
                ImGui::Combo(_lbl, &comp->data.light.type, light_types, 3);
            }
            break;

        case JCE_COMP_CAMERA:
            {
                char _lbl[256];
                snprintf(_lbl, sizeof(_lbl), "%s###FOV", jce_editor_i18n("camera.fov"));
                ImGui::DragFloat(_lbl, &comp->data.camera.fov, 1.0f, 1.0f, 179.0f);
            }
            {
                char _lbl[256];
                snprintf(_lbl, sizeof(_lbl), "%s###Near", jce_editor_i18n("camera.nearClip"));
                ImGui::DragFloat(_lbl, &comp->data.camera.near_clip, 0.01f, 0.001f, 100.0f);
            }
            {
                char _lbl[256];
                snprintf(_lbl, sizeof(_lbl), "%s###Far", jce_editor_i18n("camera.farClip"));
                ImGui::DragFloat(_lbl, &comp->data.camera.far_clip, 1.0f, 1.0f, 100000.0f);
            }
            {
                char _lbl[256];
                snprintf(_lbl, sizeof(_lbl), "%s###Orthographic", jce_editor_i18n("camera.orthographic"));
                ImGui::Checkbox(_lbl, &comp->data.camera.ortho);
            }
            break;

        case JCE_COMP_MESH_RENDERER:
            ImGui::TextColored(JCE_COLOR_INSP_LABEL, "%s", jce_editor_i18n("meshRenderer.mesh"));
            ImGui::SameLine();
            ImGui::Text("%s", comp->data.mesh_renderer.mesh_path);
            ImGui::TextColored(JCE_COLOR_INSP_LABEL, "%s", jce_editor_i18n("meshRenderer.materials"));
            ImGui::SameLine();
            ImGui::Text("%s", comp->data.mesh_renderer.material_path);
            break;

        default:
            ImGui::TextDisabled("(Properties not yet implemented)");
            break;
        }
        ImGui::Spacing();
    }

    ImGui::PopID();
}

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_inspector_content(void)
{
    ensure_init();

    uint32_t focused = jce_state_get_focused();
    JceEntityInfo *e = focused ? jce_state_get_entity(focused) : NULL;

    if (!e) {
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.noSelection"));
        return;
    }

    /* Entity header: name + delete button */
    if (s_insp.needs_sync) {
        snprintf(s_insp.name_buf, sizeof(s_insp.name_buf), "%s", e->name);
        snprintf(s_insp.tag_buf, sizeof(s_insp.tag_buf), "%s", e->tag);
        s_insp.needs_sync = false;
    }

    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 50);
    if (ImGui::InputText("##name", s_insp.name_buf, sizeof(s_insp.name_buf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        jce_state_rename_entity(e->id, s_insp.name_buf);
    ImGui::PopItemWidth();

    /* Delete button (red) — opens confirmation modal */
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
    if (ImGui::Button(jce_editor_i18n("inspector.delete")))
        jce_editor_inspector_request_delete_confirm(e->id);
    ImGui::PopStyleColor(2);

    /* Row 2: [✓ Enabled]  [Tag Color ▼] */
    bool enabled = e->enabled;
    {
        char _lbl[256];
        snprintf(_lbl, sizeof(_lbl), "%s###enabled", jce_editor_i18n("inspector.enabled"));
        if (ImGui::Checkbox(_lbl, &enabled))
            jce_state_set_entity_enabled(e->id, enabled);
    }
    ImGui::SameLine();

    int tag_color = (int)e->tag_color;
    float combo_w = ImGui::GetContentRegionAvail().x;
    ImGui::PushItemWidth(combo_w);
    {
        char _lbl[256];
        snprintf(_lbl, sizeof(_lbl), "%s###TagColor", jce_editor_i18n("inspector.tagColor"));
        if (ImGui::Combo(_lbl, &tag_color,
                          "None\0Red\0Orange\0Yellow\0Green\0Blue\0Purple\0Gray\0"))
            jce_state_set_entity_tag_color(e->id, (JceTagColor)tag_color);
    }
    ImGui::PopItemWidth();

    /* Row 3: Tag string input */
    ImGui::PushItemWidth(-1);
    {
        char _lbl[256];
        snprintf(_lbl, sizeof(_lbl), "%s###tag", jce_editor_i18n("inspector.tag"));
        bool tag_committed = false;
        if (ImGui::InputTextWithHint(_lbl, "Tag", s_insp.tag_buf,
                                      sizeof(s_insp.tag_buf),
                                      ImGuiInputTextFlags_EnterReturnsTrue)) {
            jce_state_set_entity_tag(e->id, s_insp.tag_buf);
            tag_committed = true;
        }

        if (!tag_committed && ImGui::IsItemDeactivatedAfterEdit())
            jce_state_set_entity_tag(e->id, s_insp.tag_buf);
    }
    ImGui::PopItemWidth();

    ImGui::Separator();

    if (!e->enabled) {
        ImGui::TextDisabled("Entity is disabled (hidden in scene)");
        ImGui::Spacing();
    }

    ImGui::BeginDisabled(!e->enabled);

    /* Components */
    JceComponentInfo comps[JCE_MAX_COMPONENTS];
    int comp_count = jce_state_get_components(e->id, comps, JCE_MAX_COMPONENTS);

    for (int i = 0; i < comp_count; i++)
        draw_component(&comps[i], e->id);

    /* Add Component button */
    ImGui::Spacing();
    float btn_w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button(jce_editor_i18n("inspector.addComponent"), ImVec2(btn_w, 0)))
        ImGui::OpenPopup("AddComponentPopup");

    if (ImGui::BeginPopup("AddComponentPopup")) {
        for (int t = 0; t < JCE_COMP_TYPE_COUNT; t++) {
            if (ImGui::MenuItem(jce_component_type_name((JceComponentType)t)))
                jce_state_add_component(e->id, (JceComponentType)t);
        }
        ImGui::EndPopup();
    }

    ImGui::EndDisabled();
}

/* ── Standalone wrapper ───────────────────────────────────────────── */

void jce_editor_panel_inspector(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_INSPECTOR);
    if (!*vis) return;

    if (ImGui::Begin("Inspector###Inspector", vis))
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

        /* Dialog window (dimmer is handled globally by jce_editor_layout). */
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowFocus();

        bool keep_open = s_insp.delete_requested;

    if (!ImGui::Begin("Delete?###ConfirmDeleteEntityDlg",
                                            &keep_open,
                      ImGuiWindowFlags_NoCollapse
                    | ImGuiWindowFlags_NoDocking
                    | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
                s_insp.delete_requested = keep_open;
        return;
    }

    if (s_insp.delete_entity_count == 1)
        ImGui::TextUnformatted(jce_editor_i18n("inspector.confirmDelete"));
    else
        ImGui::Text("Are you sure you want to delete %d selected entities?",
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
        if (n > JCE_MAX_SELECTED)
            n = JCE_MAX_SELECTED;
        for (int i = 0; i < n; i++)
            ids[i] = s_insp.delete_entity_ids[i];

        for (int i = 0; i < n; i++)
            jce_state_delete_entity(ids[i]);

        keep_open = false;
        ImGui::PopStyleColor(3);
        ImGui::End();
        s_insp.delete_requested = keep_open;
        if (!keep_open)
            s_insp.delete_entity_count = 0;
        return;
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("inspector.no"), ImVec2(btn_w, 0))
        || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        keep_open = false;
    }

    ImGui::End();
    s_insp.delete_requested = keep_open;
    if (!keep_open)
        s_insp.delete_entity_count = 0;
}
