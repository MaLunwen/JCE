/*
 * jce_morph_gpu.c -- see jce_morph_gpu.h for why this exists and what it does
 * NOT replace.
 */
#include "middleware/animation/jce_morph_gpu.h"

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/renderer/jce_views.h>

#include "middleware/animation/jce_morph_internal.h"
#include "os/core/jce_memory.h"
#include "renderer/jce_shader_load.h"

#include <stdlib.h>
#include <string.h>

#define LOG_TAG "morph_gpu"

/* One entry per morph-bearing PRIMITIVE in the process.  A face rig is one or
 * two; a crowd of a hundred instances of that rig is still one or two, which
 * is the whole point of keying on the model's data rather than the instance.
 * Overflow falls back to the CPU path rather than evicting: evicting a live
 * entry would free device buffers a dispatch this frame still references. */
#define MORPH_GPU_MAX_ENTRIES 64

typedef struct {
    const JceMorphData *key;          /* NULL = free slot                     */
    bgfx_vertex_buffer_handle_t base; /* COMPUTE_READ float[]  base vertices  */
    bgfx_vertex_buffer_handle_t delta;/* COMPUTE_READ float[]  target-major   */
    uint32_t num_verts;
    uint32_t num_targets;
    uint32_t stride_floats;
    uint32_t pos_off_floats;
    int32_t  nrm_off_floats;          /* < 0 = layout has no NORMAL           */
    bool     has_normals;             /* the DATA carries normal deltas       */
} MorphGpuEntry;

/* ONE file-scope struct, not five statics.  The dedup audit counts file-scope
 * mutable state and it is right to: five names that are only ever used
 * together are one object that has not been written down as one. */
static struct {
    MorphGpuEntry        entries[MORPH_GPU_MAX_ENTRIES];
    uint32_t             count;
    bgfx_program_handle_t program;
    bgfx_uniform_handle_t u_params;   /* {nv, nt, stride_floats, has_normals} */
    bgfx_uniform_handle_t u_offsets;  /* {pos_off, nrm_off, 0, 0}             */
    bgfx_uniform_handle_t u_weights;  /* vec4[4] = JCE_MORPH_MAX_WEIGHTS      */
    bool                  program_tried;
    bool                  full_warned;
} s_mg = {
    { { 0 } }, 0,
    { UINT16_MAX }, { UINT16_MAX }, { UINT16_MAX }, { UINT16_MAX },
    false, false
};

/* The shader's u_morph_weights is vec4[4].  If the authored ceiling ever moves
 * the two drift silently -- the extra targets would simply read whatever the
 * last upload left in the unused lanes -- so it is a build error instead. */
#if JCE_MORPH_MAX_WEIGHTS != 16
#error "cs_morph_deform.sc declares u_morph_weights as vec4[4] = 16 floats; \
JCE_MORPH_MAX_WEIGHTS no longer matches.  Change both."
#endif

#define MORPH_GPU_THREADS 64u   /* must equal NUM_THREADS in the shader */

bool jce_morph_gpu_available(void)
{
    /* JCE_MORPH_CPU=1 forces the CPU evaluator on a backend that could run
     * the dispatch.  This is not a debug convenience: it is the ABLATION the
     * equivalence measurement needs.  Two captures that differ only in this
     * variable are the only way to say the two evaluators agree, and a knob
     * that exists only in a patch is a measurement nobody can re-run. */
    const char *force = getenv("JCE_MORPH_CPU");
    if (force && force[0] && force[0] != '0') return false;

    const bgfx_caps_t *caps = bgfx_get_caps();
    return caps && (caps->supported & BGFX_CAPS_COMPUTE) != 0;
}

uint32_t jce_morph_gpu_entry_count(void)
{
    return s_mg.count;
}

static bool ensure_program(const JcePakArchive *pak)
{
    if (s_mg.program.idx != UINT16_MAX) return true;
    if (s_mg.program_tried) return false;   /* one attempt, then stay quiet */
    s_mg.program_tried = true;

    const char *sfx = jce_shader_backend_suffix();
    if (!sfx) {
        LOG_WARN(LOG_TAG, "no shader suffix for this backend; morph stays on "
                          "the CPU path");
        return false;
    }
    bgfx_shader_handle_t cs =
        jce_shader_load_from_pak(pak, "cs_morph_deform", sfx, LOG_TAG);
    if (cs.idx == UINT16_MAX) {
        LOG_WARN(LOG_TAG, "cs_morph_deform_%s not available; morph stays on "
                          "the CPU path", sfx);
        return false;
    }
    s_mg.program = bgfx_create_compute_program(cs, true);
    if (s_mg.program.idx == UINT16_MAX) {
        LOG_WARN(LOG_TAG, "compute program creation failed; morph stays on "
                          "the CPU path");
        return false;
    }
    s_mg.u_params  = bgfx_create_uniform("u_morph_params",  BGFX_UNIFORM_TYPE_VEC4, 1);
    s_mg.u_offsets = bgfx_create_uniform("u_morph_offsets", BGFX_UNIFORM_TYPE_VEC4, 1);
    s_mg.u_weights = bgfx_create_uniform("u_morph_weights", BGFX_UNIFORM_TYPE_VEC4, 4);
    /* From here a JceMorphData can have device buffers, so from here its
     * destruction has to reach us.  Registered at the point the first
     * upload becomes possible rather than at startup, so a build that
     * never morphs never installs a hook. */
    jce_morph_set_destroy_hook(jce_morph_gpu_release);
    LOG_INFO(LOG_TAG, "GPU morph deform active (cs_morph_deform_%s)", sfx);
    return true;
}

static MorphGpuEntry *find_entry(const JceMorphData *md)
{
    for (uint32_t i = 0; i < MORPH_GPU_MAX_ENTRIES; ++i)
        if (s_mg.entries[i].key == md) return &s_mg.entries[i];
    return NULL;
}

static void drop_entry(MorphGpuEntry *e)
{
    if (e->base.idx  != UINT16_MAX) bgfx_destroy_vertex_buffer(e->base);
    if (e->delta.idx != UINT16_MAX) bgfx_destroy_vertex_buffer(e->delta);
    memset(e, 0, sizeof(*e));
    e->key = NULL;
    if (s_mg.count) s_mg.count--;
}

/* A layout of one float3.  Used only to give the delta buffer a byte size --
 * the COMPUTE_READ view of it is float-addressed (FORMAT_32X1), so the layout
 * is the allocation's unit and nothing reads it as an attribute. */
static void float3_layout(bgfx_vertex_layout_t *out)
{
    bgfx_vertex_layout_begin(out, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(out, BGFX_ATTRIB_POSITION, 3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(out);
}

static MorphGpuEntry *build_entry(const JceMorphData *md,
                                  const JceSkinnedMesh *sm,
                                  uint32_t num_verts)
{
    const void *base_bytes = jce_skinned_mesh_base_verts(sm);
    const void *layout_v   = jce_skinned_mesh_layout(sm);
    uint32_t    stride     = jce_skinned_mesh_stride(sm);
    if (!base_bytes || !layout_v || stride == 0) return NULL;

    /* Every index in the shader is in FLOATS, which only works because a
     * vertex layout's stride and its attribute offsets are multiples of 4
     * bytes.  bgfx guarantees that; assert it anyway rather than silently
     * deform the wrong fields if it ever stops being true. */
    uint32_t pos_off = jce_skinned_mesh_pos_offset(sm);
    uint32_t nrm_off = jce_skinned_mesh_normal_offset(sm);
    if ((stride % 4u) != 0u || (pos_off % 4u) != 0u || (nrm_off % 4u) != 0u) {
        LOG_WARN(LOG_TAG, "layout is not 4-byte aligned (stride=%u pos=%u "
                          "nrm=%u); morph stays on the CPU path",
                 stride, pos_off, nrm_off);
        return NULL;
    }

    uint32_t nt = jce_morph_target_count(md);
    bool     hn = jce_morph_has_normals(md);
    if (nt == 0) return NULL;
    if (nt > JCE_MORPH_MAX_WEIGHTS) nt = JCE_MORPH_MAX_WEIGHTS;

    MorphGpuEntry *slot = find_entry(NULL);
    if (!slot) {
        if (!s_mg.full_warned) {
            s_mg.full_warned = true;
            LOG_WARN(LOG_TAG,
                     "morph GPU cache full at %d primitives; the rest stay on "
                     "the CPU path (raise MORPH_GPU_MAX_ENTRIES)",
                     MORPH_GPU_MAX_ENTRIES);
        }
        return NULL;
    }

    /* ── base vertices ───────────────────────────────────────────────
     * A STATIC vertex buffer, not a transient one: jce_gpu_scene.c records
     * that a transient VB has no D3D11 SRV, so a COMPUTE_READ of it returns
     * zeros -- the same trap, and it would show here as a mesh collapsing to
     * the origin rather than as an error. */
    const bgfx_memory_t *bm = bgfx_copy(base_bytes, num_verts * stride);
    if (!bm) return NULL;
    bgfx_vertex_buffer_handle_t base_vb = bgfx_create_vertex_buffer(
        bm, (const bgfx_vertex_layout_t *)layout_v,
        BGFX_BUFFER_COMPUTE_READ | BGFX_BUFFER_COMPUTE_FORMAT_32X1 |
        BGFX_BUFFER_COMPUTE_TYPE_FLOAT);
    if (base_vb.idx == UINT16_MAX) return NULL;

    /* ── deltas, target-major, exactly as JceMorphData holds them ──── */
    uint32_t per_target = num_verts * 3u;
    uint32_t floats     = nt * per_target * (hn ? 2u : 1u);
    const bgfx_memory_t *dm = bgfx_alloc(floats * (uint32_t)sizeof(float));
    if (!dm) { bgfx_destroy_vertex_buffer(base_vb); return NULL; }

    float *dst = (float *)dm->data;
    for (uint32_t t = 0; t < nt; ++t) {
        const jce_vec3 *p = jce_morph_position_deltas(md, t);
        if (p) memcpy(dst + (size_t)t * per_target, p,
                      (size_t)per_target * sizeof(float));
        else   memset(dst + (size_t)t * per_target, 0,
                      (size_t)per_target * sizeof(float));
    }
    if (hn) {
        float *nbase = dst + (size_t)nt * per_target;
        for (uint32_t t = 0; t < nt; ++t) {
            const jce_vec3 *n = jce_morph_normal_deltas(md, t);
            if (n) memcpy(nbase + (size_t)t * per_target, n,
                          (size_t)per_target * sizeof(float));
            else   memset(nbase + (size_t)t * per_target, 0,
                          (size_t)per_target * sizeof(float));
        }
    }

    bgfx_vertex_layout_t f3;
    float3_layout(&f3);
    bgfx_vertex_buffer_handle_t delta_vb = bgfx_create_vertex_buffer(
        dm, &f3,
        BGFX_BUFFER_COMPUTE_READ | BGFX_BUFFER_COMPUTE_FORMAT_32X1 |
        BGFX_BUFFER_COMPUTE_TYPE_FLOAT);
    if (delta_vb.idx == UINT16_MAX) {
        bgfx_destroy_vertex_buffer(base_vb);
        return NULL;
    }

    slot->key            = md;
    slot->base           = base_vb;
    slot->delta          = delta_vb;
    slot->num_verts      = num_verts;
    slot->num_targets    = nt;
    slot->stride_floats  = stride / 4u;
    slot->pos_off_floats = pos_off / 4u;
    slot->nrm_off_floats = (int32_t)(nrm_off / 4u);
    slot->has_normals    = hn;
    s_mg.count++;

    LOG_INFO(LOG_TAG,
             "uploaded %u target(s) x %u vert(s) (%u KB deltas, normals=%s); "
             "shared by every instance of this model",
             nt, num_verts,
             (unsigned)((floats * sizeof(float)) / 1024u), hn ? "yes" : "no");
    return slot;
}

bool jce_morph_gpu_prepare(const JcePakArchive *pak,
                           const JceMorphData *md,
                           const JceSkinnedMesh *sm,
                           uint32_t num_verts)
{
    if (!md || !sm || num_verts == 0) return false;
    if (!jce_morph_gpu_available()) return false;
    if (!ensure_program(pak)) return false;

    MorphGpuEntry *e = find_entry(md);
    if (!e) e = build_entry(md, sm, num_verts);
    if (!e) return false;

    /* The mesh the caller is deforming must be the one the entry was built
     * from.  A LOD swap under the same JceMorphData would otherwise dispatch
     * the old vertex count over the new buffer. */
    return e->num_verts == num_verts;
}

bool jce_morph_gpu_deform(const JceMorphData *md,
                          const JceSkinnedMesh *sm,
                          uint32_t num_verts,
                          const float *weights, uint32_t num_weights,
                          bgfx_dynamic_vertex_buffer_handle_t out)
{
    (void)sm;
    if (!md || !weights || num_weights == 0 || num_verts == 0) return false;
    if (out.idx == UINT16_MAX) return false;

    /* prepare() is what decides whether this path runs, and the caller has
     * already called it -- reaching here with no entry is a wiring mistake,
     * not a capability. */
    MorphGpuEntry *e = find_entry(md);
    if (!e || e->num_verts != num_verts) return false;

    JCE_PROFILE_ZONE_N("Anim::MorphDeformGPU");

    /* min(targets, weights) -- the same clamp jce_morph_apply makes, so the
     * two evaluators read the same number of targets for the same input. */
    uint32_t nt = e->num_targets < num_weights ? e->num_targets : num_weights;

    float params[4];
    params[0] = (float)num_verts;
    params[1] = (float)nt;
    params[2] = (float)e->stride_floats;
    params[3] = e->has_normals ? 1.0f : 0.0f;
    bgfx_set_uniform(s_mg.u_params, params, 1);

    float offsets[4];
    offsets[0] = (float)e->pos_off_floats;
    offsets[1] = (float)e->nrm_off_floats;
    offsets[2] = 0.0f;
    offsets[3] = 0.0f;
    bgfx_set_uniform(s_mg.u_offsets, offsets, 1);

    /* The lanes past nt are zeroed rather than left stale.  The shader reads
     * only nt of them, so this changes no pixel -- it changes what a capture
     * shows when somebody is trying to find out why a shape is stuck on. */
    float w[JCE_MORPH_MAX_WEIGHTS];
    memset(w, 0, sizeof(w));
    for (uint32_t t = 0; t < nt && t < JCE_MORPH_MAX_WEIGHTS; ++t) w[t] = weights[t];
    bgfx_set_uniform(s_mg.u_weights, w, 4);

    bgfx_set_compute_vertex_buffer(0, e->base,  BGFX_ACCESS_READ);
    bgfx_set_compute_vertex_buffer(1, e->delta, BGFX_ACCESS_READ);
    bgfx_set_compute_dynamic_vertex_buffer(2, out, BGFX_ACCESS_WRITE);

    uint32_t groups = (num_verts + MORPH_GPU_THREADS - 1u) / MORPH_GPU_THREADS;
    bgfx_dispatch(JCE_VIEW_MORPH_DEFORM, s_mg.program, groups, 1, 1,
                  BGFX_DISCARD_ALL);

    JCE_PROFILE_ZONE_END;
    return true;
}

void jce_morph_gpu_release(const JceMorphData *md)
{
    if (!md) return;
    MorphGpuEntry *e = find_entry(md);
    if (e) drop_entry(e);
}

void jce_morph_gpu_shutdown(void)
{
    jce_morph_set_destroy_hook(NULL);
    for (uint32_t i = 0; i < MORPH_GPU_MAX_ENTRIES; ++i)
        if (s_mg.entries[i].key) drop_entry(&s_mg.entries[i]);
    if (s_mg.u_params.idx  != UINT16_MAX) bgfx_destroy_uniform(s_mg.u_params);
    if (s_mg.u_offsets.idx != UINT16_MAX) bgfx_destroy_uniform(s_mg.u_offsets);
    if (s_mg.u_weights.idx != UINT16_MAX) bgfx_destroy_uniform(s_mg.u_weights);
    if (s_mg.program.idx   != UINT16_MAX) bgfx_destroy_program(s_mg.program);
    s_mg.u_params.idx  = UINT16_MAX;
    s_mg.u_offsets.idx = UINT16_MAX;
    s_mg.u_weights.idx = UINT16_MAX;
    s_mg.program.idx   = UINT16_MAX;
    s_mg.program_tried = false;
    s_mg.full_warned   = false;
    s_mg.count         = 0;
}
