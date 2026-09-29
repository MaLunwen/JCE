/*
 * jce_reflect_builtin.cpp  Register engine component metadata.
 *
 * Scope note (deliberate, do not "restore" the removed entries):
 *   The data-driven jce_reflect drawer is NOT the inspector's general
 *   path.  Every component except two is drawn by a handwritten
 *   draw_comp_* function in editor/src/panels/jce_panel_inspector_*.cpp,
 *   and those handwritten drawers are the AUTHORITY for labels, ranges
 *   and reset values.
 *
 *   jce_reflect_draw is called from exactly two places:
 *     - draw_comp_camera()  (jce_panel_inspector_lighting.cpp) → "Camera",
 *       registered here;
 *     - the particle editor (jce_panel_particle_editor.cpp) →
 *       "Particle Emitter", registered there next to its own drawer.
 *
 *   Registrations for Transform / Pivot / Directional Light / Point Light
 *   used to live here too.  Nothing ever looked them up, so they were pure
 *   drift bait: the Point Light "Reset to Default" radius had already
 *   silently diverged (5.0) from the engine's JSON parse fallback and the
 *   editor's Add-Component initialiser (10.0) without anyone noticing,
 *   because the blob was unreachable.  They were removed rather than kept
 *   as a half-migration.  If reflect is ever extended to another component,
 *   register it HERE together with the jce_reflect_draw call that consumes
 *   it, and delete the handwritten drawer in the same change.
 */

#include "jce_reflect.h"

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
}

/* ── Type registrations ─────────────────────────────────────────── */

/* This blob is the inspector's per-field "Reset to Default" source, so it
 * MUST agree with the engine's JSON parse fallbacks (parse_camera in
 * engine/src/middleware/scene/jce_scene_components_json.c) and with the
 * editor's Add-Component initialisers (adddef_* in
 * jce_editor_component_defaults.cpp).  The engine parser is authoritative. */
static const JceCameraComponent g_def_JceCameraComponent = {
    /* fov_deg    */ 60.0f,
    /* near_plane */ 0.1f,
    /* far_plane  */ 1000.0f,
    /* is_primary */ false,
    /* ortho      */ false,
    /* The three trailing members -- stack_index, clear_mode, culling_mask --
     * are left to zero-init deliberately, and that is what parse_camera falls
     * back to for all three.  Naming one of them here would mean naming the
     * two before it: this blob is POSITIONAL, and a mid-list entry silently
     * re-associates every per-field comment below it. */
};
JCE_REFLECT_BEGIN(JceCameraComponent, "Camera")
    JCE_FIELD_RANGE(JceCameraComponent, fov_deg,    JCE_FT_FLOAT, "FOV (deg)",     1.0f, 179.0f, 0.5f)
    JCE_FIELD_RANGE(JceCameraComponent, near_plane, JCE_FT_FLOAT, "Near",          0.001f, 1000.0f, 0.01f)
    JCE_FIELD_RANGE(JceCameraComponent, far_plane,  JCE_FT_FLOAT, "Far",           1.0f, 100000.0f, 1.0f)
    JCE_FIELD(JceCameraComponent, is_primary, JCE_FT_BOOL,  "Primary")
    JCE_FIELD(JceCameraComponent, ortho,      JCE_FT_BOOL,  "Orthographic")
JCE_REFLECT_END_DEFAULTS(JceCameraComponent, "Camera", &g_def_JceCameraComponent)

extern "C" void jce_reflect_register_builtin(void)
{
    jce_reflect_register(&g_jce_type_JceCameraComponent);
}
