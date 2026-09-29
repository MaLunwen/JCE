/*
 * jce_scene_probe_capture.c  See the header for why this exists and why it is
 *                            modal.
 *
 * One face per frame through the ordinary scene renderer and the ordinary
 * offscreen bridge, read back with the blit + bgfx_read_texture pattern the
 * impostor bake proved (jce_impostor.c): the screenshot callback does not
 * deliver for offscreen FBOs, and a blit to a READ_BACK staging texture does.
 */

#include <jce/middleware/scene/jce_scene_probe_capture.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_views.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>

#include <stdio.h>
#include <string.h>

#define LOG_TAG "probe-capture"

typedef enum { PHASE_RENDER = 0, PHASE_READBACK } CapturePhase;

static struct {
    JceProbeCaptureStatus status;
    CapturePhase          phase;
    int                   face;          /* 0..5 */
    uint32_t              face_size;
    uint8_t              *faces;         /* 6 * face_size^2 * 4 */
    uint8_t              *staging_px;    /* one face, RGBA8 or RGBA16F */
    /* 4 or 8.  The probe component's `hdr` flag decides, and the offscreen
     * target has to be able to give it: an adapter without RGBA16F render
     * targets falls back to RGBA8 there, and capturing 8 bytes out of a
     * 4-byte source would read whatever is past the end. */
    uint32_t              bpp;
    bgfx_texture_handle_t staging;
    JceOffscreenTarget   *target;
    uint32_t              ready_frame;
    jce_vec3              position;
    float                 near_z, far_z;
    char                  path[512];
} g_cap = {
    /* DESIGNATED, not positional.  The positional form silently shifted every
     * value one slot right the first time a field was inserted in the middle
     * of this struct -- and the compiler's only complaint was "too many
     * initializers", pointing at the last line rather than the insertion. */
    .status      = JCE_PROBE_CAPTURE_IDLE,
    .phase       = PHASE_RENDER,
    .staging     = { UINT16_MAX },
    .bpp         = 4u,
    .near_z      = 0.1f,
    .far_z       = 1000.0f,
};

/* ── The cube convention ──────────────────────────────────────────────
 *
 * Derived from the (face, u, v) -> direction mapping the samplers use, term by
 * term: image x is u and image y is v, and v runs DOWN the face, so the camera
 * "up" is the direction of DECREASING v.
 *
 *   +X : +u -> -Z, +v -> -Y   =>  right -Z, up +Y
 *   -X : +u -> +Z, +v -> -Y   =>  right +Z, up +Y
 *   +Y : +u -> +X, +v -> +Z   =>  right +X, up -Z
 *   -Y : +u -> +X, +v -> -Z   =>  right +X, up +Z
 *   +Z : +u -> +X, +v -> -Y   =>  right +X, up +Y
 *   -Z : +u -> -X, +v -> -Y   =>  right -X, up +Y
 *
 * test_jce_probe_face_basis asserts this against that mapping rather than
 * against a second copy of the table, because a cubemap convention that is
 * mirrored or rotated renders a plausible image whose reflections point the
 * wrong way and nothing reports it. */
bool jce_probe_face_basis(int face, jce_vec3 *out_forward, jce_vec3 *out_up)
{
    if (face < 0 || face > 5 || !out_forward || !out_up) return false;

    static const jce_vec3 kFwd[6] = {
        {  1.0f,  0.0f,  0.0f }, { -1.0f,  0.0f,  0.0f },   /* +X, -X */
        {  0.0f,  1.0f,  0.0f }, {  0.0f, -1.0f,  0.0f },   /* +Y, -Y */
        {  0.0f,  0.0f,  1.0f }, {  0.0f,  0.0f, -1.0f },   /* +Z, -Z */
    };
    static const jce_vec3 kUp[6] = {
        {  0.0f,  1.0f,  0.0f }, {  0.0f,  1.0f,  0.0f },   /* +X, -X */
        {  0.0f,  0.0f, -1.0f }, {  0.0f,  0.0f,  1.0f },   /* +Y, -Y */
        {  0.0f,  1.0f,  0.0f }, {  0.0f,  1.0f,  0.0f },   /* +Z, -Z */
    };
    *out_forward = kFwd[face];
    *out_up      = kUp[face];
    return true;
}

/* ── Lifetime ─────────────────────────────────────────────────────── */

static void capture_release(void)
{
    if (g_cap.target) {
        jce_offscreen_target_destroy(g_cap.target);
        g_cap.target = NULL;
    }
    /* NEVER trust a zero-initialised bgfx handle: idx 0 is a VALID handle,
     * i.e. someone else's texture.  The impostor bake destroyed frame buffers
     * it never created exactly that way. */
    if (g_cap.staging.idx != UINT16_MAX) {
        bgfx_destroy_texture(g_cap.staging);
        g_cap.staging.idx = UINT16_MAX;
    }
    if (g_cap.staging_px) { JCE_FREE(g_cap.staging_px); g_cap.staging_px = NULL; }
    if (g_cap.faces)      { JCE_FREE(g_cap.faces);      g_cap.faces = NULL; }
}

bool jce_scene_probe_capture_in_flight(void)
{
    return g_cap.status == JCE_PROBE_CAPTURE_RENDERING;
}

void jce_scene_probe_capture_cancel(void)
{
    if (g_cap.status == JCE_PROBE_CAPTURE_RENDERING)
        LOG_INFO(LOG_TAG, "capture cancelled at face %d/6", g_cap.face);
    capture_release();
    g_cap.status = JCE_PROBE_CAPTURE_IDLE;
}

bool jce_scene_probe_capture_begin(JceScene *scene, JceEntity probe,
                                   uint32_t face_size,
                                   const char *out_path_ktx)
{
    if (g_cap.status == JCE_PROBE_CAPTURE_RENDERING) {
        LOG_WARN(LOG_TAG, "begin rejected: a capture is already running");
        return false;
    }
    if (!scene || !out_path_ktx || !out_path_ktx[0]) return false;
    if (face_size < 16u || face_size > 512u) {
        LOG_WARN(LOG_TAG, "begin rejected: face size %u out of range", face_size);
        return false;
    }

    const JceReflectionProbeComponent *rp = jce_scene_get_reflection_probe(scene, probe);
    if (!rp) {
        LOG_WARN(LOG_TAG, "begin rejected: entity has no reflection probe");
        return false;
    }

    capture_release();

    /* HDR when the probe asks for it AND the target can carry it.  Asking
     * without checking would read past a 4-byte-per-pixel source. */
    g_cap.bpp = rp->hdr ? 8u : 4u;

    const size_t face_bytes = (size_t)face_size * face_size * g_cap.bpp;
    g_cap.faces      = (uint8_t *)JCE_MALLOC(face_bytes * 6u);
    g_cap.staging_px = (uint8_t *)JCE_MALLOC(face_bytes);
    if (!g_cap.faces || !g_cap.staging_px) {
        LOG_ERROR(LOG_TAG, "begin failed: out of memory (%u px faces)", face_size);
        capture_release();
        return false;
    }

    /* The probe's WORLD position: a probe parented to a moving object must
     * capture from where it actually is, not from its local offset. */
    const jce_mat4 w = jce_scene_get_world_matrix(scene, probe);
    const jce_vec3 pos = jce_v3(w.raw[3][0], w.raw[3][1], w.raw[3][2]);

    g_cap.position  = pos;
    g_cap.near_z    = rp->near_clip > 0.0f ? rp->near_clip : 0.1f;
    g_cap.far_z     = rp->far_clip  > g_cap.near_z ? rp->far_clip : 1000.0f;
    g_cap.face_size = face_size;
    g_cap.face      = 0;
    g_cap.phase     = PHASE_RENDER;
    g_cap.status    = JCE_PROBE_CAPTURE_RENDERING;
    snprintf(g_cap.path, sizeof g_cap.path, "%s", out_path_ktx);

    LOG_INFO(LOG_TAG, "capture started: %s (%ux%u cube at %.2f %.2f %.2f)",
             g_cap.path, face_size, face_size,
             (double)pos.x, (double)pos.y, (double)pos.z);
    return true;
}

/* ── One face ─────────────────────────────────────────────────────── */

static bool capture_render_face(JceSceneRenderer *sr, JceScene *scene,
                                JceRenderer *renderer)
{
    if (!renderer) return false;

    if (!g_cap.target) {
        g_cap.target = jce_offscreen_target_create(renderer, JCE_VIEW_EDITOR_SCENE);
        if (!g_cap.target) {
            LOG_ERROR(LOG_TAG, "offscreen target creation failed");
            return false;
        }
    }
    if (g_cap.staging.idx == UINT16_MAX) {
        /* The staging format must MATCH the offscreen target's, because a
         * bgfx blit between different formats is undefined -- and matching it
         * is the whole fix: the scene was already being rendered in RGBA16F
         * and the HDR was discarded here, one call before it would have been
         * stored. */
        if (g_cap.bpp == 8u && !jce_offscreen_target_is_hdr(g_cap.target)) {
            LOG_WARN(LOG_TAG,
                     "probe asked for HDR but the offscreen target is RGBA8 on "
                     "this adapter; capturing LDR");
            g_cap.bpp = 4u;
        }
        g_cap.staging = bgfx_create_texture_2d(
            (uint16_t)g_cap.face_size, (uint16_t)g_cap.face_size, false, 1,
            g_cap.bpp == 8u ? BGFX_TEXTURE_FORMAT_RGBA16F
                            : BGFX_TEXTURE_FORMAT_RGBA8,
            BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK
                | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
            NULL, 0);
        if (!BGFX_HANDLE_IS_VALID(g_cap.staging)) {
            LOG_ERROR(LOG_TAG, "read-back staging texture unavailable");
            return false;
        }
    }

    jce_vec3 fwd, up;
    if (!jce_probe_face_basis(g_cap.face, &fwd, &up)) return false;

    JceCameraDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.mode       = JCE_CAMERA_PERSPECTIVE;
    cd.position   = g_cap.position;
    cd.target     = jce_v3(g_cap.position.x + fwd.x,
                           g_cap.position.y + fwd.y,
                           g_cap.position.z + fwd.z);
    cd.up         = up;
    cd.fov_deg    = 90.0f;          /* a cube face is exactly 90 degrees */
    cd.near_plane = g_cap.near_z;
    cd.far_plane  = g_cap.far_z;
    JceCamera *cam = jce_camera_create(&cd);
    if (!cam) return false;
    /* set_pose preserves the exact basis; look-at alone would re-derive `up`
     * and silently roll the +Y and -Y faces, which are the two whose up is
     * not the world up. */
    (void)jce_camera_set_pose(cam, g_cap.position, fwd, up);

    const jce_mat4 view = jce_camera_view(cam);
    const jce_mat4 proj = jce_camera_proj(cam, 1.0f,
                                          bgfx_get_caps()->homogeneousDepth);

    if (!jce_offscreen_target_prepare(g_cap.target, g_cap.face_size,
                                      g_cap.face_size, view.raw[0], proj.raw[0],
                                      0x000000ff, "ProbeCaptureFace")) {
        jce_camera_destroy(cam);
        return false;
    }

    JceSceneRenderConfig cfg = jce_scene_render_config_default();
    cfg.draw_skybox      = true;
    cfg.draw_shadows     = true;
    cfg.draw_opaque      = true;
    cfg.draw_sprites     = false;
    cfg.draw_transparent = true;
    cfg.apply_postfx     = false;   /* a probe stores radiance, not a graded frame */
    cfg.viewport_width   = g_cap.face_size;
    cfg.viewport_height  = g_cap.face_size;
    cfg.viewport_id      = 0;
    cfg.scene_frame_buffer = jce_offscreen_target_get_frame_buffer(g_cap.target);

    (void)jce_scene_renderer_render(sr, scene, cam, JCE_VIEW_EDITOR_SCENE,
                                    0.0f, &cfg);
    jce_camera_destroy(cam);

    const uint16_t color_idx = jce_offscreen_target_get_color_texture(g_cap.target);
    if (color_idx == UINT16_MAX) return false;
    bgfx_texture_handle_t color = { color_idx };

    bgfx_blit((uint16_t)JCE_VIEW_PROBE_CAPTURE_BLIT, g_cap.staging, 0, 0, 0, 0,
              color, 0, 0, 0, 0,
              (uint16_t)g_cap.face_size, (uint16_t)g_cap.face_size, 1);
    g_cap.ready_frame = bgfx_read_texture(g_cap.staging, g_cap.staging_px, 0, 0);
    return true;
}

JceProbeCaptureStatus jce_scene_probe_capture_poll(JceSceneRenderer *sr,
                                                   JceScene *scene,
                                                   JceRenderer *renderer)
{
    if (g_cap.status != JCE_PROBE_CAPTURE_RENDERING) return g_cap.status;
    if (!sr || !scene || !renderer) {
        LOG_ERROR(LOG_TAG, "poll without a renderer or scene");
        capture_release();
        g_cap.status = JCE_PROBE_CAPTURE_FAILED;
        return g_cap.status;
    }

    if (g_cap.phase == PHASE_RENDER) {
        if (!capture_render_face(sr, scene, renderer)) {
            capture_release();
            g_cap.status = JCE_PROBE_CAPTURE_FAILED;
            return g_cap.status;
        }
        g_cap.phase = PHASE_READBACK;
        return g_cap.status;
    }

    /* PHASE_READBACK: bgfx promised the pixels are valid once the frame index
     * reaches ready_frame. */
    if (jce_renderer_get_frame_index(renderer) < g_cap.ready_frame)
        return g_cap.status;

    {
        const size_t face_bytes =
        (size_t)g_cap.face_size * g_cap.face_size * g_cap.bpp;
        memcpy(g_cap.faces + (size_t)g_cap.face * face_bytes,
               g_cap.staging_px, face_bytes);
    }
    g_cap.face++;

    if (g_cap.face < 6) {
        g_cap.phase = PHASE_RENDER;
        return g_cap.status;
    }

    /* Six faces captured: hand them to the bake, which convolves and writes.
     * The bake owns its own single-slot concurrency, so a refusal here is a
     * real failure and not something to retry. */
    {
        JceReflectionProbeBakeDesc desc;
        memset(&desc, 0, sizeof desc);
        desc.position     = g_cap.position;
        desc.cubemap_size = g_cap.face_size;
        desc.specular_mip_count = 1u;   /* the bake derives the real chain */
        desc.include_skybox = true;
        desc.include_dynamic_objects = true;
        desc.output_path_ktx2 = g_cap.path;
        /* The format the capture ACTUALLY produced, not the one the component
         * asked for: an adapter without RGBA16F render targets downgrades
         * g_cap.bpp above, and telling the bake `hdr` after that would have it
         * read 8 bytes out of a 4-byte-per-texel buffer. */
        desc.hdr = (g_cap.bpp == 8u);

        const bool ok = jce_reflection_probe_bake_submit_faces(
                            &desc, g_cap.faces) != 0u;
        capture_release();
        g_cap.status = ok ? JCE_PROBE_CAPTURE_DONE : JCE_PROBE_CAPTURE_FAILED;
        if (ok)
            LOG_SUCCESS(LOG_TAG, "6 faces captured from the scene -> %s",
                        g_cap.path);
        else
            LOG_ERROR(LOG_TAG, "bake refused the captured faces: %s", g_cap.path);
    }
    return g_cap.status;
}
