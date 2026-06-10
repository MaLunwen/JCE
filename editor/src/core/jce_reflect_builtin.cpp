/*
 * jce_reflect_builtin.cpp  Register engine component metadata.
 *
 * This is a thin layer that exposes JceTransform, JceCameraComponent,
 * JceDirectionalLight, JcePointLight, JceSpotLight to the reflection
 * registry so the inspector can draw them generically.
 */

#include "jce_reflect.h"

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
}

/* ── Type registrations ─────────────────────────────────────────── */

static const JceTransform g_def_JceTransform = {
    /* position */ {0.0f, 0.0f, 0.0f},
    /* rotation */ {0.0f, 0.0f, 0.0f, 1.0f},
    /* scale    */ {1.0f, 1.0f, 1.0f},
};
JCE_REFLECT_BEGIN(JceTransform, "Transform")
    JCE_FIELD(JceTransform, position, JCE_FT_VEC3, "Position")
    JCE_FIELD(JceTransform, rotation, JCE_FT_QUAT, "Rotation")
    JCE_FIELD(JceTransform, scale,    JCE_FT_VEC3, "Scale")
JCE_REFLECT_END_DEFAULTS(JceTransform, "Transform", &g_def_JceTransform)

static const JceCameraComponent g_def_JceCameraComponent = {
    /* fov_deg    */ 60.0f,
    /* near_plane */ 0.1f,
    /* far_plane  */ 1000.0f,
    /* is_primary */ false,
    /* ortho      */ false,
};
JCE_REFLECT_BEGIN(JceCameraComponent, "Camera")
    JCE_FIELD_RANGE(JceCameraComponent, fov_deg,    JCE_FT_FLOAT, "FOV (deg)",     1.0f, 179.0f, 0.5f)
    JCE_FIELD_RANGE(JceCameraComponent, near_plane, JCE_FT_FLOAT, "Near",          0.001f, 1000.0f, 0.01f)
    JCE_FIELD_RANGE(JceCameraComponent, far_plane,  JCE_FT_FLOAT, "Far",           1.0f, 100000.0f, 1.0f)
    JCE_FIELD(JceCameraComponent, is_primary, JCE_FT_BOOL,  "Primary")
    JCE_FIELD(JceCameraComponent, ortho,      JCE_FT_BOOL,  "Orthographic")
JCE_REFLECT_END_DEFAULTS(JceCameraComponent, "Camera", &g_def_JceCameraComponent)

static const JceDirectionalLight g_def_JceDirectionalLight = {
    /* direction    */ {0.0f, -1.0f, 0.0f},
    /* color        */ {1.0f, 1.0f, 1.0f},
    /* intensity    */ 1.0f,
    /* casts_shadow */ true,
    /* cookie       */ {UINT16_MAX},
    /* cookie str   */ 0.0f,
    /* cookie path  */ "",
};
JCE_REFLECT_BEGIN(JceDirectionalLight, "Directional Light")
    JCE_FIELD(JceDirectionalLight, direction,    JCE_FT_VEC3,    "Direction")
    JCE_FIELD(JceDirectionalLight, color,        JCE_FT_COLOR3,  "Color")
    JCE_FIELD_RANGE(JceDirectionalLight, intensity, JCE_FT_FLOAT, "Intensity", 0.0f, 100.0f, 0.05f)
    JCE_FIELD(JceDirectionalLight, casts_shadow, JCE_FT_BOOL,    "Casts Shadow")
JCE_REFLECT_END_DEFAULTS(JceDirectionalLight, "Directional Light", &g_def_JceDirectionalLight)

static const JcePointLight g_def_JcePointLight = {
    /* position  */ {0.0f, 0.0f, 0.0f},
    /* color     */ {1.0f, 1.0f, 1.0f},
    /* intensity */ 1.0f,
    /* radius    */ 5.0f,
};
JCE_REFLECT_BEGIN(JcePointLight, "Point Light")
    JCE_FIELD(JcePointLight, position,  JCE_FT_VEC3,   "Position")
    JCE_FIELD(JcePointLight, color,     JCE_FT_COLOR3, "Color")
    JCE_FIELD_RANGE(JcePointLight, intensity, JCE_FT_FLOAT, "Intensity", 0.0f, 1000.0f, 0.1f)
    JCE_FIELD_RANGE(JcePointLight, radius,    JCE_FT_FLOAT, "Radius",    0.01f, 1000.0f, 0.1f)
JCE_REFLECT_END_DEFAULTS(JcePointLight, "Point Light", &g_def_JcePointLight)

/* JceSpotLight has inner/outer cone — register conservatively. */

extern "C" void jce_reflect_register_builtin(void)
{
    jce_reflect_register(&g_jce_type_JceTransform);
    jce_reflect_register(&g_jce_type_JceCameraComponent);
    jce_reflect_register(&g_jce_type_JceDirectionalLight);
    jce_reflect_register(&g_jce_type_JcePointLight);
}
