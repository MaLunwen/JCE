#include <stdlib.h>
/*
 * es_camera.c — damped orbit camera around the diorama origin.
 *
 * Reference behavior: orbit with left-drag, zoom with wheel, no pan.
 * Polar clamp [45 deg, ~81.8 deg], distance clamp [8, 35], smooth damping.
 * The pose lands in the "BeautyCam" VirtualCamera component; the engine's
 * vcam system resolves it (and adds lightning trauma shake on top).
 */
#include "es_camera.h"

#include <jce/api.h>
#include <math.h>
#include <string.h>

#define ES_CAM_POLAR_MIN   (45.0f  * 0.01745329f)
#define ES_CAM_POLAR_MAX   (81.8f  * 0.01745329f)
#define ES_CAM_DIST_MIN    8.0f
#define ES_CAM_DIST_MAX    35.0f
#define ES_CAM_DAMP        10.0f     /* target chase rate (1/s)          */
#define ES_CAM_ORBIT_SPEED 0.006f    /* radians per pixel dragged        */
#define ES_CAM_ZOOM_SPEED  1.8f      /* units per wheel notch            */

typedef struct {
    const JceServices *svc;
    JceEntity ent_cam;
    /* spherical state (target + damped current) */
    float az_t, pol_t, dist_t;
    float az,   pol,   dist;
} EsCamera;

static EsCamera g_cam;

static float es_cam_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void es_camera_init(const JceServices *svc, JceScene *scene)
{
    memset(&g_cam, 0, sizeof g_cam);
    g_cam.svc = svc;
    if (scene) {
        JceEntity out[1] = {0};
        if (jce_scene_query_by_name(scene, "BeautyCam", out, 1) > 0)
            g_cam.ent_cam = out[0];
    }
    /* spherical of the authored beauty pose (18.25, 10.69, 27.32) */
    float x = 18.25f, y = 10.69f, z = 27.32f;
    g_cam.dist = sqrtf(x * x + y * y + z * z);
    g_cam.pol  = acosf(y / g_cam.dist);
    g_cam.az   = atan2f(x, z);
    /* Diagnostic: JCE_CAM_DIST forces the initial orbit distance so the
     * "zoom → black" report can be reproduced/captured at a fixed distance. */
    {
        const char *cd = getenv("JCE_CAM_DIST");
        if (cd && cd[0]) {
            float d = (float)atof(cd);
            if (d > 0.0f) g_cam.dist = d;
        }
    }
    /* Diagnostic: JCE_CAM_POLAR forces the initial polar angle in DEGREES
     * (0 = straight down, 90 = horizon) so view-angle-dependent shadow
     * artifacts (cascade-split knife lines) can be swept headless. */
    {
        const char *cp = getenv("JCE_CAM_POLAR");
        if (cp && cp[0]) {
            float p = (float)atof(cp);
            if (p > 1.0f && p < 89.0f)
                g_cam.pol = p * 0.0174533f;
        }
    }
    g_cam.az_t = g_cam.az; g_cam.pol_t = g_cam.pol; g_cam.dist_t = g_cam.dist;
}

void es_camera_update(JceScene *scene, float dt)
{
    if (!scene || !g_cam.ent_cam || !g_cam.svc || !g_cam.svc->input) return;
    const JceInput *in = g_cam.svc->input;

    /* input -> targets (left-drag orbit, wheel zoom).  NOTE: SDL mouse
     * buttons are 1-BASED — button 0 masks to nothing and never fires. */
    if (jce_input_mouse_button(in, JCE_MOUSE_BUTTON_LEFT)) {
        float dx = 0.0f, dy = 0.0f;
        jce_input_mouse_delta(in, &dx, &dy);
        g_cam.az_t  -= dx * ES_CAM_ORBIT_SPEED;
        g_cam.pol_t -= dy * ES_CAM_ORBIT_SPEED;
    }
    float wheel = jce_input_mouse_wheel(in);
    if (wheel != 0.0f)
        g_cam.dist_t -= wheel * ES_CAM_ZOOM_SPEED;

    g_cam.pol_t  = es_cam_clampf(g_cam.pol_t,  ES_CAM_POLAR_MIN, ES_CAM_POLAR_MAX);
    g_cam.dist_t = es_cam_clampf(g_cam.dist_t, ES_CAM_DIST_MIN,  ES_CAM_DIST_MAX);

    /* damped chase */
    float k = 1.0f - expf(-ES_CAM_DAMP * dt);
    g_cam.az   += (g_cam.az_t   - g_cam.az)   * k;
    g_cam.pol  += (g_cam.pol_t  - g_cam.pol)  * k;
    g_cam.dist += (g_cam.dist_t - g_cam.dist) * k;

    /* spherical -> cartesian, write the vcam pose */
    JceVirtualCameraComponent *vc =
        jce_scene_get_virtual_camera(scene, g_cam.ent_cam);
    if (!vc) return;
    float sp = sinf(g_cam.pol), cp = cosf(g_cam.pol);
    vc->position[0] = g_cam.dist * sp * sinf(g_cam.az);
    vc->position[1] = g_cam.dist * cp;
    vc->position[2] = g_cam.dist * sp * cosf(g_cam.az);
    vc->look_at[0] = 0.0f; vc->look_at[1] = 0.0f; vc->look_at[2] = 0.0f;
}
