/*
 * jce_panel_static_batch.cpp  Static batching, as a Build Profiles tab.
 *
 * WHY A TAB AND NOT A PANEL.  Static batching is a BUILD-time optimisation --
 * it is a Player Setting in Unity and a Build Settings switch in Godot -- so it
 * belongs beside the build profiles rather than in a window of its own.  A new
 * panel would also cost the seven editor touchpoints (menu, six layout sites,
 * fifteen locales, the user guide) for a feature that is one report and one
 * button.
 *
 * WHAT IT DOES.  Scans the open scene for STATIC mesh renderers, groups them by
 * material, and reports how many draw calls the merge would remove -- before
 * anything is written.  Baking then writes one .glb per group into the
 * project's models/batched/, adds one entity per group, and DISABLES the source
 * entities rather than deleting them.
 *
 * DISABLING AND NOT DELETING is the whole safety story: the merge is a
 * derived, regenerable artefact and the authored entities are the source of
 * truth.  A designer who dislikes the result re-enables them; nothing authored
 * was destroyed, and no undo transaction has to be able to reconstruct a
 * hundred entities.  It is also what makes re-baking idempotent: an already
 * batched source is disabled, and disabled entities are not candidates.
 */

#include "jce_panel_common.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_i18n.h"
#include "scene/jce_editor_scene_asset_cache.h"
#include "core/jce_assetdb.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_static_batch.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

/* One scanned candidate: everything the bake and the report need. */
struct SbCandidate {
    JceEntity   entity;
    std::string mesh_host;      /* resolved model file on the host FS */
    std::string material_id;    /* what "same material" means, spelled out */
    uint64_t    material_key;
    jce_mat4    world;
    float       base_color[4];
};

struct SbScan {
    std::vector<SbCandidate> cands;
    uint32_t skipped_dynamic  = 0;
    uint32_t skipped_no_mesh  = 0;
    uint32_t skipped_disabled = 0;
    uint32_t skipped_unresolved = 0;
};

/* ONE panel state, the way s_editor and g_tabs are one each.  Seven separate
 * file-statics is both un-idiomatic here and three counts on the dedup audit's
 * global-state ledger. */
static struct SbPanel {
    SbScan  scan;
    bool    scanned      = false;
    char    status[512]  = "";
    JceStaticBatchStats last{};
    bool    have_last    = false;
    int     min_group    = 2;
    int     max_vertices = 65536;
} g_sb;

static uint64_t sb_fnv(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

/* WHAT "THE SAME MATERIAL" MEANS, in one place.
 *
 * Everything the merged mesh will render with ONE value of.  Two objects whose
 * albedo textures differ must not merge -- the merged mesh has one material and
 * one of them would silently take the other's texture.  This is deliberately
 * the whole visible material state and not just materialPath: a scene that
 * tints two instances of one material differently would otherwise merge them
 * and lose a colour. */
static std::string sb_material_id(const JceMeshRenderer *mr)
{
    char buf[1400];
    std::snprintf(buf, sizeof buf,
                  "mat=%s|alb=%s|mr=%s|nrm=%s|ao=%s|emi=%s|"
                  "c=%.4f,%.4f,%.4f,%.4f|m=%.4f|r=%.4f|e=%.4f,%.4f,%.4f|"
                  "ns=%.4f|aos=%.4f|am=%d|ac=%.4f|ds=%d|cs=%d|rs=%d|prio=%d",
                  mr->material_path ? mr->material_path : "",
                  mr->albedo_tex ? mr->albedo_tex : "",
                  mr->mr_tex ? mr->mr_tex : "",
                  mr->normal_tex ? mr->normal_tex : "",
                  mr->ao_tex ? mr->ao_tex : "",
                  mr->emissive_tex ? mr->emissive_tex : "",
                  (double)mr->base_color[0], (double)mr->base_color[1],
                  (double)mr->base_color[2], (double)mr->base_color[3],
                  (double)mr->metallic, (double)mr->roughness,
                  (double)mr->emissive[0], (double)mr->emissive[1],
                  (double)mr->emissive[2],
                  (double)mr->normal_scale, (double)mr->ao_strength,
                  mr->alpha_mode, (double)mr->alpha_cutoff,
                  mr->double_sided ? 1 : 0,
                  mr->shadow_cast_off ? 0 : 1,
                  mr->shadow_receive_off ? 0 : 1,
                  mr->render_priority);
    return std::string(buf);
}

static void sb_scan_entity(JceScene *scene, JceEntity e, void *ud)
{
    SbScan *sc = (SbScan *)ud;
    if (!scene || !sc) return;

    if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_MESH_RENDERER)) {
        ++sc->skipped_disabled;
        return;
    }
    JceEditorMeta *em = jce_scene_get_editor_meta(scene, e);
    if (em && !em->enabled) { ++sc->skipped_disabled; return; }

    JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
    if (!mr || !mr->mesh_path || mr->mesh_path[0] == '\0') {
        ++sc->skipped_no_mesh;
        return;
    }
    if (!mr->visible) { ++sc->skipped_disabled; return; }

    /* STATIC means "nothing moves it at runtime", and the renderer already has
     * that rule -- jce_scene_entity_is_dynamic, which walks the parent chain
     * for a rigidbody / character / skeletal / vehicle / softbody.  Asking it
     * rather than re-deciding here is what keeps "static" meaning one thing. */
    if (jce_scene_entity_is_dynamic(scene, e)) { ++sc->skipped_dynamic; return; }

    /* A LOD group or a custom shader program changes what the draw does, not
     * just what it looks like, so those are left alone. */
    if (jce_scene_has_lod_group(scene, e)) { ++sc->skipped_dynamic; return; }
    if (mr->has_custom_program)            { ++sc->skipped_dynamic; return; }

    char host[1024];
    if (!jce_editor_scene_asset_cache_resolve_mesh_path(mr->mesh_path, host,
                                                        (int)sizeof host)) {
        ++sc->skipped_unresolved;
        return;
    }

    SbCandidate c;
    c.entity      = e;
    c.mesh_host   = host;
    c.material_id = sb_material_id(mr);
    c.material_key = sb_fnv(1469598103934665603ull,
                            c.material_id.data(), c.material_id.size());
    c.world       = jce_scene_get_world_matrix(scene, e);
    for (int i = 0; i < 4; ++i) c.base_color[i] = mr->base_color[i];
    sc->cands.push_back(std::move(c));
}

static void sb_rescan(JceScene *scene)
{
    g_sb.scan = SbScan{};
    g_sb.scanned = false;
    if (!scene) return;
    jce_scene_invalidate_world_cache(scene);
    jce_scene_each_entity(scene, sb_scan_entity, &g_sb.scan);
    g_sb.scanned = true;
}

/* How many groups the current settings would form, without writing anything.
 * The SAME grouping rule the engine uses -- by material key, min_group, and the
 * vertex cap -- except the vertex cap needs the meshes loaded, so the preview
 * reports groups by material only and says so.  A preview that silently
 * disagreed with the bake would be worse than no preview. */
static void sb_preview(uint32_t *out_groups, uint32_t *out_merged,
                       uint32_t *out_alone)
{
    std::vector<uint64_t> keys;
    std::vector<uint32_t> counts;
    for (const SbCandidate &c : g_sb.scan.cands) {
        size_t i = 0;
        for (; i < keys.size(); ++i) if (keys[i] == c.material_key) break;
        if (i == keys.size()) { keys.push_back(c.material_key); counts.push_back(0); }
        counts[i]++;
    }
    uint32_t g = 0, merged = 0, alone = 0;
    const uint32_t min_group = (uint32_t)(g_sb.min_group < 2 ? 2 : g_sb.min_group);
    for (uint32_t n : counts) {
        if (n >= min_group) { ++g; merged += n; }
        else                { alone += n; }
    }
    *out_groups = g; *out_merged = merged; *out_alone = alone;
}

}  /* namespace */

extern "C" void jce_editor_panel_static_batch_bake(void);

extern "C" void jce_editor_panel_static_batch_content(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) {
        ImGui::TextUnformatted(jce_editor_i18n("panel.staticBatch.noScene"));
        return;
    }

    ImGui::TextWrapped("%s", jce_editor_i18n("panel.staticBatch.what"));
    ImGui::Separator();

    if (ImGui::Button(jce_editor_i18n("panel.staticBatch.scan")) || !g_sb.scanned)
        sb_rescan(scene);

    if (!g_sb.scanned) return;

    uint32_t groups = 0, merged = 0, alone = 0;
    sb_preview(&groups, &merged, &alone);

    ImGui::Text("%s: %zu", jce_editor_i18n("panel.staticBatch.candidates"),
                g_sb.scan.cands.size());
    ImGui::Text("%s: %u", jce_editor_i18n("panel.staticBatch.groups"), groups);
    /* THE NUMBER THIS FEATURE EXISTS FOR.  Everything else on this panel is
     * how it was arrived at. */
    ImGui::Text("%s: %u \xE2\x86\x92 %u",
                jce_editor_i18n("panel.staticBatch.draws"),
                merged + alone, groups + alone);
    ImGui::TextDisabled("%s: %u dynamic, %u no mesh, %u disabled, %u unresolved",
                        jce_editor_i18n("panel.staticBatch.skipped"),
                        g_sb.scan.skipped_dynamic, g_sb.scan.skipped_no_mesh,
                        g_sb.scan.skipped_disabled, g_sb.scan.skipped_unresolved);

    ImGui::Separator();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragInt(jce_editor_i18n_id("panel.staticBatch.minGroup", "SbMinGroup"),
                   &g_sb.min_group, 1.0f, 2, 64);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragInt(jce_editor_i18n_id("panel.staticBatch.maxVerts", "SbMaxVerts"),
                   &g_sb.max_vertices, 1024.0f, 1024, 1000000);
    ImGui::TextDisabled("%s", jce_editor_i18n("panel.staticBatch.maxVerts.help"));

    ImGui::Separator();
    const bool can_bake = groups > 0;
    if (!can_bake) ImGui::BeginDisabled();
    if (ImGui::Button(jce_editor_i18n("panel.staticBatch.bake")))
        jce_editor_panel_static_batch_bake();
    if (!can_bake) ImGui::EndDisabled();

    if (g_sb.status[0]) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", g_sb.status);
    }
    if (g_sb.have_last) {
        ImGui::TextDisabled("%s: %u \xE2\x86\x92 %u draws, %u group(s), "
                            "%u verts, %u tris",
                            jce_editor_i18n("panel.staticBatch.result"),
                            g_sb.last.draws_before, g_sb.last.draws_after,
                            g_sb.last.groups_written, g_sb.last.vertices,
                            g_sb.last.triangles);
    }
}

/* Scan and bake in one call, for a caller with no panel in front of it.
 *
 * THE MEASUREMENT SEAM.  What this feature is FOR is a draw-call count, and a
 * draw-call count comes from a running engine -- so the bake has to be
 * reachable without a mouse, or the only evidence for it would be its own
 * report of what it did.  JCE_STATIC_BATCH_BAKE=1 makes the editor run it once
 * at startup; then JCE_KPI_DRAW_LOG records num_draw for the frames after, and
 * the same scene captured with and without the variable differs in exactly one
 * thing. */
extern "C" void jce_editor_panel_static_batch_bake_now(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) return;
    sb_rescan(scene);
    jce_editor_panel_static_batch_bake();
}

extern "C" void jce_editor_panel_static_batch_bake(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene || g_sb.scan.cands.empty()) return;

    const char *root = jce_assetdb_get_root();
    if (!root || !root[0]) {
        std::snprintf(g_sb.status, sizeof g_sb.status, "%s",
                      jce_editor_i18n("panel.staticBatch.noProject"));
        return;
    }

    char dir[1280];
    /* jce_assetdb_get_root() IS the assets root, so the VFS path below is this
     * host path minus that prefix.  The first version appended
     * "resources/assets" to it as well and wrote
     * <project>/resources/assets/resources/assets/models/batched -- a directory
     * the runtime cannot see, which the bake then reported as a success because
     * writing it had worked. */
    std::snprintf(dir, sizeof dir, "%s/models/batched", root);
    jce_fs_host_create_directory(dir);

    const size_t n = g_sb.scan.cands.size();
    std::vector<JceStaticBatchInstance> inst(n);
    for (size_t i = 0; i < n; ++i) {
        std::memset(&inst[i], 0, sizeof inst[i]);
        inst[i].mesh_host_path = g_sb.scan.cands[i].mesh_host.c_str();
        std::memcpy(inst[i].world, &g_sb.scan.cands[i].world, sizeof inst[i].world);
        inst[i].material_key = g_sb.scan.cands[i].material_key;
        for (int k = 0; k < 4; ++k)
            inst[i].base_color[k] = g_sb.scan.cands[i].base_color[k];
    }

    JceStaticBatchDesc desc{};
    desc.min_group    = (uint32_t)(g_sb.min_group < 2 ? 2 : g_sb.min_group);
    desc.max_vertices = (uint32_t)(g_sb.max_vertices < 1024 ? 1024 : g_sb.max_vertices);

    std::vector<int32_t> group_of(n, -1);
    JceStaticBatchStats st{};
    const bool ok = jce_static_batch_bake(inst.data(), (uint32_t)n, &desc,
                                          dir, "batch", group_of.data(), &st);
    if (!ok) {
        std::snprintf(g_sb.status, sizeof g_sb.status, "%s",
                      jce_editor_i18n("panel.staticBatch.failed"));
        return;
    }

    /* One entity per group, at the ORIGIN with an identity transform: the merge
     * baked every source's world matrix into its vertices, which is exactly
     * what removed the per-object work. */
    for (uint32_t g = 0; g < st.groups_written; ++g) {
        char name[64], vfs[256];
        std::snprintf(name, sizeof name, "StaticBatch_%u", g);
        std::snprintf(vfs, sizeof vfs, "models/batched/batch_%u.glb", g);

        JceEntity be = jce_scene_create_entity(scene, name);
        if (be == JCE_ENTITY_INVALID) continue;

        JceTransform xf;
        std::memset(&xf, 0, sizeof xf);
        xf.position = jce_v3(0.0f, 0.0f, 0.0f);
        xf.rotation = jce_q_identity();
        xf.scale    = jce_v3(1.0f, 1.0f, 1.0f);
        jce_scene_set_transform(scene, be, &xf);

        /* The group's material, taken from its FIRST member -- which is exactly
         * what the material key promised: every member shares it. */
        const JceMeshRenderer *src = nullptr;
        for (size_t i = 0; i < n; ++i) {
            if (group_of[i] == (int32_t)g) {
                src = jce_scene_get_mesh_renderer(scene, g_sb.scan.cands[i].entity);
                break;
            }
        }
        JceMeshRenderer mrc;
        jce_mesh_renderer_init(&mrc);
        if (src) mrc = *src;
        mrc.mesh_path = jce_scene_intern(scene, vfs);
        mrc.mesh_shape = 0;
        jce_scene_set_mesh_renderer(scene, be, &mrc);
    }

    /* The sources are DISABLED, not deleted -- see the file header.
     *
     * Through jce_state_set_entity_enabled, which jce_scene.h names as THE
     * entry for this flag: EditorMeta.enabled is mutated in place, so it bumps
     * no generation of its own and the entry point calls
     * jce_scene_bump_enable_gen for it.  Writing the field directly greys the
     * hierarchy row and changes nothing on screen -- the renderer's kind cache
     * keeps last frame's answer, which is exactly what the first version of
     * this did: forty props reported as batched and forty props still drawn. */
    uint32_t disabled = 0;
    for (size_t i = 0; i < n; ++i) {
        if (group_of[i] < 0) continue;
        jce_state_set_entity_enabled(g_sb.scan.cands[i].entity, false);
        ++disabled;
    }

    g_sb.last = st;
    g_sb.have_last = true;
    std::snprintf(g_sb.status, sizeof g_sb.status,
                  "%s  (%u \xE2\x86\x92 %u, %u disabled)",
                  jce_editor_i18n("panel.staticBatch.done"),
                  st.draws_before, st.draws_after, disabled);
    LOG_INFO("editor", "static batch: %u -> %u draws, %u group(s), %u source "
                       "entities disabled", st.draws_before, st.draws_after,
             st.groups_written, disabled);

    jce_state_mark_scene_modified();
    sb_rescan(scene);
}
