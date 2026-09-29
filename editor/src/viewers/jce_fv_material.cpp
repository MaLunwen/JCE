/*
 * jce_fv_material.cpp  PBR Material (.mat.json) sub-viewer with editing.
 *
 * Displays material properties as editable ImGui widgets and texture map
 * thumbnails. Users can modify PBR values and save back to .mat.json.
 */

#include "jce_fv_common.h"
#include "ui/jce_editor_dnd.h"

#include <string>
#include <vector>

extern "C" {
#include <jce/renderer/jce_pbr_material.h>
#include <jce/resource/jce_image_decode.h>
}

#include "core/jce_assetdb.h"        /* JCE_ASSET_KIND_MATERIAL */
#include "dialogs/jce_path_input.h"  /* jce_draw_path_input_asset */
#include "io/jce_editor_file_util.h"
#include "ui/jce_editor_panels.h"

#define LOG_TAG "fv_material"

/* ══════════════════════════════════════════════════════════════════════
 *  PER-TAB MATERIAL STATE (keyed by path)
 * ══════════════════════════════════════════════════════════════════════ */

struct MatTexSlot {
    JceTexture handle;
    int        w, h;
    bool       tried;   /* already attempted to load */
};

struct MatViewState {
    bool           parsed;
    bool           load_ok;
    bool           modified;
    JcePbrMaterial mat;
    char           tex_paths[5][256];   /* albedo, mr, normal, ao, emissive */
    MatTexSlot     thumbs[5];
    /* The parent link, as the FILE states it (relative, unresolved).  Not in
     * JcePbrMaterial: that struct is the resolved render state and is copied
     * by value on every draw. */
    char           parent[256];
};

static std::vector<std::pair<std::string, MatViewState>> s_mat_states;

static MatViewState *fv_get_mat_state(FvTab *tab)
{
    for (auto &pr : s_mat_states)
        if (pr.first == tab->path) return &pr.second;
    s_mat_states.push_back(std::make_pair(std::string(tab->path), MatViewState()));
    return &s_mat_states.back().second;
}

/* ── Lazy-load a single texture thumbnail from disk ───────────── */

static void try_load_thumb(MatTexSlot *slot, const char *path)
{
    if (slot->tried) return;
    slot->tried = true;
    slot->handle.idx = UINT16_MAX;
    slot->w = slot->h = 0;

    if (!path || path[0] == '\0') return;

    size_t sz = 0;
    char *buf = (char *)ed_read_file(path, &sz);
    if (!buf) return;
    if (sz == 0 || sz > 64 * 1024 * 1024) { ED_FREE(buf); return; }

    JceImage img;
    if (jce_image_decode(buf, sz, &img)) {
        slot->w = (int)img.width;
        slot->h = (int)img.height;
        slot->handle = jce_texture_from_rgba(img.pixels, img.width, img.height);
        jce_image_free(&img);
    }
    ED_FREE(buf);
}

/* ══════════════════════════════════════════════════════════════════════
 *  RENDER
 * ══════════════════════════════════════════════════════════════════════ */

static const char *const s_tex_label_keys[5] = {
    "inspector.texture.albedo",
    "inspector.texture.metalRough",
    "inspector.texture.normal",
    "inspector.texture.ao",
    "inspector.texture.emissive"
};

/* Track edits: call after each ImGui edit widget. */
static void check_edit(MatViewState *ms)
{
    if (ImGui::IsItemDeactivatedAfterEdit())
        ms->modified = true;
}

void fv_render_material(FvTab *tab)
{
    MatViewState *ms = fv_get_mat_state(tab);

    /* ── Parse on first access ──────────────────────────────────── */
    if (!ms->parsed) {
        ms->parsed = true;
        ms->load_ok = false;
        ms->modified = false;
        tab->modified = false;
        memset(&ms->mat, 0, sizeof(ms->mat));
        memset(ms->tex_paths, 0, sizeof(ms->tex_paths));
        for (int i = 0; i < 5; i++) {
            ms->thumbs[i].handle.idx = UINT16_MAX;
            ms->thumbs[i].tried = false;
        }

        ms->parent[0] = '\0';
        if (tab->path[0] != '\0') {
            ms->load_ok = jce_pbr_material_load_json(tab->path,
                                                      &ms->mat,
                                                      ms->tex_paths);
            jce_pbr_material_get_parent(tab->path, ms->parent,
                                        sizeof ms->parent);
        }
    }

    /* ── Toolbar ────────────────────────────────────────────────── */
    ImGui::TextColored(JCE_COLOR_ACCENT, "%s",
        jce_editor_i18n("viewer.materialSettings"));
    ImGui::SameLine();
    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "  %s%s  |  %.1f KB",
        tab->display_name,
        ms->modified ? " *" : "",
        (double)tab->file_size / 1024.0);

    /* Save button */
    if (ms->modified && ms->load_ok) {
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n("dialog.save"))) {
            if (jce_pbr_material_save_json(tab->path, &ms->mat, ms->tex_paths)) {
                ms->modified = false;
                tab->modified = false;
                jce_editor_console_log("Saved material: %s", tab->path);
                /* Push changes to all entities referencing this material. */
                jce_editor_inspector_reload_material(tab->path);
                /* Refresh raw content for the Source section. */
                size_t got = 0, total = 0;
                char *buf = (char *)ed_read_file_capped(tab->path, FV_MAX_CONTENT,
                                                        &got, &total);
                if (buf) {
                    ED_FREE(tab->content);
                    tab->content = buf;
                    tab->content_len = (int)got;
                    tab->file_size = (long)total;
                }
            } else {
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "Failed to save material: %s", tab->path);
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n("dialog.cancel"))) {
            /* Reload from disk to discard changes. */
            ms->parsed = false;
            ms->modified = false;
            tab->modified = false;
        }
    }

    ImGui::Separator();

    if (!ms->load_ok) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
            "%s", jce_editor_i18n("viewer.materialParseFailed"));
        ImGui::Spacing();
        ImGui::Separator();
        fv_render_code(tab);
        return;
    }

    /* ── Parent (material variant) ──────────────────────────────── */
    {
        char before[256];
        snprintf(before, sizeof before, "%s", ms->parent);
        jce_draw_path_input_asset(
            jce_editor_i18n_id("viewer.material.parent", "Parent Material"),
            ms->parent, sizeof ms->parent, JCE_ASSET_KIND_MATERIAL);
        if (strcmp(before, ms->parent) != 0) {
            /* Written through the FILE and reloaded immediately: the values
             * shown below are the RESOLVED ones, and they do not exist until
             * the link is on disk.  Editing the path and seeing the old
             * numbers would be the worst of both. */
            if (jce_pbr_material_set_parent(tab->path,
                                            ms->parent[0] ? ms->parent : NULL)) {
                ms->parsed = false;      /* re-resolve on the next frame */
                ms->modified = false;
                tab->modified = false;
                jce_editor_inspector_reload_material(tab->path);
            } else {
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "Cannot set material parent: %s", tab->path);
                snprintf(ms->parent, sizeof ms->parent, "%s", before);
            }
        }
        if (ms->parent[0]) {
            ImGui::TextDisabled("%s", jce_editor_i18n_id(
                "viewer.material.parentHint",
                "values below are resolved; saving stores only what differs"));
        }
        ImGui::Separator();
    }

    /* ── PBR Properties (editable) ──────────────────────────────── */
    JcePbrMaterial &m = ms->mat;

    if (ImGui::CollapsingHeader(jce_editor_i18n("inspector.pbrMaterial"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Columns(2, "##matprops", false);
        ImGui::SetColumnWidth(0, 160);

        const char *alpha_items[] = {
            jce_editor_i18n("inspector.alphaMode.opaque"),
            jce_editor_i18n("inspector.alphaMode.mask"),
            jce_editor_i18n("inspector.alphaMode.blend")
        };

        /* Base Color */
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                           "%s", jce_editor_i18n("inspector.baseColor"));
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::ColorEdit4("##bc", m.base_color_factor,
            ImGuiColorEditFlags_AlphaPreview | ImGuiColorEditFlags_Float);
        check_edit(ms);
        ImGui::NextColumn();

        /* Metallic */
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                   "%s", jce_editor_i18n("viewer.metallic"));
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##metallic", &m.metallic_factor, 0.01f, 0.0f, 1.0f);
        check_edit(ms);
        ImGui::NextColumn();

        /* Roughness */
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                   "%s", jce_editor_i18n("viewer.roughness"));
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##roughness", &m.roughness_factor, 0.01f, 0.0f, 1.0f);
        check_edit(ms);
        ImGui::NextColumn();

        /* Normal Scale */
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                   "%s", jce_editor_i18n("inspector.normalScale"));
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##nscale", &m.normal_scale, 0.01f, 0.0f, 4.0f);
        check_edit(ms);
        ImGui::NextColumn();

        /* AO Strength */
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                   "%s", jce_editor_i18n("inspector.aoStrength"));
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##ao", &m.ao_strength, 0.01f, 0.0f, 2.0f);
        check_edit(ms);
        ImGui::NextColumn();

        /* Emissive */
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                   "%s", jce_editor_i18n("inspector.emissive"));
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::ColorEdit3("##em", m.emissive_factor, ImGuiColorEditFlags_Float);
        check_edit(ms);
        ImGui::NextColumn();

        /* Alpha Mode */
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                           "%s", jce_editor_i18n("inspector.alphaMode"));
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        int alpha_idx = (int)m.alpha_mode;
        if (alpha_idx < 0 || alpha_idx > 2) alpha_idx = 0;
        if (ImGui::Combo("##alpha", &alpha_idx, alpha_items, 3)) {
            m.alpha_mode = (JceAlphaMode)alpha_idx;
            ms->modified = true;
        }
        if (m.alpha_mode == JCE_ALPHA_MASK) {
            ImGui::NextColumn();
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                               "%s", jce_editor_i18n("inspector.alphaCutoff"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##cutoff", &m.alpha_cutoff, 0.01f, 0.0f, 1.0f);
            check_edit(ms);
        }
        ImGui::NextColumn();

        /* Double Sided */
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                           "%s", jce_editor_i18n("inspector.doubleSided"));
        ImGui::NextColumn();
        if (ImGui::Checkbox("##dbl", &m.double_sided))
            ms->modified = true;
        ImGui::NextColumn();

        ImGui::Columns(1);
    }

    ImGui::Spacing();

    /* ── Texture Maps (editable paths + thumbnails) ─────────────── */
    if (ImGui::CollapsingHeader(jce_editor_i18n("inspector.textures"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        const float thumb_sz = 64.0f;

        for (int ti = 0; ti < 5; ti++) {
            char *path = ms->tex_paths[ti];
            bool has_path = (path[0] != '\0');

            ImGui::PushID(ti);

            /* Lazy-load thumbnail. */
            if (has_path)
                try_load_thumb(&ms->thumbs[ti], path);

            /* Thumbnail + label */
            if (has_path && jce_texture_valid(ms->thumbs[ti].handle)) {
                ImGui::Image(
                    (ImTextureID)(uintptr_t)((uint32_t)ms->thumbs[ti].handle.idx + 1u),
                    ImVec2(thumb_sz, thumb_sz));
                ImGui::SameLine();
            }

            ImGui::BeginGroup();
            ImGui::TextColored(JCE_COLOR_ACCENT, "%s",
                               jce_editor_i18n(s_tex_label_keys[ti]));
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            char input_id[32];
            snprintf(input_id, sizeof(input_id), "##tp%d", ti);
            ImGui::InputText(input_id, path, 256);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                ms->modified = true;
                /* Reset thumbnail so it reloads from new path. */
                ms->thumbs[ti].tried = false;
                if (jce_texture_valid(ms->thumbs[ti].handle))
                    ms->thumbs[ti].handle.idx = UINT16_MAX;
            }
            /* Accept texture drag-drop from asset browser. */
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload *payload =
                        ImGui::AcceptDragDropPayload(JCE_DND_ASSET_PATH)) {
                    const char *src = (const char *)payload->Data;
                    char rel[1024];
                    const char *store_path = jce_editor_path_relative_or(rel, sizeof(rel), src);
                    snprintf(path, 256, "%s", store_path);
                    ms->modified = true;
                    ms->thumbs[ti].tried = false;
                    ms->thumbs[ti].handle.idx = UINT16_MAX;
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::EndGroup();

            if (ImGui::IsItemHovered() && has_path)
                ImGui::SetTooltip("%s", path);

            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    tab->modified = ms->modified;

    /* ── Raw JSON source ────────────────────────────────────────── */
    if (ImGui::CollapsingHeader(jce_editor_i18n("viewer.source"))) {
        fv_render_code(tab);
    }
}
