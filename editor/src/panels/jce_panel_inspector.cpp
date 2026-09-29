/*
 * jce_panel_inspector.cpp  Inspector panel (entity properties) — dispatcher.
 *
 * All draw_comp_* functions have been moved to domain TUs:
 *   jce_panel_inspector_transform.cpp  jce_panel_inspector_lighting.cpp  jce_panel_inspector_render.cpp
 *   jce_panel_inspector_physics.cpp    jce_panel_inspector_physics2d.cpp jce_panel_inspector_animation.cpp
 *   jce_panel_inspector_audio.cpp      jce_panel_inspector_gameplay.cpp  jce_panel_inspector_ui.cpp
 * Shared state and small helpers live in jce_panel_inspector_common.cpp.
 */

#include "jce_panel_inspector_common.h"
#include "ui/jce_editor_modals.h"

/* Raw component blob — one engine registry call: comp_id → the row's
 * type-erased get accessor and struct_size. */
static void *comp_get_ptr_and_size(JceScene *scene, JceEntity e,
                                   int comp_id, size_t *out_size)
{
    *out_size = 0;
    if (!scene || comp_id == JCE_COMP_ID_INVALID) return NULL;
    uint32_t sz = 0;
    void *p = jce_scene_get_comp(scene, e, comp_id, &sz);
    *out_size = sz;
    return p;
}

/* Dense id of the essential Transform row (resolved once; ids are stable
 * for the process lifetime once the engine registry is populated). */
static int insp_transform_comp_id(void)
{
    static int cid = JCE_COMP_ID_INVALID;
    if (cid == JCE_COMP_ID_INVALID)
        cid = jce_component_find("Transform");
    return cid;
}

/* ── Prefab override indicator cache (F12 visual) ──────────────────────
 *
 * is_overridden loads + parses the source .prefab.json, so we compute the
 * whole override-name set ONCE per inspector frame for the focused entity
 * (jce_state_get_prefab_overrides does a single source load) and answer
 * per-component header queries from the cache.  Refreshed lazily when the
 * focused entity changes or when invalidated after Apply/Revert. */
namespace {
struct PrefabOverrideCache {
    uint32_t entity_id = 0;
    bool     is_instance = false;
    std::vector<std::string> names;   /* canonical overridden comp names */

    void refresh(uint32_t eid) {
        entity_id = eid;
        names.clear();
        is_instance = false;
        if (!eid || !jce_state_is_prefab_instance(eid)) return;
        is_instance = true;
        char buf[JCE_COMP_MAX][64];
        int n = jce_state_get_prefab_overrides(eid, buf, JCE_COMP_MAX);
        for (int i = 0; i < n; i++)
            names.emplace_back(buf[i]);
    }
    bool has(const char *comp_name) const {
        if (!comp_name) return false;
        for (const std::string &s : names)
            if (s == comp_name) return true;
        return false;
    }
    void invalidate() { entity_id = 0; }
};
PrefabOverrideCache s_override_cache;

/* Map a section's dense comp_id to the canonical override name.  The light
 * section is drawn with a CONCRETE light comp_id (DirectionalLight/...),
 * but overrides are tracked under the unified "Light" — coalesce here so
 * the badge + Apply/Revert use the name write_override_node emitted. */
const char *override_query_name(int comp_id)
{
    const char *n = jce_component_name(comp_id);
    if (jce_editor_component_id_is_light_group(comp_id)) return "Light";
    if (n && (strcmp(n, "DirectionalLight") == 0 ||
              strcmp(n, "PointLight") == 0 ||
              strcmp(n, "SpotLight") == 0 ||
              strcmp(n, "AreaLight") == 0))
        return "Light";
    return n;
}
} /* namespace */

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

/* Fold state: one bit per dense comp_id in sidecar.expanded[4] (256 bits,
 * default all-expanded) — every registered component gets a bit, including
 * the post-64 rows the old single-word flag mask could not represent. */
static bool comp_section_is_open(const EditorEntitySidecar &sidecar,
                                 int comp_id)
{
    if (comp_id < 0 || comp_id >= JCE_COMP_MAX)
        return true;
    return (sidecar.expanded[comp_id >> 6] >> (comp_id & 63)) & 1u;
}

static void comp_section_set_open(EditorEntitySidecar &sidecar,
                                  int comp_id,
                                  bool open)
{
    if (comp_id < 0 || comp_id >= JCE_COMP_MAX)
        return;
    uint64_t bit = UINT64_C(1) << (comp_id & 63);
    if (open)
        sidecar.expanded[comp_id >> 6] |= bit;
    else
        sidecar.expanded[comp_id >> 6] &= ~bit;
}

/* Returns true if the component's body should be drawn this frame.
 * Updates sidecar fold state.  Handles the "..." popup
 * with a Remove menu (disabled when not removable, e.g. Transform). */
/* Set while a disabled component's body is being drawn dimmed (Alpha pushed in
 * comp_section_begin, popped in comp_section_end). Sections are never nested. */
static bool s_comp_section_dimmed = false;

static bool comp_section_begin(uint32_t entity_id,
                               EditorEntitySidecar &sidecar,
                               int comp_id,
                               const char *display_name,
                               bool removable)
{
    ImGui::PushID(comp_id);

    bool was_open = comp_section_is_open(sidecar, comp_id);
    int tn_flags = ImGuiTreeNodeFlags_AllowOverlap |
                   (was_open ? ImGuiTreeNodeFlags_DefaultOpen : 0);

    /* Inspector collapsing-header tint:
       - Dark themes: keep the slate slate-blue accent (#323744) so it
         reads as a distinct band over the dark window bg.
       - Light themes: defer to ImGuiCol_Header so the bar tracks the
         active palette (avoids a near-black strip on white). */
    /* Per-component enable state (Transform is essential — always on). When
     * disabled, dim the section so it clearly reads as inactive (grey). */
    JceScene *_es = jce_state_get_scene();
    JceEntity _ee = jce_state_to_ecs_entity(entity_id);
    /* id-keyed enable state works for EVERY registered component — the old
     * 64-bit disabled mask could not toggle the post-64 (former synthetic)
     * rows.  Transform stays essential / always on. */
    bool comp_toggleable = comp_id != JCE_COMP_ID_INVALID &&
                           comp_id != insp_transform_comp_id();
    bool comp_disabled = comp_toggleable &&
                         !jce_scene_comp_enabled(_es, _ee, comp_id);

    ImGui::PushStyleColor(ImGuiCol_Header, jce_theme::inspector_header_color());
    if (comp_disabled)
        ImGui::PushStyleColor(ImGuiCol_Text, jce_theme::text_secondary());
    float hdr_x = ImGui::GetCursorPosX();
    bool open = ImGui::CollapsingHeader(display_name, tn_flags);
    if (comp_disabled)
        ImGui::PopStyleColor();
    comp_section_set_open(sidecar, comp_id, open);

    /* Prefab override indicator (F12 visual): a bold blue "*" badge after
     * the header name when this component differs from its source prefab.
     * The light-group header keys on the unified "Light" override name. */
    bool comp_overridden = false;
    if (s_override_cache.entity_id == entity_id && s_override_cache.is_instance)
        comp_overridden = s_override_cache.has(override_query_name(comp_id));
    if (comp_overridden) {
        ImGui::SameLine(hdr_x + ImGui::GetTreeNodeToLabelSpacing()
                        + ImGui::CalcTextSize(display_name).x + 4.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.36f, 0.66f, 1.0f, 1.0f));
        ImGui::TextUnformatted("*");
        ImGui::PopStyleColor();
        /* Tooltip: the base message PLUS the per-FIELD list of which
         * serialized keys diverge (e.g. "posX, scaleY").  Queried lazily on
         * hover only (it loads + diffs the source), so it costs nothing on
         * the common non-hovered path.  An empty list (the unified "Light",
         * whose rows can't be field-diffed) shows the base message alone —
         * that override is whole-component by design.  Full per-field-widget
         * revert is a documented follow-up; this surfaces the divergence. */
        if (ImGui::IsItemHovered()) {
            char fbuf[JCE_COMP_MAX][64];
            int fn = jce_state_get_prefab_field_overrides(
                entity_id, override_query_name(comp_id), fbuf, JCE_COMP_MAX);
            if (fn > 0) {
                std::string list;
                for (int fi = 0; fi < fn; fi++) {
                    if (fi) list += ", ";
                    list += fbuf[fi];
                }
                ImGui::SetTooltip("%s\n%s: %s",
                    jce_editor_i18n("inspector.prefabOverrideTip"),
                    jce_editor_i18n("inspector.prefabOverrideFields"),
                    list.c_str());
            } else {
                ImGui::SetTooltip("%s",
                    jce_editor_i18n("inspector.prefabOverrideTip"));
            }
        }
    }

    /* Drag-reorder: pressing & dragging a header begins a drag; while
     * active, hovering another header records it as the drop target. On
     * mouse release the loop applies the move. */
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
        if (!s_drag.active) {
            s_drag.active   = true;
            s_drag.entity_id = entity_id;
            s_drag.src_comp  = comp_id;
        }
    }
    if (s_drag.active && s_drag.entity_id == entity_id && ImGui::IsItemHovered()) {
        s_drag.hover_comp = comp_id;
        /* Visual cue: thin line above the hovered header. */
        ImVec2 mn = ImGui::GetItemRectMin();
        ImVec2 mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(mn.x, mn.y), ImVec2(mx.x, mn.y),
            ImGui::GetColorU32(ImGuiCol_DragDropTarget), 2.0f);
    }

    float header_w = ImGui::GetContentRegionAvail().x;

    /* Enable checkbox right after the component NAME (prominent), so it clearly
     * reads as that component's on/off switch. Transform is always enabled. */
    if (comp_toggleable) {
        /* A script that re-asserts this component's enable state every frame
         * overwrites anything set here within one frame.  Offering a live
         * checkbox in that case reads as a broken control, so say who owns it
         * and take the checkbox out of service instead. */
        const bool script_driven =
            jce_scene_comp_script_driven(_es, _ee, comp_id);

        float cb_x = hdr_x + ImGui::GetTreeNodeToLabelSpacing()
                   + ImGui::CalcTextSize(display_name).x + 12.0f;
        if (cb_x > header_w - 46.0f) cb_x = header_w - 46.0f;
        bool _en = !comp_disabled;
        ImGui::SameLine(cb_x);
        ImGui::BeginDisabled(script_driven);
        if (ImGui::Checkbox("##comp_enabled", &_en) && !script_driven)
            jce_scene_set_comp_enabled(_es, _ee, comp_id, _en);
        ImGui::EndDisabled();
        /* Tooltip has to be outside BeginDisabled: disabled items do not
         * report hover, and the explanation is exactly what is needed here. */
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", jce_editor_i18n(
                script_driven ? "inspector.componentScriptDrivenTip"
                              : "inspector.toggleComponentTip"));
        if (script_driven) {
            ImGui::SameLine(0.0f, 6.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, jce_theme::text_secondary());
            ImGui::TextUnformatted(
                jce_editor_i18n("inspector.componentScriptDriven"));
            ImGui::PopStyleColor();
        }
    }

    ImGui::SameLine(header_w - 20);
    if (ImGui::SmallButton("..."))
        ImGui::OpenPopup("ComponentSettings");

    if (ImGui::BeginPopup("ComponentSettings")) {
        JceScene *_cs = jce_state_get_scene();
        JceEntity _ce = jce_state_to_ecs_entity(entity_id);
        size_t _csz = 0;
        void *_cptr = comp_get_ptr_and_size(_cs, _ce, comp_id, &_csz);

        if (ImGui::MenuItem(jce_editor_i18n("inspector.copyComponent"),
                            NULL, false, _cptr != NULL && _csz > 0 && _csz <= sizeof(s_comp_clipboard.data))) {
            s_comp_clipboard.comp_id = comp_id;
            s_comp_clipboard.data_size = _csz;
            memcpy(s_comp_clipboard.data, _cptr, _csz);
        }
        bool can_paste = (s_comp_clipboard.comp_id == comp_id
                          && comp_id != JCE_COMP_ID_INVALID
                          && s_comp_clipboard.data_size > 0
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
            s_pending_move.src_comp  = comp_id;
            s_pending_move.dir       = -1;
            s_pending_move.pending   = true;
        }
        if (ImGui::MenuItem(jce_editor_i18n("inspector.moveDown"))) {
            s_pending_move.entity_id = entity_id;
            s_pending_move.src_comp  = comp_id;
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
            std::vector<std::string> names = jce_preset_list(comp_id);
            if (names.empty()) {
                ImGui::TextDisabled("%s", jce_editor_i18n("inspector.preset.empty"));
            } else {
                std::string pending_delete;
                for (const auto &n : names) {
                    if (ImGui::BeginMenu(n.c_str())) {
                        if (ImGui::MenuItem(jce_editor_i18n("inspector.preset.apply"))) {
                            jce_preset_apply(comp_id, n.c_str(), _cs, _ce);
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
                    jce_preset_delete(comp_id, pending_delete.c_str());
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
                if (comp_id == insp_transform_comp_id()) {
                    JceTransform *t = (JceTransform *)_cptr;
                    t->position = { 0.0f, 0.0f, 0.0f };
                    t->rotation = jce_q_identity();
                    t->scale    = { 1.0f, 1.0f, 1.0f };
                } else if (comp_id == jce_component_find("CompoundCollider")) {
                    jce_editor_component_compound_default(
                        (JceCompoundColliderComponent *)_cptr);
                } else if (comp_id == jce_component_find("VideoPlayer")) {
                    /* Reset only the authoring fields; do NOT zero the live
                     * decoder / texture handles here (that would orphan the GPU
                     * texture + decoder).  Emptying clip_path makes
                     * jce_scene_video_update() observe a clip_path/opened_hash
                     * mismatch next tick and release them cleanly. */
                    JceVideoPlayerComponent *vp =
                        (JceVideoPlayerComponent *)_cptr;
                    vp->clip_path[0] = '\0';
                    vp->loop         = false;
                    vp->autoplay     = true;
                    vp->playing      = false;
                } else {
                    memset(_cptr, 0, _csz);
                }
                jce_state_end_batch_edit();
            }
        }

        /* ── Prefab override: Apply to / Revert from source ──────────
         * Only meaningful for a prefab-instance entity.  The canonical
         * engine name for Apply/Revert is the unified "Light" for the
         * light group, else the component's registry name.  Disabled when
         * the component is not currently overridden (nothing to push/drop).
         * Calls are deferred-safe (they batch their own undo). */
        if (jce_state_is_prefab_instance(entity_id)) {
            ImGui::Separator();
            const char *pf_name = override_query_name(comp_id);
            bool is_ov = pf_name && s_override_cache.entity_id == entity_id
                         && s_override_cache.has(pf_name);
            if (!is_ov) ImGui::BeginDisabled();
            if (ImGui::MenuItem(jce_editor_i18n("inspector.applyToPrefab"))) {
                if (pf_name &&
                    jce_state_apply_prefab_component(entity_id, pf_name))
                    s_override_cache.invalidate();
            }
            if (ImGui::MenuItem(jce_editor_i18n("inspector.revertToPrefab"))) {
                if (pf_name &&
                    jce_state_revert_prefab_component(entity_id, pf_name))
                    s_override_cache.invalidate();
            }
            if (!is_ov) ImGui::EndDisabled();
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
                s_pending_remove.comp_id   = comp_id;
                s_pending_remove.pending   = true;
            }
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();

    /* Preset save modal — shared per-section so opening one closes others. */
    if (ImGui::BeginPopup("##preset_save_popup")) {
        const JceEditorComponentDescriptor *_pd =
            jce_editor_component_find_by_id(comp_id);
        ImGui::Text(jce_editor_i18n("inspector.preset.savePromptFmt"),
                    _pd ? _pd->display_name : display_name);
        ImGui::SetNextItemWidth(220);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool commit = ImGui::InputText("##preset_name", s_preset_save_buf,
                                       sizeof(s_preset_save_buf),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button(jce_editor_i18n("dialog.save")) || commit) {
            if (s_preset_save_buf[0]) {
                JceScene *_ps = jce_state_get_scene();
                JceEntity _pe = jce_state_to_ecs_entity(entity_id);
                jce_preset_save(comp_id, s_preset_save_buf, _ps, _pe);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("dialog.cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    /* Dim the component BODY while disabled (popped in comp_section_end). */
    if (open && comp_disabled) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.55f);
        s_comp_section_dimmed = true;
    }
    return open;
}

static void comp_section_end(void)
{
    if (s_comp_section_dimmed) {
        ImGui::PopStyleVar();        /* end the disabled-component dim */
        s_comp_section_dimmed = false;
    }
    ImGui::Spacing();
    ImGui::PopID();
}

/* ══════════════════════════════════════════════════════════════════════
 *  COMPONENT DISPLAY ORDER + DISPATCH
 *
 *  Inspector iterates components in user-controlled order stored in
 *  EditorEntitySidecar.component_order (dense engine comp_ids; the light
 *  group is the unified "Light" row id).
 * ══════════════════════════════════════════════════════════════════════ */

/* Ensures sidecar.component_order contains exactly the comp_ids we want
 * to draw, given the entity's current components:
 *   - Removes entries no longer present (component was removed).
 *   - Appends new entries in default-order positions (component added).
 *   - The three light components collapse into the unified "Light" row. */
static void sync_component_order(EditorEntitySidecar &sidecar,
                                 JceScene *scene,
                                 JceEntity entity,
                                 uint64_t flags)
{
    auto wanted = [&](int cid) -> bool {
        if (cid == JCE_COMP_ID_INVALID)
            return false;
        if (jce_editor_component_id_is_light_group(cid))
            return (flags & INSP_LIGHT_MASK) != 0;

        const JceEditorComponentDescriptor *desc =
            jce_editor_component_find_by_id(cid);
        if (desc && (desc->legacy_flag & INSP_LIGHT_MASK) != 0)
            return false;

        return jce_scene_has_comp(scene, entity, cid);
    };

    /* Drop stale entries while preserving order of survivors. */
    auto &v = sidecar.component_order;
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](int c) { return !wanted(c); }),
            v.end());

    /* Add any missing entries by walking the default order. */
    int n_order = jce_editor_component_default_order_count();
    for (int i = 0; i < n_order; i++) {
        int def = jce_editor_component_default_order_comp_id(i);
        if (!wanted(def)) continue;
        if (std::find(v.begin(), v.end(), def) == v.end())
            v.push_back(def);
    }

    /* Fallback: a component the entity HAS whose descriptor exists but is
     * missing from kDefaultOrderNames still gets a section (appended last).
     * A forgotten order entry must never silently hide a component — that
     * is exactly how FoliageCluster rendered as "Transform only" for every
     * ES bush/leaf entity. */
    int n_desc = jce_editor_component_descriptor_count();
    for (int i = 0; i < n_desc; i++) {
        const JceEditorComponentDescriptor *d =
            jce_editor_component_descriptor_at(i);
        if (!d || d->comp_id == JCE_COMP_ID_INVALID) continue;
        if (!wanted(d->comp_id)) continue;
        if (std::find(v.begin(), v.end(), d->comp_id) == v.end())
            v.push_back(d->comp_id);
    }
}

static void draw_one_component_section(uint32_t focused,
                                       EditorEntitySidecar &sidecar,
                                       JceScene *scene,
                                       JceEntity ecs_e,
                                       int comp_id);

/* ══════════════════════════════════════════════════════════════════════
 *  MULTI-OBJECT EDITING
 *  When >1 entities are selected, the focused entity's component sections
 *  still render (the multi-select view no longer early-returns).  Edits
 *  made through the focused entity's section are broadcast PER FIELD onto
 *  the other selected entities of the same type: we snapshot the focused
 *  component bytes before the draw, then after the draw copy ONLY the
 *  contiguous byte ranges that actually changed (i.e. the field the user
 *  just touched) — never a whole-struct memcpy, so fields that differ per
 *  entity are left untouched.  We restrict the broadcast to a whitelist of
 *  pure value-type components — string buffers and asset handles inside
 *  other components must be edited per-entity.
 * ══════════════════════════════════════════════════════════════════════ */

static bool multi_edit_supported(int comp_id)
{
    const JceEditorComponentDescriptor *desc =
        jce_editor_component_find_by_id(comp_id);
    return desc && desc->multi_edit_supported;
}

/* Raw blob for the broadcast paths — the engine registry's type-erased
 * accessor replaces the old 13-case getter switch (+ compound-collider
 * synthetic-slot branch).  The descriptor's `multi_edit_supported` flag
 * remains the SOLE whitelist gate (same 13 pure value-type components as
 * before); this helper is only reached for whitelisted rows. */
static void *multi_get_comp_ptr(JceScene *scene, JceEntity e,
                                int comp_id, size_t *out_size)
{
    size_t sz = 0;
    void *p = comp_get_ptr_and_size(scene, e, comp_id, &sz);
    if (out_size) *out_size = sz;
    return p;
}

/* ── Multi-select per-field broadcast: field reflection ──────────────────
 *
 * Industry-standard multi-object editing (Unity/Unreal/Godot) applies the
 * EDITED PROPERTY to every selected peer, leaving each peer's other properties
 * intact.  A byte-level before/after diff cannot do this safely: a DragFloat
 * nudge (e.g. 1.0 -> 1.5) changes only the low mantissa byte(s) of a float, so
 * splicing just those bytes onto a peer holding a different value (say 9.0)
 * produces garbage (-> 24.0), not the focused value.  Instead we map each
 * changed byte to the WHOLE struct field that contains it and copy that field
 * intact.
 *
 * The tables below list every editable member of each multi-edit component at
 * scalar granularity — math vectors / float arrays expanded per-axis (Unity
 * behaviour: nudging position.x broadcasts only x), and char[] strings copied
 * whole (never spliced).  Any changed byte not covered by a listed field falls
 * back to a 4-byte-aligned word copy, which is safe for 4-byte scalars (so an
 * accidentally-unlisted numeric field still broadcasts correctly). */
namespace {
struct InspField { uint16_t off, size; };
#define IF_F(T, m)         { (uint16_t)offsetof(T, m), (uint16_t)sizeof(((T*)0)->m) }
#define IF_AX(T, m, i)     { (uint16_t)(offsetof(T, m) + (i) * (uint16_t)sizeof(float)), (uint16_t)sizeof(float) }
#define IF_STR(T, m, i, n) { (uint16_t)(offsetof(T, m) + (i) * (n)), (uint16_t)(n) }

static const InspField kF_transform[] = {
    IF_AX(JceTransform, position, 0), IF_AX(JceTransform, position, 1), IF_AX(JceTransform, position, 2),
    IF_AX(JceTransform, rotation, 0), IF_AX(JceTransform, rotation, 1), IF_AX(JceTransform, rotation, 2), IF_AX(JceTransform, rotation, 3),
    IF_AX(JceTransform, scale, 0), IF_AX(JceTransform, scale, 1), IF_AX(JceTransform, scale, 2),
};
static const InspField kF_mesh_renderer[] = {
    IF_F(JceMeshRenderer, model), IF_F(JceMeshRenderer, shader), IF_F(JceMeshRenderer, visible),
    IF_F(JceMeshRenderer, mesh_path), IF_F(JceMeshRenderer, material_path), IF_F(JceMeshRenderer, mesh_shape),
    IF_AX(JceMeshRenderer, base_color, 0), IF_AX(JceMeshRenderer, base_color, 1), IF_AX(JceMeshRenderer, base_color, 2), IF_AX(JceMeshRenderer, base_color, 3),
    IF_F(JceMeshRenderer, metallic), IF_F(JceMeshRenderer, roughness),
    IF_AX(JceMeshRenderer, emissive, 0), IF_AX(JceMeshRenderer, emissive, 1), IF_AX(JceMeshRenderer, emissive, 2),
    IF_F(JceMeshRenderer, normal_scale), IF_F(JceMeshRenderer, ao_strength),
    IF_F(JceMeshRenderer, alpha_mode), IF_F(JceMeshRenderer, alpha_cutoff), IF_F(JceMeshRenderer, double_sided),
    IF_F(JceMeshRenderer, shadow_cast_off), IF_F(JceMeshRenderer, shadow_receive_off),
    IF_F(JceMeshRenderer, albedo_tex), IF_F(JceMeshRenderer, mr_tex), IF_F(JceMeshRenderer, normal_tex),
    IF_F(JceMeshRenderer, ao_tex), IF_F(JceMeshRenderer, emissive_tex),
};
static const InspField kF_camera[] = {
    IF_F(JceCameraComponent, fov_deg), IF_F(JceCameraComponent, near_plane), IF_F(JceCameraComponent, far_plane),
    IF_F(JceCameraComponent, is_primary), IF_F(JceCameraComponent, ortho),
    IF_F(JceCameraComponent, stack_index), IF_F(JceCameraComponent, clear_mode),
};
static const InspField kF_sprite_renderer[] = {
    IF_F(JceSpriteRendererComponent, sprite_path),
    IF_AX(JceSpriteRendererComponent, color, 0), IF_AX(JceSpriteRendererComponent, color, 1), IF_AX(JceSpriteRendererComponent, color, 2), IF_AX(JceSpriteRendererComponent, color, 3),
    IF_F(JceSpriteRendererComponent, flip_x), IF_F(JceSpriteRendererComponent, flip_y), IF_F(JceSpriteRendererComponent, sorting_order),
    IF_F(JceSpriteRendererComponent, sorting_layer),
};
static const InspField kF_skeletal[] = {
    IF_F(JceSkeletalAnimatorComponent, skeleton_path),
    IF_STR(JceSkeletalAnimatorComponent, clip_names, 0, 64), IF_STR(JceSkeletalAnimatorComponent, clip_names, 1, 64),
    IF_STR(JceSkeletalAnimatorComponent, clip_names, 2, 64), IF_STR(JceSkeletalAnimatorComponent, clip_names, 3, 64),
    IF_STR(JceSkeletalAnimatorComponent, clip_names, 4, 64), IF_STR(JceSkeletalAnimatorComponent, clip_names, 5, 64),
    IF_STR(JceSkeletalAnimatorComponent, clip_names, 6, 64), IF_STR(JceSkeletalAnimatorComponent, clip_names, 7, 64),
    IF_F(JceSkeletalAnimatorComponent, clip_count), IF_F(JceSkeletalAnimatorComponent, active_clip),
    IF_F(JceSkeletalAnimatorComponent, speed), IF_F(JceSkeletalAnimatorComponent, loop), IF_F(JceSkeletalAnimatorComponent, playing),
    IF_F(JceSkeletalAnimatorComponent, sm_path), IF_F(JceSkeletalAnimatorComponent, use_blend_tree), IF_F(JceSkeletalAnimatorComponent, blend_param),
    IF_AX(JceSkeletalAnimatorComponent, blend_thresholds, 0), IF_AX(JceSkeletalAnimatorComponent, blend_thresholds, 1),
    IF_AX(JceSkeletalAnimatorComponent, blend_thresholds, 2), IF_AX(JceSkeletalAnimatorComponent, blend_thresholds, 3),
    IF_AX(JceSkeletalAnimatorComponent, blend_thresholds, 4), IF_AX(JceSkeletalAnimatorComponent, blend_thresholds, 5),
    IF_AX(JceSkeletalAnimatorComponent, blend_thresholds, 6), IF_AX(JceSkeletalAnimatorComponent, blend_thresholds, 7),
};
static const InspField kF_constraint[] = {
    IF_F(JceConstraintComponent, constraint_type), IF_F(JceConstraintComponent, target_entity),
    IF_AX(JceConstraintComponent, pivot_a, 0), IF_AX(JceConstraintComponent, pivot_a, 1), IF_AX(JceConstraintComponent, pivot_a, 2),
    IF_AX(JceConstraintComponent, pivot_b, 0), IF_AX(JceConstraintComponent, pivot_b, 1), IF_AX(JceConstraintComponent, pivot_b, 2),
    IF_AX(JceConstraintComponent, axis, 0), IF_AX(JceConstraintComponent, axis, 1), IF_AX(JceConstraintComponent, axis, 2),
    IF_F(JceConstraintComponent, lower_limit), IF_F(JceConstraintComponent, upper_limit), IF_F(JceConstraintComponent, disable_collision),
};
static const InspField kF_rigidbody[] = {
    IF_F(JceRigidBodyComponent, body_handle_idx), IF_F(JceRigidBodyComponent, body_type), IF_F(JceRigidBodyComponent, shape_type),
    IF_F(JceRigidBodyComponent, mass), IF_F(JceRigidBodyComponent, friction), IF_F(JceRigidBodyComponent, restitution),
    IF_F(JceRigidBodyComponent, drag), IF_F(JceRigidBodyComponent, angular_drag),
    IF_F(JceRigidBodyComponent, use_gravity), IF_F(JceRigidBodyComponent, is_kinematic),
    IF_F(JceRigidBodyComponent, ccd_mode), IF_F(JceRigidBodyComponent, ccd_threshold), IF_F(JceRigidBodyComponent, ccd_sphere_radius),
    IF_F(JceRigidBodyComponent, gravity_scale), IF_F(JceRigidBodyComponent, physics_layer), IF_F(JceRigidBodyComponent, physmat_path),
};
static const InspField kF_box[] = {
    IF_AX(JceBoxColliderComponent, center, 0), IF_AX(JceBoxColliderComponent, center, 1), IF_AX(JceBoxColliderComponent, center, 2),
    IF_AX(JceBoxColliderComponent, size, 0), IF_AX(JceBoxColliderComponent, size, 1), IF_AX(JceBoxColliderComponent, size, 2),
    IF_F(JceBoxColliderComponent, is_trigger),
};
static const InspField kF_sphere[] = {
    IF_AX(JceSphereColliderComponent, center, 0), IF_AX(JceSphereColliderComponent, center, 1), IF_AX(JceSphereColliderComponent, center, 2),
    IF_F(JceSphereColliderComponent, radius), IF_F(JceSphereColliderComponent, is_trigger),
};
static const InspField kF_capsule[] = {
    IF_AX(JceCapsuleColliderComponent, center, 0), IF_AX(JceCapsuleColliderComponent, center, 1), IF_AX(JceCapsuleColliderComponent, center, 2),
    IF_F(JceCapsuleColliderComponent, radius), IF_F(JceCapsuleColliderComponent, height), IF_F(JceCapsuleColliderComponent, axis), IF_F(JceCapsuleColliderComponent, is_trigger),
};
static const InspField kF_mesh_collider[] = {
    IF_F(JceMeshColliderComponent, mesh_path), IF_F(JceMeshColliderComponent, convex), IF_F(JceMeshColliderComponent, is_trigger),
    IF_F(JceMeshColliderComponent, friction), IF_F(JceMeshColliderComponent, restitution),
};
static const InspField kF_compound[] = {
    IF_F(JceCompoundColliderComponent, model_path), IF_F(JceCompoundColliderComponent, mode), IF_F(JceCompoundColliderComponent, split),
    IF_F(JceCompoundColliderComponent, is_static), IF_F(JceCompoundColliderComponent, detect_naming), IF_F(JceCompoundColliderComponent, is_trigger),
    IF_F(JceCompoundColliderComponent, friction), IF_F(JceCompoundColliderComponent, restitution),
    IF_F(JceCompoundColliderComponent, vhacd_resolution), IF_F(JceCompoundColliderComponent, vhacd_max_hulls), IF_F(JceCompoundColliderComponent, vhacd_max_verts_per_hull),
    IF_F(JceCompoundColliderComponent, physmat_path),
};
static const InspField kF_audio[] = {
    IF_F(JceAudioSourceComponent, clip_path), IF_F(JceAudioSourceComponent, volume), IF_F(JceAudioSourceComponent, pitch),
    IF_F(JceAudioSourceComponent, spatial_blend), IF_F(JceAudioSourceComponent, loop), IF_F(JceAudioSourceComponent, play_on_awake),
};
#undef IF_F
#undef IF_AX
#undef IF_STR

/* Map a dense comp_id to its field table (NULL ⇒ use the 4-byte fallback
 * for the whole changed region).  Rows are keyed by canonical engine name
 * and resolve their comp_id lazily once the engine registry exists. */
struct InspBcastRow {
    const char      *engine_name;
    const InspField *fields;
    int              count;
    int              comp_id; /* lazily resolved; JCE_COMP_ID_INVALID until */
};

#define IF_ROW(NAME, TBL) \
    { NAME, TBL, (int)(sizeof(TBL) / sizeof((TBL)[0])), JCE_COMP_ID_INVALID }
static InspBcastRow kBcastRows[] = {
    IF_ROW("Transform",        kF_transform),
    IF_ROW("MeshRenderer",     kF_mesh_renderer),
    IF_ROW("Camera",           kF_camera),
    IF_ROW("SpriteRenderer",   kF_sprite_renderer),
    IF_ROW("SkeletalAnimator", kF_skeletal),
    IF_ROW("Constraint",       kF_constraint),
    IF_ROW("Rigidbody",        kF_rigidbody),
    IF_ROW("BoxCollider",      kF_box),
    IF_ROW("SphereCollider",   kF_sphere),
    IF_ROW("CapsuleCollider",  kF_capsule),
    IF_ROW("MeshCollider",     kF_mesh_collider),
    IF_ROW("CompoundCollider", kF_compound),
    IF_ROW("AudioSource",      kF_audio),
};
#undef IF_ROW

static const InspField *inspbcast_fields(int comp_id, int *count)
{
    if (comp_id != JCE_COMP_ID_INVALID) {
        for (InspBcastRow &r : kBcastRows) {
            if (r.comp_id == JCE_COMP_ID_INVALID)
                r.comp_id = jce_component_find(r.engine_name);
            if (r.comp_id == comp_id) {
                *count = r.count;
                return r.fields;
            }
        }
    }
    *count = 0;
    return nullptr;
}
} /* namespace */

/* Wrap a single-component draw with a before/after byte diff and broadcast the
 * EDITED FIELD(S) to every other selected entity that holds the same
 * component (comp_id).
 *
 * The drawers mutate the focused entity's component in place, touching only the
 * fields the user actually edited this frame.  We snapshot the component bytes
 * before the draw and, afterwards, map each differing byte to the whole struct
 * field that contains it (see the field tables above) and copy those fields —
 * intact — onto the peers, leaving each peer's untouched fields (which may
 * legitimately differ per entity) alone.  Copying whole fields (not raw byte
 * runs) is what makes the broadcast value-correct for floats and strings. */
static void draw_section_with_multi_broadcast(uint32_t focused,
                                              EditorEntitySidecar &sidecar,
                                              JceScene *scene,
                                              JceEntity ecs_e,
                                              int entry)
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

    const uint8_t *after = (const uint8_t *)focused_ptr;
    const uint8_t *prev  = before.data();
    if (memcmp(after, prev, comp_size) == 0) return;

    /* Resolve the changed bytes to whole struct fields.  For each listed field
     * that has ANY differing byte, queue the WHOLE field as a copy span; then,
     * for any differing byte not covered by the table, fall back to its
     * 4-byte-aligned word (safe for plain scalars).  Copying whole fields — not
     * raw byte runs — is what keeps peer values correct. */
    struct Span { size_t off, len; };
    std::vector<Span> spans;
    std::vector<uint8_t> covered(comp_size, 0);

    int fcount = 0;
    const InspField *fields = inspbcast_fields(entry, &fcount);
    for (int fi = 0; fi < fcount; ++fi) {
        size_t off = fields[fi].off;
        size_t end = off + fields[fi].size;
        if (off >= comp_size) continue;
        if (end > comp_size) end = comp_size;
        bool changed = false;
        for (size_t b = off; b < end; ++b) {
            if (after[b] != prev[b]) changed = true;
            covered[b] = 1;
        }
        if (changed) spans.push_back({ off, end - off });
    }
    /* Fallback: any differing byte the table did not cover. */
    for (size_t b = 0; b < comp_size; ++b) {
        if (after[b] == prev[b] || covered[b]) continue;
        size_t ws = (b / 4) * 4;
        size_t we = ws + 4;
        if (we > comp_size) we = comp_size;
        spans.push_back({ ws, we - ws });
        for (size_t k = ws; k < we; ++k) covered[k] = 1;
    }
    if (spans.empty()) return;

    /* Broadcast each edited field to every other selected peer of the same
     * type, leaving the peers' untouched fields as-is. */
    for (int k = 0; k < sel_count; ++k) {
        uint32_t other = sel[k];
        if (other == focused) continue;
        JceEntity oe = jce_state_to_ecs_entity(other);
        if (!oe) continue;
        if (!jce_scene_has_comp(scene, oe, entry))
            continue;
        size_t osize = 0;
        void *optr = multi_get_comp_ptr(scene, oe, entry, &osize);
        if (!optr || osize != comp_size) continue;
        uint8_t *odst = (uint8_t *)optr;
        for (const Span &sp : spans)
            memcpy(odst + sp.off, after + sp.off, sp.len);
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
            s_drag.src_comp != s_drag.hover_comp &&
            s_drag.hover_comp != JCE_COMP_ID_INVALID) {
            auto &v = sidecar.component_order;
            auto it_src = std::find(v.begin(), v.end(), s_drag.src_comp);
            auto it_dst = std::find(v.begin(), v.end(), s_drag.hover_comp);
            if (it_src != v.end() && it_dst != v.end()) {
                jce_state_begin_batch_edit();
                int c = *it_src;
                size_t dst_idx = (size_t)(it_dst - v.begin());
                v.erase(it_src);
                if (dst_idx > (size_t)(it_src - v.begin())) dst_idx--;
                v.insert(v.begin() + dst_idx, c);
                jce_state_end_batch_edit();
            }
        }
        s_drag = { 0, JCE_COMP_ID_INVALID, JCE_COMP_ID_INVALID, false };
    }

    if (!s_pending_move.pending) return;
    if (s_pending_move.entity_id != focused_entity) {
        s_pending_move.pending = false;
        return;
    }

    auto &v = sidecar.component_order;
    auto it = std::find(v.begin(), v.end(), s_pending_move.src_comp);
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
    s_pending_move = { 0, JCE_COMP_ID_INVALID, 0, false };
}

/* == Per-row draw adapters ==============================================
 * One thin adapter per component, registered into the editor registry
 * rows at first inspector draw (the core registry TU cannot reference
 * the panel drawers).  draw_one_component_section dispatches through
 * the row -- this replaces the historic 50-case JCE_DRAW switch AND
 * the synthetic-slot early-outs.  Adding a future component means one
 * INSP_DRAWFN line + one entry in insp_register_draw_fns(). */

/* Every drawer publishes whose component it is drawing, so insp_track_edit()
 * -- which runs deep inside them, without the entity as a parameter -- can
 * record ONE ENTITY instead of serialising the whole scene twice per edit.
 * Cleared on the way out, so a widget drawn outside a component drawer keeps
 * the unscoped behaviour. */
#define INSP_DRAWFN(NAME, EXPR)                                               \
    static void drawfn_##NAME(JceScene *scene, JceEntity e,              \
                              uint32_t entity_id)                      \
    {                                                                     \
        (void)scene; (void)e; (void)entity_id;                            \
        insp_set_edit_scope(entity_id);                   \
        EXPR;                                                             \
        insp_set_edit_scope(0u);                          \
    }

INSP_DRAWFN(transform, draw_comp_transform(entity_id, jce_scene_get_transform(scene, e)))
INSP_DRAWFN(pivot, draw_comp_pivot(scene, e, jce_scene_get_pivot(scene, e)))
INSP_DRAWFN(camera, draw_comp_camera(jce_scene_get_camera(scene, e)))
/* draw + invalidate this entity's material_gen (lever ③ draw-cmd cache): the
 * inspector mutates the MeshRenderer in place via get_mut with no set-call, so
 * unconditionally bumping the inspected entity's material each frame it is shown
 * robustly picks up ANY edit path (widgets, drag-drop, material-file load). Cost
 * = the one selected entity rebuilds its cached cmd while inspected; harmless. */
INSP_DRAWFN(mesh_renderer, (draw_comp_mesh_renderer(jce_scene_get_mesh_renderer(scene, e)),
                            jce_scene_invalidate_entity_material(scene, e)))
INSP_DRAWFN(sprite_renderer, draw_comp_sprite_renderer(jce_scene_get_sprite_renderer(scene, e)))
INSP_DRAWFN(animator, draw_comp_animator(jce_scene_get_animator(scene, e)))
INSP_DRAWFN(skeletal_animator,
            draw_comp_skeletal_animator(jce_scene_get_skeletal_animator(scene, e)))
INSP_DRAWFN(rigidbody, draw_comp_rigidbody(jce_scene_get_rigidbody(scene, e)))
INSP_DRAWFN(box_collider, draw_comp_box_collider(jce_scene_get_box_collider(scene, e)))
INSP_DRAWFN(sphere_collider, draw_comp_sphere_collider(jce_scene_get_sphere_collider(scene, e)))
INSP_DRAWFN(character_controller,
            draw_comp_character_controller(jce_scene_get_character_controller(scene, e)))
INSP_DRAWFN(audio_source, draw_comp_audio_source(jce_scene_get_audio_source(scene, e), e))
INSP_DRAWFN(music_track, draw_comp_music_track(jce_scene_get_music_track(scene, e)))
INSP_DRAWFN(script, draw_comp_script(jce_scene_get_script(scene, e)))
INSP_DRAWFN(skybox, draw_comp_skybox(jce_scene_get_skybox(scene, e)))
INSP_DRAWFN(sprite_animator, draw_comp_sprite_animator(jce_scene_get_sprite_animator(scene, e)))
INSP_DRAWFN(constraint, draw_comp_constraint(jce_scene_get_constraint(scene, e)))
INSP_DRAWFN(terrain, draw_comp_terrain(jce_scene_get_terrain(scene, e)))
INSP_DRAWFN(vegetation_scatter, draw_comp_vegetation_scatter(jce_scene_get_vegetation_scatter(scene, e), scene, e))
INSP_DRAWFN(grass_field, draw_comp_grass_field(jce_scene_get_grass_field(scene, e)))
INSP_DRAWFN(foliage_cluster, draw_comp_foliage_cluster(jce_scene_get_foliage_cluster(scene, e)))
INSP_DRAWFN(water, draw_comp_water(jce_scene_get_water(scene, e)))
INSP_DRAWFN(buoyancy, draw_comp_buoyancy(jce_scene_get_buoyancy(scene, e)))
INSP_DRAWFN(rigidbody2d, draw_comp_rigidbody2d(jce_scene_get_rigidbody2d(scene, e)))
INSP_DRAWFN(particle_emitter, draw_comp_particle_emitter(jce_scene_get_particle_emitter(scene, e)))
INSP_DRAWFN(behavior_tree, draw_comp_behavior_tree(jce_scene_get_behavior_tree(scene, e)))
INSP_DRAWFN(lod_group, draw_comp_lod_group(jce_scene_get_lod_group(scene, e), scene, e))
INSP_DRAWFN(virtual_camera, draw_comp_virtual_camera(jce_scene_get_virtual_camera(scene, e)))
INSP_DRAWFN(trigger_volume, draw_comp_trigger_volume(jce_scene_get_trigger_volume(scene, e)))
INSP_DRAWFN(capsule_collider, draw_comp_capsule_collider(jce_scene_get_capsule_collider(scene, e)))
INSP_DRAWFN(mesh_collider, draw_comp_mesh_collider(jce_scene_get_mesh_collider(scene, e)))
INSP_DRAWFN(compound_collider,
            draw_comp_compound_collider(jce_scene_get_compound_collider(scene, e)))
INSP_DRAWFN(collider2d, draw_comp_collider2d(jce_scene_get_collider2d(scene, e)))
INSP_DRAWFN(trail_renderer, draw_comp_trail_renderer(jce_scene_get_trail_renderer(scene, e)))
INSP_DRAWFN(line_renderer, draw_comp_line_renderer(jce_scene_get_line_renderer(scene, e)))
INSP_DRAWFN(reflection_probe, draw_comp_reflection_probe(scene, e, jce_scene_get_reflection_probe(scene, e)))
INSP_DRAWFN(decal, draw_comp_decal(jce_scene_get_decal(scene, e)))
INSP_DRAWFN(light_probe_group,
            draw_comp_light_probe_group(jce_scene_get_light_probe_group(scene, e)))
INSP_DRAWFN(audio_listener, draw_comp_audio_listener(jce_scene_get_audio_listener(scene, e)))
INSP_DRAWFN(audio_reverb_zone,
            draw_comp_audio_reverb_zone(jce_scene_get_audio_reverb_zone(scene, e)))
INSP_DRAWFN(audio_occlusion, draw_comp_audio_occlusion(jce_scene_get_audio_occlusion(scene, e)))
INSP_DRAWFN(spawn_manager, draw_comp_spawn_manager(jce_scene_get_spawn_manager(scene, e)))
INSP_DRAWFN(weapon, draw_comp_weapon(jce_scene_get_weapon(scene, e)))
INSP_DRAWFN(save_point, draw_comp_save_point(jce_scene_get_save_point(scene, e)))
INSP_DRAWFN(wheel_collider, draw_comp_wheel_collider(jce_scene_get_wheel_collider(scene, e)))
INSP_DRAWFN(vehicle, draw_comp_vehicle(jce_scene_get_vehicle(scene, e)))
INSP_DRAWFN(soft_body, draw_comp_soft_body(jce_scene_get_soft_body(scene, e)))
INSP_DRAWFN(constant_force, draw_comp_constant_force(jce_scene_get_constant_force(scene, e)))
INSP_DRAWFN(configurable_joint,
            draw_comp_configurable_joint(jce_scene_get_configurable_joint(scene, e)))
INSP_DRAWFN(joint2d, draw_comp_joint2d(jce_scene_get_joint2d(scene, e)))
INSP_DRAWFN(billboard_renderer,
            draw_comp_billboard_renderer(jce_scene_get_billboard_renderer(scene, e)))
INSP_DRAWFN(canvas, draw_comp_canvas(jce_scene_get_canvas(scene, e)))
INSP_DRAWFN(canvas_group, draw_comp_canvas_group(jce_scene_get_canvas_group(scene, e)))
INSP_DRAWFN(layout_group, draw_comp_layout_group(jce_scene_get_layout_group(scene, e)))
INSP_DRAWFN(layout_element, draw_comp_layout_element(jce_scene_get_layout_element(scene, e)))
INSP_DRAWFN(content_size_fitter, draw_comp_content_size_fitter(jce_scene_get_content_size_fitter(scene, e)))
INSP_DRAWFN(bone_attachment, draw_comp_bone_attachment(jce_scene_get_bone_attachment(scene, e)))
INSP_DRAWFN(ui_image, draw_comp_ui_image(jce_scene_get_ui_image(scene, e)))
INSP_DRAWFN(ui_text, draw_comp_ui_text(jce_scene_get_ui_text(scene, e)))
INSP_DRAWFN(ui_button, draw_comp_ui_button(jce_scene_get_ui_button(scene, e)))
INSP_DRAWFN(ui_slider, draw_comp_ui_slider(jce_scene_get_ui_slider(scene, e)))
INSP_DRAWFN(ui_toggle, draw_comp_ui_toggle(jce_scene_get_ui_toggle(scene, e)))
INSP_DRAWFN(ui_input_field, draw_comp_ui_input_field(jce_scene_get_ui_input_field(scene, e)))
INSP_DRAWFN(ui_scroll_view, draw_comp_ui_scroll_view(jce_scene_get_ui_scroll_view(scene, e)))
INSP_DRAWFN(ui_progress_bar, draw_comp_ui_progress_bar(jce_scene_get_ui_progress_bar(scene, e)))
INSP_DRAWFN(ui_dropdown, draw_comp_ui_dropdown(jce_scene_get_ui_dropdown(scene, e)))
INSP_DRAWFN(cloth, draw_comp_cloth(jce_scene_get_cloth(scene, e)))
INSP_DRAWFN(network_object, draw_comp_network_object(jce_scene_get_network_object(scene, e)))
INSP_DRAWFN(net_transform, draw_comp_net_transform(jce_scene_get_net_transform(scene, e)))
INSP_DRAWFN(net_animator, draw_comp_net_animator(jce_scene_get_net_animator(scene, e)))
INSP_DRAWFN(net_rigidbody, draw_comp_net_rigidbody(jce_scene_get_net_rigidbody(scene, e)))
INSP_DRAWFN(tilemap, draw_comp_tilemap(jce_scene_get_tilemap(scene, e)))
INSP_DRAWFN(tilemap_collider2d,
            draw_comp_tilemap_collider2d(jce_scene_get_tilemap_collider2d(scene, e)))
INSP_DRAWFN(avatar, draw_comp_avatar(jce_scene_get_avatar(scene, e)))
INSP_DRAWFN(volume, draw_comp_volume(jce_scene_get_volume(scene, e)))
INSP_DRAWFN(fullscreen_effect,
            draw_comp_fullscreen_effect(jce_scene_get_fullscreen_effect(scene, e)))
INSP_DRAWFN(occlusion_portal, draw_comp_occlusion_portal(jce_scene_get_occlusion_portal(scene, e)))
INSP_DRAWFN(video_player, draw_comp_video_player(jce_scene_get_video_player(scene, e)))
INSP_DRAWFN(nav_agent, draw_comp_nav_agent(jce_scene_get_nav_agent(scene, e)))
INSP_DRAWFN(sim_lod, draw_comp_sim_lod(jce_scene_get_sim_lod(scene, e)))
INSP_DRAWFN(ik_constraints, draw_comp_ik_constraints(jce_scene_get_ik_constraints(scene, e)))
INSP_DRAWFN(foot_ik, draw_comp_foot_ik(jce_scene_get_foot_ik(scene, e)))
INSP_DRAWFN(full_body_ik, draw_comp_full_body_ik(jce_scene_get_full_body_ik(scene, e)))
INSP_DRAWFN(sequence_player, draw_comp_sequence_player(jce_scene_get_sequence_player(scene, e)))
INSP_DRAWFN(morph_weights, draw_comp_morph_weights(scene, e, jce_scene_get_morph_weights(scene, e)))
INSP_DRAWFN(network_variable, draw_comp_network_variable(jce_scene_get_network_variable(scene, e)))
INSP_DRAWFN(gas, draw_comp_gas(jce_scene_get_gas(scene, e)))
INSP_DRAWFN(ragdoll, draw_comp_ragdoll(jce_scene_get_ragdoll(scene, e)))
INSP_DRAWFN(fracture, draw_comp_fracture(jce_scene_get_fracture(scene, e)))
#undef INSP_DRAWFN

static void insp_register_draw_fns(void)
{
    static bool done = false;
    if (done) return;
    done = true;
    struct DrawRow { const char *engine_name; JceEditorCompDrawFn fn; };
    static const DrawRow kRows[] = {
        { "Transform", drawfn_transform },
        { "Pivot", drawfn_pivot },
        { "Camera", drawfn_camera },
        { "MeshRenderer", drawfn_mesh_renderer },
        { "SpriteRenderer", drawfn_sprite_renderer },
        { "Animator", drawfn_animator },
        { "SkeletalAnimator", drawfn_skeletal_animator },
        { "Rigidbody", drawfn_rigidbody },
        { "BoxCollider", drawfn_box_collider },
        { "SphereCollider", drawfn_sphere_collider },
        { "CharacterController", drawfn_character_controller },
        { "AudioSource", drawfn_audio_source },
        { "MusicTrack", drawfn_music_track },
        { "Script", drawfn_script },
        { "Skybox", drawfn_skybox },
        { "SpriteAnimator", drawfn_sprite_animator },
        { "Constraint", drawfn_constraint },
        { "Terrain", drawfn_terrain },
        { "VegetationScatter", drawfn_vegetation_scatter },
        { "GrassField", drawfn_grass_field },
        { "FoliageCluster", drawfn_foliage_cluster },
        { "Water", drawfn_water },
        { "Buoyancy", drawfn_buoyancy },
        { "Rigidbody2D", drawfn_rigidbody2d },
        { "ParticleEmitter", drawfn_particle_emitter },
        { "BehaviorTree", drawfn_behavior_tree },
        { "LODGroup", drawfn_lod_group },
        { "VirtualCamera", drawfn_virtual_camera },
        { "TriggerVolume", drawfn_trigger_volume },
        { "CapsuleCollider", drawfn_capsule_collider },
        { "MeshCollider", drawfn_mesh_collider },
        { "CompoundCollider", drawfn_compound_collider },
        { "Collider2D", drawfn_collider2d },
        { "TrailRenderer", drawfn_trail_renderer },
        { "LineRenderer", drawfn_line_renderer },
        { "ReflectionProbe", drawfn_reflection_probe },
        { "Decal", drawfn_decal },
        { "LightProbeGroup", drawfn_light_probe_group },
        { "AudioListener", drawfn_audio_listener },
        { "AudioReverbZone", drawfn_audio_reverb_zone },
        { "AudioOcclusion", drawfn_audio_occlusion },
        { "SpawnManager", drawfn_spawn_manager },
        { "Weapon", drawfn_weapon },
        { "SavePoint", drawfn_save_point },
        { "WheelCollider", drawfn_wheel_collider },
        { "Vehicle", drawfn_vehicle },
        { "SoftBody", drawfn_soft_body },
        { "ConstantForce", drawfn_constant_force },
        { "ConfigurableJoint", drawfn_configurable_joint },
        { "Joint2D", drawfn_joint2d },
        { "BillboardRenderer", drawfn_billboard_renderer },
        { "Canvas", drawfn_canvas },
        { "CanvasGroup", drawfn_canvas_group },
        { "LayoutGroup", drawfn_layout_group },
        { "LayoutElement", drawfn_layout_element },
        { "ContentSizeFitter", drawfn_content_size_fitter },
        { "BoneAttachment", drawfn_bone_attachment },
        { "UIImage", drawfn_ui_image },
        { "UIText", drawfn_ui_text },
        { "UIButton", drawfn_ui_button },
        { "UISlider", drawfn_ui_slider },
        { "UIToggle", drawfn_ui_toggle },
        { "UIInputField", drawfn_ui_input_field },
        { "UIScrollView", drawfn_ui_scroll_view },
        { "UIProgressBar", drawfn_ui_progress_bar },
        { "UIDropdown", drawfn_ui_dropdown },
        { "Cloth", drawfn_cloth },
        { "NetworkObject", drawfn_network_object },
        { "NetworkTransform", drawfn_net_transform },
        { "NetworkAnimator", drawfn_net_animator },
        { "NetworkRigidbody", drawfn_net_rigidbody },
        { "Tilemap", drawfn_tilemap },
        { "TilemapCollider2D", drawfn_tilemap_collider2d },
        { "Avatar", drawfn_avatar },
        { "Volume", drawfn_volume },
        { "FullscreenEffect", drawfn_fullscreen_effect },
        { "OcclusionPortal", drawfn_occlusion_portal },
        { "VideoPlayer", drawfn_video_player },
        { "NavAgent", drawfn_nav_agent },
        { "SimLod", drawfn_sim_lod },
        { "IkConstraints", drawfn_ik_constraints },
        { "FootIk", drawfn_foot_ik },
        { "FullBodyIk", drawfn_full_body_ik },
        { "SequencePlayer", drawfn_sequence_player },
        { "MorphWeights", drawfn_morph_weights },
        { "NetworkVariable", drawfn_network_variable },
        { "GameplayAbilitySystem", drawfn_gas },
        { "Ragdoll", drawfn_ragdoll },
        { "Fracture", drawfn_fracture },
    };
    for (const DrawRow &r : kRows)
        jce_editor_component_set_draw_fn(r.engine_name, r.fn);
}

/* Single dispatch from a dense comp_id to its registry row draw fn.
 * Light is handled inline by the caller (light-group path); rows with
 * no draw fn (per-type lights, EditorMeta) draw nothing, exactly like
 * the old switch default case. */
static void draw_one_component_section(uint32_t focused,
                                       EditorEntitySidecar &sidecar,
                                       JceScene *scene,
                                       JceEntity ecs_e,
                                       int comp_id)
{
    insp_register_draw_fns();
    const JceEditorComponentDescriptor *desc =
        jce_editor_component_find_by_id(comp_id);
    if (!desc || !desc->draw)
        return;
    if (comp_section_begin(focused, sidecar, comp_id, desc->display_name,
                           desc->removable))
        desc->draw(scene, ecs_e, focused);
    comp_section_end();
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

/* ── Add Component options + popup live in jce_panel_inspector_add_component.cpp ── */

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

    /* ── Multi-entity selection: shared bulk controls ──────────────────
     * Render the bulk toggles / transform editor / selected-entity list,
     * then FALL THROUGH so the focused entity's per-component sections
     * still draw below.  Edits made there are broadcast per-field to the
     * other selected entities (draw_section_with_multi_broadcast). */
    insp_draw_multi_select_view(scene);

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
                /* One call: jce_state_set_entity_layer now writes the engine's
                 * JceLayerComponent too.  Calling both here was the ONLY site
                 * that remembered to, which is what made the two copies agree
                 * in the Inspector and nowhere else. */
                if (ImGui::Selectable(nm, i == cur_layer))
                    jce_state_set_entity_layer(focused, i);
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

    sync_component_order(sidecar, scene, ecs_e, flags);

    /* Refresh the prefab-override indicator set ONCE for this entity this
     * frame (one source .prefab.json load) before drawing component
     * headers, which query the cache.  Recompute when the focused entity
     * changed or the cache was invalidated by an Apply/Revert.  Non-
     * instances clear the set (no badges).  Best-effort visual only. */
    if (s_override_cache.entity_id != focused)
        s_override_cache.refresh(focused);

    /* Iterate components in user-defined display order (dense comp_ids).
     * The dispatcher delegates to the same comp_section_begin /
     * draw_comp_X / end sequence the previous code used per-flag. */
    bool light_drawn = false;
    for (int entry : sidecar.component_order) {
        if (jce_editor_component_id_is_light_group(entry)) {
            if (light_drawn) continue;
            if (!(flags & INSP_LIGHT_MASK)) continue;
            /* Section state (fold/remove/presets) keys on the PRESENT
             * light type's own comp_id, matching the old per-flag key. */
            uint64_t lf = (flags & JCE_COMP_FLAG_DIR_LIGHT)   ? JCE_COMP_FLAG_DIR_LIGHT
                       : (flags & JCE_COMP_FLAG_POINT_LIGHT) ? JCE_COMP_FLAG_POINT_LIGHT
                                                              : JCE_COMP_FLAG_SPOT_LIGHT;
            int lcid = jce_editor_component_comp_id(lf);
            if (comp_section_begin(focused, sidecar, lcid, "Light", true))
                draw_comp_light(scene, ecs_e, flags);
            comp_section_end();
            light_drawn = true;
            continue;
        }
        if (entry == JCE_COMP_ID_INVALID ||
            !jce_scene_has_comp(scene, ecs_e, entry))
            continue;
        draw_section_with_multi_broadcast(focused, sidecar, scene, ecs_e, entry);
    }

    apply_pending_reorder(focused, sidecar);

    insp_add_component_button_and_popup(focused, flags);

    ImGui::EndDisabled();

    /* Flush deferred component removal here, after all draw_comp_*
     * functions have returned (so no stale flecs pointer is in use).
     * jce_state_remove_component_id resolves the registry row from the
     * dense comp_id, so one call covers every row. */
    if (s_pending_remove.pending) {
        uint32_t eid = s_pending_remove.entity_id;
        int      cid = s_pending_remove.comp_id;
        s_pending_remove.pending   = false;
        s_pending_remove.entity_id = 0;
        s_pending_remove.comp_id   = JCE_COMP_ID_INVALID;
        jce_state_remove_component_id(eid, cid);
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

    bool keep_open = s_insp.delete_requested;
    jce_modal::Result r = jce_modal::confirm_delete(
        &keep_open,
        "###ConfirmDeleteEntityDlg",
        "inspector.yes",
        "inspector.no",
        330.0f,
        [&]() {
            if (s_insp.delete_entity_count == 1)
                ImGui::TextUnformatted(jce_editor_i18n("inspector.confirmDelete"));
            else
                ImGui::Text("%s: %d",
                            jce_editor_i18n("inspector.confirmDeleteMultiple"),
                            s_insp.delete_entity_count);
        });

    if (r == jce_modal::CONFIRM) {
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
    }

    s_insp.delete_requested = keep_open;
    if (!keep_open) s_insp.delete_entity_count = 0;
}
