/*
 * jce_gizmo_internal.h  Shared declarations for gizmo subsystem files.
 *
 * Functions below are defined in jce_gizmo.cpp and used by
 * jce_gizmo_draw.cpp and jce_gizmo_interact.cpp.
 */

#ifndef JCE_GIZMO_INTERNAL_H
#define JCE_GIZMO_INTERNAL_H

#include "jce_gizmo.h"

#ifdef __cplusplus
extern "C" {
#endif

JceGizmoAxis jce_gizmo_internal_hovered(void);
bool         jce_gizmo_internal_dragging(void);
JceGizmoAxis jce_gizmo_internal_drag_axis(void);
void         jce_gizmo_internal_get_axes(float ax_x[3], float ax_y[3], float ax_z[3]);

#ifdef __cplusplus
}
#endif

#endif /* JCE_GIZMO_INTERNAL_H */
