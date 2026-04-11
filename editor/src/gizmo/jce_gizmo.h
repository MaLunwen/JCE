/*
 * jce_gizmo.h  Public API for the Maya-style 3D transform gizmo.
 *
 * Provides Translate / Rotate / Scale manipulators rendered via ImDrawList
 * with mouse hit-testing and drag interaction.  C linkage for engine compat.
 */

#ifndef JCE_GIZMO_H
#define JCE_GIZMO_H

#include "jce_gizmo_math.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Gizmo Axis flags ──────────────────────────────────────────────── */

typedef enum {
    JCE_GIZMO_AXIS_NONE = 0,
    JCE_GIZMO_AXIS_X    = (1 << 0),
    JCE_GIZMO_AXIS_Y    = (1 << 1),
    JCE_GIZMO_AXIS_Z    = (1 << 2),
    JCE_GIZMO_AXIS_XY   = JCE_GIZMO_AXIS_X | JCE_GIZMO_AXIS_Y,
    JCE_GIZMO_AXIS_XZ   = JCE_GIZMO_AXIS_X | JCE_GIZMO_AXIS_Z,
    JCE_GIZMO_AXIS_YZ   = JCE_GIZMO_AXIS_Y | JCE_GIZMO_AXIS_Z,
    JCE_GIZMO_AXIS_XYZ  = JCE_GIZMO_AXIS_X | JCE_GIZMO_AXIS_Y | JCE_GIZMO_AXIS_Z,
    JCE_GIZMO_AXIS_VIEW = (1 << 3),
} JceGizmoAxis;

/* ── Lifecycle ─────────────────────────────────────────────────────── */

void jce_gizmo_init(void);
void jce_gizmo_shutdown(void);

/* ── Per-frame entry points (called from scene view panel) ─────────── */

/*
 * jce_gizmo_update — run hit-testing and drag logic.
 *
 * gizmo_mode: 0=Translate, 1=Rotate, 2=Scale  (maps to JceGizmoMode)
 * gizmo_space: 0=Local, 1=World
 * scale_factor: preference-based gizmo display scale (default 1.0)
 *
 * inout_position / inout_rotation / inout_scale are modified in-place
 * when dragging.
 *
 * Returns true if the gizmo consumed mouse input this frame.
 */
bool jce_gizmo_update(const JceGizmoCamera *cam,
                       int gizmo_mode,
                       int gizmo_space,
                       float scale_factor,
                       float *inout_position,
                       float *inout_rotation,
                       float *inout_scale);

/*
 * jce_gizmo_draw — render gizmo overlays onto an ImDrawList.
 */
struct ImDrawList; /* forward decl */
void jce_gizmo_draw(struct ImDrawList *dl,
                     const JceGizmoCamera *cam,
                     int gizmo_mode,
                     int gizmo_space,
                     float scale_factor,
                     const float *position,
                     const float *rotation,
                     const float *scale);

/* ── Query ─────────────────────────────────────────────────────────── */

bool         jce_gizmo_is_active(void);
JceGizmoAxis jce_gizmo_hovered_axis(void);
void         jce_gizmo_cancel_interaction(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_GIZMO_H */
