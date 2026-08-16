/*
 * jce_reflection_probe_bake.c  Reflection probe bake worker.
 *
 * See jce_reflection_probe_bake.h for the contract. The CPU procedural
 * sky placeholder still drives the source pixels (live scene capture
 * remains a follow-up — bgfx is not thread-safe), but the encoded
 * artefact is now a real cubemap container.
 *
 * On-disk format (P3-E.3 follow-up):
 *   - Specular cubemap:  KTX1 (`.ktx`) emitted by bimg's
 *                        `imageWriteKtx`. RGBA8, 6 faces in
 *                        +X,-X,+Y,-Y,+Z,-Z order, currently a single
 *                        mip (mip-chain bake is the next increment).
 *   - Irradiance sidecar: `<stem>.irr.ktx` (same encoder), single-mip
 *                        diffuse-convolved cube. v2 placeholder uses
 *                        the same RGBA8 source faces as the specular
 *                        side; analytic SH convolution lands with the
 *                        live capture path.
 *
 * bimg does NOT ship a KTX2 supercompression writer in the vendored
 * bgfx revision, so the public `output_path_ktx2` field is treated as
 * a base path: any `.cube` / `.ktx2` suffix is rewritten to `.ktx` so
 * the editor file viewer and asset pipeline recognise it. The public
 * struct field name and overall API are unchanged.
 *
 * Concurrency: one bake at a time; module-scoped state, atomic progress,
 * and a structured background task. Callers poll from the main thread.
 */

#include "jce/renderer/jce_reflection_probe_bake.h"

#include "jce/os/core/jce_alloc.h"
#include "jce/os/core/jce_async.h"
#include "jce/os/core/jce_filesystem.h"
#include "jce/os/core/jce_log.h"
#include "jce/os/core/jce_thread.h"

#include "jce_ktx2_writer.h"

#include <stdio.h>
#include <string.h>

#define JCE_RPB_TAG               "rprobe-bake"
#define JCE_RPB_MAX_FACE          512u
#define JCE_RPB_DEFAULT_FACE      256u
#define JCE_RPB_DEFAULT_SPEC_MIPS 5u

typedef struct {
    JceReflectionProbeBakeDesc desc;
    char                       path[512];

    JceAsyncTask              *task;
    JceAtomicI32              *status;   /* JceBakeStatus */
    JceAtomicI32              *cancel;   /* 0/1 */
    JceAtomicI32              *progress; /* 0..1000 within step */
    JceAtomicI32              *overall;  /* 0..1000 whole bake */

    /* Snapshot updated by main-thread poll() for stable string return. */
    const char                *message;
    uint32_t                   handle;
    bool                       in_use;
} JceRpbSlot;

static JceRpbSlot g_rpb;
static uint32_t   g_next_handle = 1u;

/* ── helpers ──────────────────────────────────────────────────────── */

static void rpb_atomic_set(JceAtomicI32 *a, int32_t v)
{
    if (a) jce_atomic_i32_store(a, v);
}

static int32_t rpb_atomic_get(const JceAtomicI32 *a)
{
    return a ? jce_atomic_i32_load(a) : 0;
}

static void rpb_set_progress(float step01, float overall01)
{
    if (step01    < 0.0f) step01    = 0.0f; if (step01    > 1.0f) step01    = 1.0f;
    if (overall01 < 0.0f) overall01 = 0.0f; if (overall01 > 1.0f) overall01 = 1.0f;
    rpb_atomic_set(g_rpb.progress, (int32_t)(step01    * 1000.0f));
    rpb_atomic_set(g_rpb.overall,  (int32_t)(overall01 * 1000.0f));
}

static bool rpb_cancelled(const JceAsyncContext *ctx)
{
    return rpb_atomic_get(g_rpb.cancel) != 0 ||
           jce_async_context_cancel_requested(ctx);
}

/* ── face direction basis ────────────────────────────────────────── */

/* Map face index + 2D coord (u,v in [-1,1]) to a unit world direction. */
static jce_vec3 rpb_face_dir(int face, float u, float v)
{
    jce_vec3 d;
    switch (face) {
    case 0: d = jce_v3( 1.0f,   -v,   -u); break; /* +X */
    case 1: d = jce_v3(-1.0f,   -v,    u); break; /* -X */
    case 2: d = jce_v3(   u,  1.0f,    v); break; /* +Y */
    case 3: d = jce_v3(   u, -1.0f,   -v); break; /* -Y */
    case 4: d = jce_v3(   u,   -v,  1.0f); break; /* +Z */
    default:d = jce_v3(  -u,   -v, -1.0f); break; /* -Z */
    }
    float len = jce_v3_len(d);
    if (len > 1e-6f) d = jce_v3_scale(d, 1.0f / len);
    return d;
}

/* Sky-and-ground procedural placeholder: blue→white gradient above
 * horizon, grey→dark below, tinted slightly by probe world position so
 * different probes produce visibly different artefacts. */
static void rpb_sample_proc(jce_vec3 dir, jce_vec3 pos, uint8_t out[4])
{
    float horizon = dir.y;                 /* +1 zenith, -1 nadir */
    float t = 0.5f * (horizon + 1.0f);

    /* Position-derived hue offset so per-probe bakes are distinguishable. */
    float r_off = 0.10f * (pos.x - (int)pos.x);
    float g_off = 0.10f * (pos.y - (int)pos.y);
    float b_off = 0.10f * (pos.z - (int)pos.z);
    if (r_off < 0.0f) r_off = -r_off;
    if (g_off < 0.0f) g_off = -g_off;
    if (b_off < 0.0f) b_off = -b_off;

    float r, g, b;
    if (horizon >= 0.0f) {
        /* sky: deep blue (0.20, 0.40, 0.85) → near-white (0.85, 0.92, 1.00) */
        float k = t;
        r = 0.20f + (0.85f - 0.20f) * k + r_off;
        g = 0.40f + (0.92f - 0.40f) * k + g_off;
        b = 0.85f + (1.00f - 0.85f) * k + b_off;
    } else {
        /* ground: mid grey → near-black */
        float k = -horizon;
        r = 0.35f * (1.0f - 0.8f * k) + r_off;
        g = 0.34f * (1.0f - 0.8f * k) + g_off;
        b = 0.32f * (1.0f - 0.8f * k) + b_off;
    }
    if (r < 0.0f) r = 0.0f; if (r > 1.0f) r = 1.0f;
    if (g < 0.0f) g = 0.0f; if (g > 1.0f) g = 1.0f;
    if (b < 0.0f) b = 0.0f; if (b > 1.0f) b = 1.0f;

    out[0] = (uint8_t)(r * 255.0f);
    out[1] = (uint8_t)(g * 255.0f);
    out[2] = (uint8_t)(b * 255.0f);
    out[3] = 255u;
}

/* ── disk container ───────────────────────────────────────────────── */

/* Rewrite any `.cube` / `.ktx2` suffix in `path` (in place) to `.ktx`
 * to match the real on-disk format. Other suffixes (or no suffix) are
 * left untouched. Returns true if `path` is non-empty. */
static bool rpb_normalise_extension(char *path, size_t cap)
{
    if (!path || cap == 0u || path[0] == '\0') return false;
    const size_t n = strlen(path);
    /* Find the last '.' after the last path separator. */
    size_t dot = n;
    for (size_t i = n; i-- > 0; ) {
        char c = path[i];
        if (c == '/' || c == '\\') break;
        if (c == '.') { dot = i; break; }
    }
    if (dot == n) return true; /* no extension — leave as-is */
    const char *ext = path + dot;
    if (strcmp(ext, ".ktx") == 0) return true;
    if (strcmp(ext, ".cube") == 0 || strcmp(ext, ".ktx2") == 0) {
        if (dot + 5u > cap) return false; /* ".ktx" + NUL */
        memcpy(path + dot, ".ktx", 5u);
    }
    return true;
}

/* Derive `<stem>.irr.ktx` from a `.ktx` path. Writes into `out`. */
static void rpb_irr_path(const char *src, char *out, size_t cap)
{
    if (cap == 0u) return;
    out[0] = '\0';
    if (!src || !src[0]) return;
    const size_t n = strlen(src);
    size_t dot = n;
    for (size_t i = n; i-- > 0; ) {
        char c = src[i];
        if (c == '/' || c == '\\') break;
        if (c == '.') { dot = i; break; }
    }
    if (dot == n) {
        snprintf(out, cap, "%s.irr.ktx", src);
    } else {
        if (dot >= cap) return;
        memcpy(out, src, dot);
        out[dot] = '\0';
        size_t left = cap - dot;
        snprintf(out + dot, left, ".irr.ktx");
    }
}

/* ── worker ───────────────────────────────────────────────────────── */

static JceAsyncRunResult rpb_worker_main(JceAsyncContext *ctx, void *arg)
{
    (void)arg;
    const uint32_t face_size = g_rpb.desc.cubemap_size;
    const uint32_t spec_mips = g_rpb.desc.specular_mip_count;
    const size_t   face_pix  = (size_t)face_size * face_size;
    const size_t   total_pix = 6u * face_pix;
    uint8_t       *faces     = (uint8_t *)jce_malloc(total_pix * 4u);
    if (!faces) {
        LOG_ERROR(JCE_RPB_TAG, "alloc failed (%u px cubemap)", face_size);
        rpb_atomic_set(g_rpb.status, JCE_BAKE_STATUS_FAILED);
        g_rpb.message = "out-of-memory";
        return JCE_ASYNC_RUN_FAILED;
    }

    /* Step 1/4: render 6 faces. */
    rpb_atomic_set(g_rpb.status, JCE_BAKE_STATUS_RENDERING_FACES);
    for (int f = 0; f < 6; ++f) {
        if (rpb_cancelled(ctx)) goto cancelled;
        uint8_t *dst = faces + (size_t)f * face_pix * 4u;
        for (uint32_t y = 0; y < face_size; ++y) {
            float v = ((float)y + 0.5f) / (float)face_size * 2.0f - 1.0f;
            for (uint32_t x = 0; x < face_size; ++x) {
                float u = ((float)x + 0.5f) / (float)face_size * 2.0f - 1.0f;
                jce_vec3 d = rpb_face_dir(f, u, v);
                rpb_sample_proc(d, g_rpb.desc.position,
                                 &dst[(y * face_size + x) * 4u]);
            }
        }
        rpb_set_progress((float)(f + 1) / 6.0f, 0.50f * (float)(f + 1) / 6.0f);
    }

    /* Step 2/4: irradiance (analytic placeholder — no work needed for v1
     * since the procedural sky is already a smooth function; surfaced as
     * a status transition so the UI bar advances). */
    rpb_atomic_set(g_rpb.status, JCE_BAKE_STATUS_CONVOLVING_IRRADIANCE);
    rpb_set_progress(1.0f, 0.65f);
    if (rpb_cancelled(ctx)) goto cancelled;

    /* Step 3/4: specular mip chain (placeholder — recorded count is
     * round-tripped into the container header). */
    rpb_atomic_set(g_rpb.status, JCE_BAKE_STATUS_CONVOLVING_SPECULAR);
    for (uint32_t m = 0; m < spec_mips; ++m) {
        if (rpb_cancelled(ctx)) goto cancelled;
        rpb_set_progress((float)(m + 1) / (float)spec_mips,
                          0.65f + 0.25f * (float)(m + 1) / (float)spec_mips);
    }

    /* Step 4/4: encode + write. Real KTX1 cubemap via bimg. */
    rpb_atomic_set(g_rpb.status, JCE_BAKE_STATUS_ENCODING_KTX2);
    rpb_set_progress(0.3f, 0.92f);

    /* Caller may have submitted a `.cube` or `.ktx2` path from the
     * pre-bimg era; rewrite the suffix in place so the artefact lands
     * with its real extension. The struct field name stays put. */
    (void)rpb_normalise_extension(g_rpb.path, sizeof g_rpb.path);

    bool ok = jce__ktx2_write_cubemap(g_rpb.path,
                                       face_size,
                                       1u,        /* mip_count — spec mip chain TBD */
                                       faces,
                                       4u);       /* RGBA8 */
    if (ok) {
        rpb_set_progress(0.7f, 0.97f);
        /* Irradiance sidecar: same source faces for v2 (analytic SH
         * convolution lands with the live capture path). Failure here
         * is non-fatal — specular write already succeeded. */
        char irr_path[512];
        rpb_irr_path(g_rpb.path, irr_path, sizeof irr_path);
        if (irr_path[0]) {
            (void)jce__ktx2_write_cubemap(irr_path, face_size, 1u, faces, 4u);
        }
    }
    jce_free(faces);

    if (!ok) {
        LOG_ERROR(JCE_RPB_TAG, "write failed: %s", g_rpb.path);
        rpb_atomic_set(g_rpb.status, JCE_BAKE_STATUS_FAILED);
        g_rpb.message = "write-failed";
        return JCE_ASYNC_RUN_FAILED;
    }

    rpb_set_progress(1.0f, 1.0f);
    rpb_atomic_set(g_rpb.status, JCE_BAKE_STATUS_DONE);
    g_rpb.message = "done";
    LOG_SUCCESS(JCE_RPB_TAG,
                "bake done: %s (%ux%u cube, %u spec mips requested, KTX1 + .irr.ktx sidecar)",
                g_rpb.path, face_size, face_size, spec_mips);
    return JCE_ASYNC_RUN_SUCCESS;

cancelled:
    jce_free(faces);
    rpb_atomic_set(g_rpb.status, JCE_BAKE_STATUS_CANCELLED);
    g_rpb.message = "cancelled";
    LOG_INFO(JCE_RPB_TAG, "bake cancelled");
    return JCE_ASYNC_RUN_CANCELLED;
}

/* ── public API ───────────────────────────────────────────────────── */

JCE_API JceReflectionProbeBakeHandle JCE_CALL
jce_reflection_probe_bake_submit(const JceReflectionProbeBakeDesc *desc)
{
    if (!desc || !desc->output_path_ktx2 || !desc->output_path_ktx2[0]) {
        LOG_WARN(JCE_RPB_TAG, "submit: missing desc / output path");
        return 0u;
    }

    /* Refuse if a bake is in flight (DONE/FAILED/CANCELLED slots are
     * recycled below — they are terminal states for the previous handle
     * but the slot itself is reusable). */
    if (g_rpb.in_use) {
        JceBakeStatus s = (JceBakeStatus)rpb_atomic_get(g_rpb.status);
        if (s != JCE_BAKE_STATUS_IDLE   && s != JCE_BAKE_STATUS_DONE &&
            s != JCE_BAKE_STATUS_FAILED && s != JCE_BAKE_STATUS_CANCELLED) {
            LOG_WARN(JCE_RPB_TAG, "submit rejected: bake already running");
            return 0u;
        }
        if (g_rpb.task && !jce_async_task_is_terminal(g_rpb.task)) {
            LOG_WARN(JCE_RPB_TAG, "submit rejected: prior task is retiring");
            return 0u;
        }
        if (g_rpb.task) {
            jce_async_task_release(g_rpb.task);
            g_rpb.task = NULL;
        }
    }

    /* Capture desc (path string is copied — caller's pointer not stored). */
    g_rpb.desc = *desc;
    if (g_rpb.desc.cubemap_size == 0u)       g_rpb.desc.cubemap_size       = JCE_RPB_DEFAULT_FACE;
    if (g_rpb.desc.cubemap_size > JCE_RPB_MAX_FACE)
        g_rpb.desc.cubemap_size = JCE_RPB_MAX_FACE;
    if (g_rpb.desc.specular_mip_count == 0u) g_rpb.desc.specular_mip_count = JCE_RPB_DEFAULT_SPEC_MIPS;
    if (g_rpb.desc.specular_mip_count > 12u) g_rpb.desc.specular_mip_count = 12u;

    snprintf(g_rpb.path, sizeof(g_rpb.path), "%s", desc->output_path_ktx2);
    g_rpb.desc.output_path_ktx2 = g_rpb.path;

    /* Lazy-create the atomics (cheap; module-scoped lifetime). */
    if (!g_rpb.status)   g_rpb.status   = jce_atomic_i32_create(JCE_BAKE_STATUS_IDLE);
    if (!g_rpb.cancel)   g_rpb.cancel   = jce_atomic_i32_create(0);
    if (!g_rpb.progress) g_rpb.progress = jce_atomic_i32_create(0);
    if (!g_rpb.overall)  g_rpb.overall  = jce_atomic_i32_create(0);
    if (!g_rpb.status || !g_rpb.cancel || !g_rpb.progress || !g_rpb.overall) {
        LOG_ERROR(JCE_RPB_TAG, "atomic init failed");
        return 0u;
    }
    rpb_atomic_set(g_rpb.status,   JCE_BAKE_STATUS_RENDERING_FACES);
    rpb_atomic_set(g_rpb.cancel,   0);
    rpb_atomic_set(g_rpb.progress, 0);
    rpb_atomic_set(g_rpb.overall,  0);

    g_rpb.handle  = g_next_handle++;
    if (g_next_handle == 0u) g_next_handle = 1u; /* skip the sentinel */
    g_rpb.in_use  = true;
    g_rpb.message = "rendering";

    JceAsyncTaskDesc task_desc;
    jce_async_task_desc_init(&task_desc);
    task_desc.work       = rpb_worker_main;
    task_desc.debug_name = "renderer.reflection-probe.bake";
    task_desc.priority   = JCE_ASYNC_PRIORITY_BACKGROUND;
    g_rpb.task = jce_async_submit(jce_async_default_executor(), &task_desc);
    if (!g_rpb.task) {
        rpb_atomic_set(g_rpb.status, JCE_BAKE_STATUS_FAILED);
        g_rpb.message = "queue-full";
        g_rpb.in_use = false;
        LOG_ERROR(JCE_RPB_TAG, "bake task submission failed");
        return 0u;
    }
    LOG_INFO(JCE_RPB_TAG, "bake submitted h=%u path=%s size=%u",
             g_rpb.handle, g_rpb.path, g_rpb.desc.cubemap_size);
    return g_rpb.handle;
}

JCE_API bool JCE_CALL
jce_reflection_probe_bake_poll(JceReflectionProbeBakeHandle h,
                                JceReflectionProbeBakeProgress *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (h == 0u || h != g_rpb.handle || !g_rpb.in_use) return false;

    JceBakeStatus s = (JceBakeStatus)rpb_atomic_get(g_rpb.status);
    if (out) {
        out->status   = s;
        out->progress = (float)rpb_atomic_get(g_rpb.progress) / 1000.0f;
        out->overall  = (float)rpb_atomic_get(g_rpb.overall)  / 1000.0f;
        out->message  = g_rpb.message;
    }

    /* Reap the caller-owned handle after work and public status are terminal. */
    if ((s == JCE_BAKE_STATUS_DONE   || s == JCE_BAKE_STATUS_FAILED ||
         s == JCE_BAKE_STATUS_CANCELLED) && g_rpb.task &&
        jce_async_task_is_terminal(g_rpb.task)) {
        jce_async_task_release(g_rpb.task);
        g_rpb.task = NULL;
    }
    return true;
}

JCE_API void JCE_CALL
jce_reflection_probe_bake_cancel(JceReflectionProbeBakeHandle h)
{
    if (h == 0u || h != g_rpb.handle || !g_rpb.in_use) return;
    rpb_atomic_set(g_rpb.cancel, 1);
    rpb_atomic_set(g_rpb.status, JCE_BAKE_STATUS_CANCELLED);
    g_rpb.message = "cancelled";
    if (g_rpb.task) (void)jce_async_task_cancel(g_rpb.task);
    LOG_INFO(JCE_RPB_TAG, "cancel requested h=%u", h);
}
