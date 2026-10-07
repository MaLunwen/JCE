#include <jce/middleware/scene/jce_scene_stress.h>
#include <jce/middleware/scene/jce_foliage.h>
#include <math.h>
#include <string.h>

static JceEntity stress_entity(JceScene *scene, const char *name, JceEntity parent,
                              float x, float y, float z)
{
    JceEntity e = jce_scene_create_entity(scene, name);
    if (e == JCE_ENTITY_INVALID) return e;
    JceTransform t = {0};
    t.position = jce_v3(x, y, z);
    t.rotation = jce_q_identity();
    t.scale = jce_v3(1, 1, 1);
    jce_scene_set_transform(scene, e, &t);
    if (parent != JCE_ENTITY_INVALID)
        jce_scene_set_parent(scene, e, parent);
    JceEditorMeta meta;
    jce_editor_meta_init(&meta);
    meta.enabled = true;
    jce_scene_set_editor_meta(scene, e, &meta);
    return e;
}

static bool stress_scatter(JceScene *scene, const JceSceneStressDesc *desc,
                           JceSceneStressResult *result)
{
    uint32_t seed = 1;
    while (result->spawned < desc->count) {
        JceEntity e = stress_entity(scene, "BenchScatter", result->root, 0, 0, 0);
        if (e == JCE_ENTITY_INVALID) return false;
        const uint32_t remaining = desc->count - result->spawned;
        const uint32_t count = remaining < JCE_FOLIAGE_MAX_INSTANCES
                             ? remaining : JCE_FOLIAGE_MAX_INSTANCES;
        JceVegetationScatterComponent vs = {0};
        vs.visible = true;
        vs.mesh_shape = desc->kind == JCE_SCENE_STRESS_TRIANGLE ? 1u : 0u;
        /* Independent deterministic chunks cover the same requested area.
         * Each component retains the renderer's ordinary resource bound. */
        vs.area_x = vs.area_z = sqrtf((float)desc->count) * 2.0f;
        vs.density = (float)count / (vs.area_x * vs.area_z);
        uint32_t actual = 0;
        int attempt;
        for (attempt = 0; attempt < 8; ++attempt) {
            actual = (uint32_t)((double)vs.density * vs.area_x * vs.area_z);
            if (actual >= count) break;
            vs.density = nextafterf(vs.density, INFINITY);
        }
        if (actual != count) return false;
        /* Match the two-unit scatter cell spacing so large-count fixtures
         * retain raster coverage under antialiasing at fixed resolution. */
        vs.scale_min = vs.scale_max = 2;
        /* Keep subpixel instances distinguishable from neutral backgrounds. */
        vs.tint[0] = 1;
        vs.tint[1] = vs.tint[2] = 0.05f;
        vs.max_slope_deg = 90;
        vs.seed = seed++;
        jce_scene_set_vegetation_scatter(scene, e, &vs);
        result->spawned += actual;
    }
    return true;
}

static bool stress_grid(JceScene *scene, const JceSceneStressDesc *desc,
                        JceSceneStressResult *result)
{
    const bool physics = desc->kind == JCE_SCENE_STRESS_PHYSICS;
    const float spacing = physics ? 1.6f : 1.5f;
    const bool spread = desc->spread > 0;
    uint32_t side = 1;
    while ((spread ? (uint64_t)side * side : (uint64_t)side * side * side) < desc->count)
        ++side;
    const uint32_t sy = spread ? 1 : side;
    const float step = spread && side > 1 ? desc->spread / (float)(side - 1) : spacing;
    const float ox = spread ? -desc->spread * 0.5f : -0.5f * (side - 1) * spacing;
    const float oy = spread ? 0 : ox;
    uint32_t x, y, z;
    for (x = 0; x < side && result->spawned < desc->count; ++x)
    for (y = 0; y < sy && result->spawned < desc->count; ++y)
    for (z = 0; z < side && result->spawned < desc->count; ++z) {
        const uint32_t i = result->spawned;
        const float py = spread ? (float)((i * 17) % 23) * 0.35f : oy + y * spacing;
        JceEntity e = stress_entity(scene, "b", result->root,
                                   ox + x * step, py, ox + z * step);
        if (e == JCE_ENTITY_INVALID) return false;
        JceMeshRenderer mr;
        jce_mesh_renderer_init(&mr);
        mr.visible = true;
        mr.base_color[0] = mr.base_color[1] = mr.base_color[2] = mr.base_color[3] = 1;
        mr.roughness = 0.5f;
        mr.normal_scale = mr.ao_strength = 1;
        mr.alpha_cutoff = 0.5f;
        if (desc->kind == JCE_SCENE_STRESS_DRAWCALL) {
            mr.base_color[0] = 0.15f + 0.7f * (float)((i * 13) % 101) / 101;
            mr.base_color[1] = 0.15f + 0.7f * (float)((i * 37) % 103) / 103;
            mr.base_color[2] = 0.15f + 0.7f * (float)((i * 71) % 107) / 107;
            mr.roughness = 0.05f + 0.9f * (float)((i * 53) % 991) / 991;
            mr.metallic = (float)((i * 29) % 251) / 251;
        }
        jce_scene_set_mesh_renderer(scene, e, &mr);
        if (physics) {
            JceRigidBodyComponent rb = {0};
            JceBoxColliderComponent box = {0};
            rb.body_type = 1;
            rb.mass = 1;
            rb.friction = 0.5f;
            rb.use_gravity = true;
            rb.gravity_scale = 1;
            rb.angular_drag = 0.05f;
            box.size[0] = box.size[1] = box.size[2] = 1;
            jce_scene_set_rigidbody(scene, e, &rb);
            jce_scene_set_box_collider(scene, e, &box);
        }
        ++result->spawned;
    }
    return result->spawned == desc->count;
}

bool jce_scene_stress_spawn(JceScene *scene, const JceSceneStressDesc *desc,
                            JceSceneStressResult *result)
{
    if (!result) return false;
    memset(result, 0, sizeof(*result));
    if (!scene || !desc || desc->count == 0 || desc->count > 2000000 ||
        desc->kind < JCE_SCENE_STRESS_DRAWCALL || desc->kind > JCE_SCENE_STRESS_PHYSICS ||
        !isfinite(desc->spread) || desc->spread < 0)
        return false;
    const bool scatter = desc->kind == JCE_SCENE_STRESS_INSTANCING ||
                         desc->kind == JCE_SCENE_STRESS_TRIANGLE;
    result->root = stress_entity(scene, "Benchmark", JCE_ENTITY_INVALID, 0, 0, 0);
    if (result->root == JCE_ENTITY_INVALID) return false;
    const bool ok = scatter ? stress_scatter(scene, desc, result)
                            : stress_grid(scene, desc, result);
    if (!ok) {
        jce_scene_destroy_entity(scene, result->root);
        memset(result, 0, sizeof(*result));
    }
    return ok;
}
