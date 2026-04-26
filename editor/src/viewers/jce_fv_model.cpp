/*
 * jce_fv_model.cpp  3D Model sub-viewer with wireframe, grid, rotation, info.
 *
 * Reference: Java ModelViewerWindow.
 *
 * Controls (Windows 3D Viewer style):
 *   - Left-drag: rotate model (clockwise)
 *   - Right-drag: pan view
 *   - Scroll: zoom in/out
 *   - Toolbar: Wireframe, Grid, Auto-rotate, Reset View
 */

#include "jce_fv_common.h"

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <string>
#include <vector>

#define LOG_TAG "fv_model"

/* Complexity thresholds for graceful degradation.
 * Files above FV_MODEL_SKIP_PARSE_BYTES skip assimp entirely.
 * Models above FV_MODEL_MAX_WIREFRAME_VERTS show info-only. */
#define FV_MODEL_SKIP_PARSE_BYTES    (10 * 1024 * 1024)  /* 10 MB */
#define FV_MODEL_MAX_WIREFRAME_VERTS 100000               /* 100 K */

/* ══════════════════════════════════════════════════════════════════════
 *  MODEL VIEWER STATE (per tab, keyed by path)
 * ══════════════════════════════════════════════════════════════════════ */

struct ModelViewState {
    std::vector<float> vx, vy, vz;
    std::vector<int>   face_idx;
    std::vector<int>   face_sizes;
    float rotX, rotY;
    float zoom;
    float panX, panY;       /* right-drag panning */
    bool  auto_rotate;
    bool  show_wireframe;
    bool  show_grid;
    bool  parsed;
    bool  too_complex;       /* model too large for wireframe preview */
    bool  skipped_parse;     /* file too large, assimp skipped entirely */
    float minX, minY, minZ, maxX, maxY, maxZ;
    float cx, cy, cz, scale;
    int   mesh_count;
    int   vert_count, face_count, mat_count;
    std::vector<std::string> materials;
    bool  load_ok;
    char  load_error[256];
};

static std::vector<std::pair<std::string, ModelViewState>> s_model_states;

/* ══════════════════════════════════════════════════════════════════════
 *  CLEANUP
 * ══════════════════════════════════════════════════════════════════════ */

void fv_model_close_tab(const char *path)
{
    for (size_t i = 0; i < s_model_states.size(); i++) {
        if (s_model_states[i].first == path) {
            s_model_states.erase(s_model_states.begin() + (ptrdiff_t)i);
            return;
        }
    }
}

void fv_model_shutdown(void)
{
    s_model_states.clear();
}

/* ══════════════════════════════════════════════════════════════════════
 *  ASSIMP MODEL PARSING
 * ══════════════════════════════════════════════════════════════════════ */

static ModelViewState *fv_get_model_state(FvTab *tab)
{
    for (auto &pr : s_model_states)
        if (pr.first == tab->path) return &pr.second;
    s_model_states.push_back(std::make_pair(std::string(tab->path), ModelViewState()));
    return &s_model_states.back().second;
}

static void fv_parse_model_assimp(FvTab *tab, ModelViewState *ms)
{
    if (ms->parsed) return;
    ms->parsed = true;

    ms->rotX = 30.0f; ms->rotY = -45.0f;
    ms->zoom = 1.0f;
    ms->panX = 0.0f; ms->panY = 0.0f;
    ms->auto_rotate = false;
    ms->show_wireframe = true;
    ms->show_grid = true;
    ms->mesh_count = 0;
    ms->vert_count = 0; ms->face_count = 0; ms->mat_count = 0;
    ms->load_ok = false;
    ms->too_complex = false;
    ms->skipped_parse = false;
    ms->load_error[0] = '\0';
    ms->minX = ms->minY = ms->minZ =  1e30f;
    ms->maxX = ms->maxY = ms->maxZ = -1e30f;

    /* Skip assimp entirely for very large files to avoid main-thread stall. */
    if (tab->file_size > FV_MODEL_SKIP_PARSE_BYTES) {
        ms->skipped_parse = true;
        ms->too_complex   = true;
        ms->load_ok       = true;  /* not an error, just degraded */
        return;
    }

    if (!tab->content || tab->content_len <= 0) {
        snprintf(ms->load_error, sizeof(ms->load_error),
                 "empty model content");
        return;
    }

    const unsigned assimp_flags =
        aiProcess_Triangulate
        | aiProcess_JoinIdenticalVertices
        | aiProcess_GenSmoothNormals
        | aiProcess_ImproveCacheLocality;

    /* Prefer file-based loading so assimp can resolve external references
     * (e.g. .bin files referenced by .gltf).  Fall back to memory-based
     * loading when the file path doesn't work (embedded buffers, etc.).
     * Both importers live at the same scope so the scene pointer stays
     * valid throughout parsing. */
    Assimp::Importer file_importer;
    Assimp::Importer mem_importer;
    const aiScene *scene = nullptr;

    if (tab->path[0] != '\0')
        scene = file_importer.ReadFile(tab->path, assimp_flags);

    if (!scene || scene->mNumMeshes == 0) {
        const char *hint = tab->ext;
        if (hint && hint[0] == '.') hint++;
        scene = mem_importer.ReadFileFromMemory(
            tab->content, (size_t)tab->content_len, assimp_flags, hint);
    }

    if (!scene || scene->mNumMeshes == 0) {
        const char *err = file_importer.GetErrorString();
        if (!err || err[0] == '\0')
            err = mem_importer.GetErrorString();
        snprintf(ms->load_error, sizeof(ms->load_error),
                 "assimp: %s", err ? err : "unknown error");
        return;
    }

    ms->mesh_count = (int)scene->mNumMeshes;

    for (unsigned i = 0; i < scene->mNumMaterials; i++) {
        const aiMaterial *mat = scene->mMaterials[i];
        aiString name;
        if (mat && mat->Get(AI_MATKEY_NAME, name) == AI_SUCCESS && name.length > 0) {
            ms->materials.push_back(std::string(name.C_Str()));
        } else {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "Material %u", i);
            ms->materials.push_back(std::string(tmp));
        }
    }
    ms->mat_count = (int)ms->materials.size();

    /* Count total vertices/faces without building arrays first. */
    {
        int total_verts = 0, total_faces = 0;
        for (unsigned m = 0; m < scene->mNumMeshes; m++) {
            const aiMesh *mesh = scene->mMeshes[m];
            if (!mesh) continue;
            total_verts += (int)mesh->mNumVertices;
            total_faces += (int)mesh->mNumFaces;
        }
        ms->vert_count = total_verts;
        ms->face_count = total_faces;

        if (total_verts > FV_MODEL_MAX_WIREFRAME_VERTS) {
            ms->too_complex = true;
            ms->load_ok = true;
            return;
        }
    }

    /* Collect vertex/face data for wireframe rendering (small models). */
    ms->vert_count = 0;
    ms->face_count = 0;
    int vert_base = 0;
    for (unsigned m = 0; m < scene->mNumMeshes; m++) {
        const aiMesh *mesh = scene->mMeshes[m];
        if (!mesh) continue;

        for (unsigned v = 0; v < mesh->mNumVertices; v++) {
            float x = mesh->mVertices[v].x;
            float y = mesh->mVertices[v].y;
            float z = mesh->mVertices[v].z;

            ms->vx.push_back(x);
            ms->vy.push_back(y);
            ms->vz.push_back(z);

            if (x < ms->minX) ms->minX = x; if (x > ms->maxX) ms->maxX = x;
            if (y < ms->minY) ms->minY = y; if (y > ms->maxY) ms->maxY = y;
            if (z < ms->minZ) ms->minZ = z; if (z > ms->maxZ) ms->maxZ = z;
            ms->vert_count++;
        }

        for (unsigned f = 0; f < mesh->mNumFaces; f++) {
            const aiFace &face = mesh->mFaces[f];
            if (face.mNumIndices < 2) continue;

            ms->face_sizes.push_back((int)face.mNumIndices);
            for (unsigned fi = 0; fi < face.mNumIndices; fi++)
                ms->face_idx.push_back((int)face.mIndices[fi] + vert_base);
            ms->face_count++;
        }

        vert_base += (int)mesh->mNumVertices;
    }

    if (ms->vert_count <= 0) {
        snprintf(ms->load_error, sizeof(ms->load_error),
                 "assimp loaded scene but found no vertices");
        return;
    }

    if (ms->vert_count > 0) {
        ms->cx = (ms->minX + ms->maxX) * 0.5f;
        ms->cy = (ms->minY + ms->maxY) * 0.5f;
        ms->cz = (ms->minZ + ms->maxZ) * 0.5f;
        float dx = ms->maxX - ms->minX;
        float dy = ms->maxY - ms->minY;
        float dz = ms->maxZ - ms->minZ;
        float max_extent = dx;
        if (dy > max_extent) max_extent = dy;
        if (dz > max_extent) max_extent = dz;
        ms->scale = (max_extent > 0.0001f) ? (1.0f / max_extent) : 1.0f;
    }

    ms->load_ok = true;
}

/* ══════════════════════════════════════════════════════════════════════
 *  3D PROJECTION
 * ══════════════════════════════════════════════════════════════════════ */

static void fv_project(float px, float py, float pz,
                       float cosX, float sinX, float cosY, float sinY,
                       float *ox, float *oy)
{
    float x1 =  px * cosY + pz * sinY;
    float z1 = -px * sinY + pz * cosY;
    float y1 = py * cosX - z1 * sinX;
    *ox = x1;
    *oy = -y1;
}

/* ══════════════════════════════════════════════════════════════════════
 *  RENDER
 * ══════════════════════════════════════════════════════════════════════ */

void fv_render_model(FvTab *tab)
{
    const char *fmt_name = "Unknown";
    if (strcmp(tab->ext, ".gltf") == 0) fmt_name = "glTF 2.0 (JSON)";
    else if (strcmp(tab->ext, ".glb") == 0) fmt_name = "glTF 2.0 (Binary)";
    else if (strcmp(tab->ext, ".obj") == 0) fmt_name = "Wavefront OBJ";
    else if (strcmp(tab->ext, ".fbx") == 0) fmt_name = "Autodesk FBX";
    else if (strcmp(tab->ext, ".dae") == 0) fmt_name = "Collada DAE";
    else if (strcmp(tab->ext, ".3ds") == 0) fmt_name = "3D Studio";

    /* ── Toolbar ─────────────────────────────────────────────────── */
    {
        if (ImGui::Button(jce_editor_i18n("viewer.openExternal"))) {
            jce_host_reveal_path(tab->path);
        }
        ImGui::SameLine();
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
            "%s  |  %s  |  %.1f KB",
            tab->display_name, fmt_name, (double)tab->file_size / 1024.0);
    }
    ImGui::Separator();

    ModelViewState *ms = fv_get_model_state(tab);
    fv_parse_model_assimp(tab, ms);

    /* ── Too-complex / skipped: info-only panel (no wireframe) ───── */
    if (ms->too_complex) {
        ImGui::Spacing();
        if (ms->skipped_parse)
            ImGui::TextColored(JCE_COLOR_TEXT_ERROR, "%s",
                               jce_editor_i18n("viewer.modelTooLarge"));
        else
            ImGui::TextColored(JCE_COLOR_TEXT_ERROR, "%s",
                               jce_editor_i18n("viewer.modelTooComplex"));

        ImGui::TextWrapped("%s", jce_editor_i18n("viewer.useExternalViewer"));
        ImGui::Spacing();

        ImGui::TextColored(JCE_COLOR_ACCENT, "%s", jce_editor_i18n("viewer.modelInfo"));
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Columns(2, "##mdlinfo_complex", false);
        ImGui::SetColumnWidth(0, 120);

        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.format"));
        ImGui::NextColumn(); ImGui::Text("%s", fmt_name); ImGui::NextColumn();

        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.fileSize"));
        ImGui::NextColumn();
        if (tab->file_size >= 1024 * 1024)
            ImGui::Text("%.2f MB", (double)tab->file_size / (1024.0 * 1024.0));
        else
            ImGui::Text("%.1f KB", (double)tab->file_size / 1024.0);
        ImGui::NextColumn();

        if (!ms->skipped_parse) {
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.meshes"));
            ImGui::NextColumn(); ImGui::Text("%d", ms->mesh_count); ImGui::NextColumn();

            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.vertices"));
            ImGui::NextColumn(); ImGui::Text("%d", ms->vert_count); ImGui::NextColumn();

            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.faces"));
            ImGui::NextColumn(); ImGui::Text("%d", ms->face_count); ImGui::NextColumn();
        }

        if (strcmp(tab->ext, ".glb") == 0 && tab->content && tab->content_len >= 12) {
            const unsigned char *d = (const unsigned char *)tab->content;
            uint32_t version = d[4] | (d[5] << 8) | (d[6] << 16) | (d[7] << 24);
            uint32_t length  = d[8] | (d[9] << 8) | (d[10] << 16) | (d[11] << 24);
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.glbVersion"));
            ImGui::NextColumn(); ImGui::Text("%u", version); ImGui::NextColumn();
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.totalSize"));
            ImGui::NextColumn(); ImGui::Text("%u bytes", length); ImGui::NextColumn();
        }

        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.path"));
        ImGui::NextColumn(); ImGui::TextWrapped("%s", tab->path); ImGui::NextColumn();
        ImGui::Columns(1);

        if (!ms->skipped_parse && !ms->materials.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(JCE_COLOR_ACCENT, "%s (%d)",
                jce_editor_i18n("viewer.materials"),
                (int)ms->materials.size());
            ImGui::Separator();
            for (auto &m : ms->materials)
                ImGui::BulletText("%s", m.c_str());
        }

        return;
    }

    /* ── Assimp: unified 3D wireframe view for all model formats ─── */
    if (ms->load_ok) {

        ImVec2 avail = ImGui::GetContentRegionAvail();
        float preview_w = avail.x * 0.65f;

        /* Preview area */
        ImGui::BeginChild("##mdlpreview", ImVec2(preview_w, avail.y),
                          ImGuiChildFlags_Borders);
        {
            /* Controls bar */
            ImGui::Checkbox(jce_editor_i18n("viewer.wireframe"), &ms->show_wireframe);
            ImGui::SameLine();
            ImGui::Checkbox(jce_editor_i18n("viewer.showGrid"), &ms->show_grid);
            ImGui::SameLine();
            ImGui::Checkbox(jce_editor_i18n("viewer.autoRotate"), &ms->auto_rotate);
            ImGui::SameLine();
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s: %.1fx", jce_editor_i18n("viewer.zoom"), ms->zoom);
            ImGui::SameLine();
            if (ImGui::SmallButton(jce_editor_i18n("viewer.resetView"))) {
                ms->rotX = 30.0f; ms->rotY = -45.0f; ms->zoom = 1.0f;
                ms->panX = 0.0f; ms->panY = 0.0f;
            }

            /* Auto-rotate */
            if (ms->auto_rotate) {
                ms->rotY += ImGui::GetIO().DeltaTime * 30.0f;
                if (ms->rotY > 360.0f) ms->rotY -= 360.0f;
            }

            ImVec2 view_avail = ImGui::GetContentRegionAvail();
            ImVec2 view_pos = ImGui::GetCursorScreenPos();
            float vw = view_avail.x;
            float vh = view_avail.y;
            float view_size = (vw < vh) ? vw : vh;
            float half = view_size * 0.45f * ms->zoom;
            float center_x = view_pos.x + vw * 0.5f + ms->panX;
            float center_y = view_pos.y + vh * 0.5f + ms->panY;

            ImGui::InvisibleButton("##mdlview", view_avail);
            bool hovered = ImGui::IsItemHovered();
            bool active  = ImGui::IsItemActive();

            /* Left/middle-drag rotation */
            if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Left)
                        || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
                ImVec2 delta = ImGui::GetIO().MouseDelta;
                ms->rotY += delta.x * 0.5f;
                ms->rotX += delta.y * 0.5f;
                if (ms->rotX > 89.0f) ms->rotX = 89.0f;
                if (ms->rotX < -89.0f) ms->rotX = -89.0f;
            }

            /* Right-drag panning (Windows 3D Viewer style) */
            if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
                ImVec2 delta = ImGui::GetIO().MouseDelta;
                ms->panX += delta.x;
                ms->panY += delta.y;
            }

            /* Scroll zoom */
            if (hovered) {
                float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f) {
                    ms->zoom *= (wheel > 0) ? 1.15f : (1.0f / 1.15f);
                    if (ms->zoom < 0.1f) ms->zoom = 0.1f;
                    if (ms->zoom > 5.0f) ms->zoom = 5.0f;
                }
            }

            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(view_pos,
                ImVec2(view_pos.x + vw, view_pos.y + vh), true);

            /* Dark background */
            dl->AddRectFilled(view_pos,
                ImVec2(view_pos.x + vw, view_pos.y + vh),
                IM_COL32(30, 30, 35, 255));

            float radX = ms->rotX * JCE_DEG2RAD;
            float radY = ms->rotY * JCE_DEG2RAD;
            float cosX = cosf(radX), sinX = sinf(radX);
            float cosY = cosf(radY), sinY = sinf(radY);

            /* Grid */
            if (ms->show_grid) {
                int grid_n = 10;
                float gs = 0.5f;
                ImU32 grid_col = IM_COL32(80, 80, 90, 100);
                ImU32 red_col  = IM_COL32(200, 50, 50, 150);
                ImU32 blue_col = IM_COL32(50, 50, 200, 150);

                for (int g = -grid_n; g <= grid_n; g++) {
                    float t = g * gs / (float)grid_n;
                    ImU32 c = (g == 0) ? red_col : grid_col;

                    float ax, ay, bx, by;
                    fv_project(t, 0, -gs, cosX, sinX, cosY, sinY, &ax, &ay);
                    fv_project(t, 0,  gs, cosX, sinX, cosY, sinY, &bx, &by);
                    dl->AddLine(
                        ImVec2(center_x + ax * half, center_y + ay * half),
                        ImVec2(center_x + bx * half, center_y + by * half),
                        (g == 0) ? blue_col : c, 1.0f);

                    fv_project(-gs, 0, t, cosX, sinX, cosY, sinY, &ax, &ay);
                    fv_project( gs, 0, t, cosX, sinX, cosY, sinY, &bx, &by);
                    dl->AddLine(
                        ImVec2(center_x + ax * half, center_y + ay * half),
                        ImVec2(center_x + bx * half, center_y + by * half),
                        (g == 0) ? red_col : c, 1.0f);
                }
            }

            /* Model rendering */
            if (ms->vert_count > 0) {
                if (ms->show_wireframe) {
                    /* Wireframe faces */
                    ImU32 wire_col = IM_COL32(180, 180, 200, 200);
                    int fi = 0;
                    for (int f = 0; f < (int)ms->face_sizes.size(); f++) {
                        int nv = ms->face_sizes[f];
                        for (int e = 0; e < nv; e++) {
                            int i0 = ms->face_idx[fi + e];
                            int i1 = ms->face_idx[fi + ((e + 1) % nv)];
                            if (i0 < 0 || i0 >= ms->vert_count) continue;
                            if (i1 < 0 || i1 >= ms->vert_count) continue;

                            float px0 = (ms->vx[i0] - ms->cx) * ms->scale;
                            float py0 = (ms->vy[i0] - ms->cy) * ms->scale;
                            float pz0 = (ms->vz[i0] - ms->cz) * ms->scale;
                            float px1 = (ms->vx[i1] - ms->cx) * ms->scale;
                            float py1 = (ms->vy[i1] - ms->cy) * ms->scale;
                            float pz1 = (ms->vz[i1] - ms->cz) * ms->scale;

                            float sx0, sy0, sx1, sy1;
                            fv_project(px0, py0, pz0, cosX, sinX, cosY, sinY, &sx0, &sy0);
                            fv_project(px1, py1, pz1, cosX, sinX, cosY, sinY, &sx1, &sy1);

                            dl->AddLine(
                                ImVec2(center_x + sx0 * half, center_y + sy0 * half),
                                ImVec2(center_x + sx1 * half, center_y + sy1 * half),
                                wire_col, 1.0f);
                        }
                        fi += nv;
                    }
                } else {
                    /* Point cloud mode: draw each vertex as a small filled circle */
                    ImU32 point_col = IM_COL32(180, 200, 220, 220);
                    for (int v = 0; v < ms->vert_count; v++) {
                        float px0 = (ms->vx[v] - ms->cx) * ms->scale;
                        float py0 = (ms->vy[v] - ms->cy) * ms->scale;
                        float pz0 = (ms->vz[v] - ms->cz) * ms->scale;
                        float sx, sy;
                        fv_project(px0, py0, pz0, cosX, sinX, cosY, sinY, &sx, &sy);
                        dl->AddCircleFilled(
                            ImVec2(center_x + sx * half, center_y + sy * half),
                            1.5f, point_col);
                    }
                }
            }

            dl->PopClipRect();
        }
        ImGui::EndChild();

        ImGui::SameLine();

        /* Info panel (35%) */
        ImGui::BeginChild("##mdlinfo_panel", ImVec2(0, avail.y), false);
        {
            ImGui::TextColored(JCE_COLOR_ACCENT, "%s", jce_editor_i18n("viewer.modelInfo"));
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Columns(2, "##mdlinfo2", false);
            ImGui::SetColumnWidth(0, 100);

            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.format"));
            ImGui::NextColumn(); ImGui::Text("%s", fmt_name); ImGui::NextColumn();

            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.loader"));
            ImGui::NextColumn(); ImGui::Text("%s", jce_editor_i18n("viewer.loaderAssimp")); ImGui::NextColumn();

            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.meshes"));
            ImGui::NextColumn(); ImGui::Text("%d", ms->mesh_count); ImGui::NextColumn();

            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.vertices"));
            ImGui::NextColumn(); ImGui::Text("%d", ms->vert_count); ImGui::NextColumn();

            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.faces"));
            ImGui::NextColumn(); ImGui::Text("%d", ms->face_count); ImGui::NextColumn();

            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.fileSize"));
            ImGui::NextColumn();
            if (tab->file_size >= 1024 * 1024)
                ImGui::Text("%.2f MB", (double)tab->file_size / (1024.0 * 1024.0));
            else
                ImGui::Text("%.1f KB", (double)tab->file_size / 1024.0);
            ImGui::NextColumn();

            if (ms->vert_count > 0) {
                ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.boundingBox"));
                ImGui::NextColumn();
                ImGui::Text("%.2f x %.2f x %.2f",
                    ms->maxX - ms->minX, ms->maxY - ms->minY, ms->maxZ - ms->minZ);
                ImGui::NextColumn();
            }
            ImGui::Columns(1);

            if (!ms->materials.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(JCE_COLOR_ACCENT, "%s (%d)",
                    jce_editor_i18n("viewer.materials"),
                    (int)ms->materials.size());
                ImGui::Separator();
                for (auto &m : ms->materials)
                    ImGui::BulletText("%s", m.c_str());
            }

            ImGui::Spacing();
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.path"));
            ImGui::TextWrapped("%s", tab->path);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.controls"));
            ImGui::BulletText("%s", jce_editor_i18n("viewer.controlRotate"));
            ImGui::BulletText("%s", jce_editor_i18n("viewer.controlPan"));
            ImGui::BulletText("%s", jce_editor_i18n("viewer.controlZoom"));
        }
        ImGui::EndChild();

        return;
    }

    /* ── Assimp load failed: show diagnostics + optional source ──── */
    ImGui::Spacing();
    ImGui::TextColored(JCE_COLOR_ACCENT, "%s", jce_editor_i18n("viewer.modelInfo"));
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(JCE_COLOR_TEXT_ERROR, "%s", jce_editor_i18n("viewer.assimpLoadFailed"));
    if (ms->load_error[0])
        ImGui::TextWrapped("%s", ms->load_error);
    ImGui::Spacing();

    ImGui::Columns(2, "##mdlinfo", false);
    ImGui::SetColumnWidth(0, 120);

    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.format"));
    ImGui::NextColumn(); ImGui::Text("%s", fmt_name); ImGui::NextColumn();

    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.fileSize"));
    ImGui::NextColumn();
    if (tab->file_size >= 1024 * 1024)
        ImGui::Text("%.2f MB", (double)tab->file_size / (1024.0 * 1024.0));
    else
        ImGui::Text("%.1f KB", (double)tab->file_size / 1024.0);
    ImGui::NextColumn();

    if (strcmp(tab->ext, ".glb") == 0 && tab->content_len >= 12) {
        const unsigned char *d = (const unsigned char *)tab->content;
        uint32_t version = d[4] | (d[5] << 8) | (d[6] << 16) | (d[7] << 24);
        uint32_t length  = d[8] | (d[9] << 8) | (d[10] << 16) | (d[11] << 24);
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.glbVersion"));
        ImGui::NextColumn(); ImGui::Text("%u", version); ImGui::NextColumn();
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.totalSize"));
        ImGui::NextColumn(); ImGui::Text("%u bytes", length); ImGui::NextColumn();
    }

    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.path"));
    ImGui::NextColumn(); ImGui::TextWrapped("%s", tab->path); ImGui::NextColumn();
    ImGui::Columns(1);

    if (strcmp(tab->ext, ".gltf") == 0 || strcmp(tab->ext, ".obj") == 0
        || strcmp(tab->ext, ".dae") == 0) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.source"));
        fv_render_code(tab);
    }
}
