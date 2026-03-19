/*
 * jce_views.h  bgfx view ID assignments.
 *
 * bgfx renders views in ID order. Lower IDs render first.
 * This header defines the canonical view layout for JCE.
 */

#ifndef JCE_VIEWS_H
#define JCE_VIEWS_H

/* -- View IDs ------------------------------------------------------- */

/* Main 3D scene (perspective camera, depth test, lit meshes). */
#define JCE_VIEW_MAIN_3D    0

/* 2D overlay (orthographic, no depth test  sprites, text, UI). */
#define JCE_VIEW_UI          1

/* Debug overlay (bgfx debug text, profiler). */
#define JCE_VIEW_DEBUG       2

/* Reserved range for shadow maps (Phase 4). */
#define JCE_VIEW_SHADOW_BASE 10

/* Reserved range for post-processing (Phase 4). */
#define JCE_VIEW_POST_BASE   20

/* ImGui editor overlay (always last  renders on top). */
#define JCE_VIEW_IMGUI       255

#endif /* JCE_VIEWS_H */
