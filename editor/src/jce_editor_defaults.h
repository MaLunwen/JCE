/*
 * jce_editor_defaults.h  Centralized default values for JCE Editor.
 *
 * Ported from EditorDefaults.java — no magic numbers in panel code.
 */

#ifndef JCE_EDITOR_DEFAULTS_H
#define JCE_EDITOR_DEFAULTS_H

/* ── Layout (simulated docking ratios) ─────────────────────────────── */

#define JCE_LAYOUT_LEFT_RATIO    0.15f
#define JCE_LAYOUT_RIGHT_RATIO   0.25f
#define JCE_LAYOUT_BOTTOM_RATIO  0.25f

/* ── Viewport ──────────────────────────────────────────────────────── */

#define JCE_VIEWPORT_WIDTH          1280
#define JCE_VIEWPORT_HEIGHT         720
#define JCE_VIEWPORT_MIN_WIDTH      320
#define JCE_VIEWPORT_MIN_HEIGHT     240

/* ── Window ────────────────────────────────────────────────────────── */

#define JCE_WINDOW_WIDTH            1920
#define JCE_WINDOW_HEIGHT           1080
#define JCE_WINDOW_MIN_WIDTH        800
#define JCE_WINDOW_MIN_HEIGHT       600
#define JCE_PANEL_INITIAL_WIDTH     300
#define JCE_PANEL_INITIAL_HEIGHT    200

/* ── Camera ────────────────────────────────────────────────────────── */

#define JCE_CAMERA_FOV              60.0f
#define JCE_CAMERA_NEAR_CLIP        0.1f
#define JCE_CAMERA_FAR_CLIP         1000.0f
#define JCE_CAMERA_ORTHO_SIZE       5.0f
#define JCE_CAMERA_MOVE_SPEED       10.0f
#define JCE_CAMERA_ROTATION_SPEED   0.3f
#define JCE_CAMERA_ZOOM_SPEED       1.0f

/* Camera orbit defaults (SceneViewWindow reference). */
#define JCE_CAMERA_DEFAULT_YAW      45.0f
#define JCE_CAMERA_DEFAULT_PITCH    35.0f
#define JCE_CAMERA_DEFAULT_DISTANCE 15.0f

/* ── Grid ──────────────────────────────────────────────────────────── */

#define JCE_GRID_SIZE               1.0f
#define JCE_GRID_EXTENT             100
#define JCE_GRID_MAJOR_INTERVAL     10
#define JCE_GRID_SNAP_ENABLED       false
#define JCE_GRID_SNAP_INCREMENT     0.25f

/* ── Gizmo ─────────────────────────────────────────────────────────── */

#define JCE_GIZMO_AXIS_LENGTH       1.5f
#define JCE_GIZMO_LINE_THICKNESS    3.0f
#define JCE_GIZMO_HANDLE_SIZE       0.15f
#define JCE_GIZMO_SELECT_THRESHOLD  10.0f

/* ── Timing ────────────────────────────────────────────────────────── */

#define JCE_TARGET_FPS              60
#define JCE_AUTO_SAVE_INTERVAL      300  /* seconds */
#define JCE_UNDO_HISTORY_LIMIT      100
#define JCE_DOUBLE_CLICK_MS         300

/* ── Asset Browser ─────────────────────────────────────────────────── */

#define JCE_THUMBNAIL_SIZE          64
#define JCE_THUMBNAIL_SIZE_LARGE    128
#define JCE_THUMBNAIL_CACHE_SIZE    500
#define JCE_ASSET_TREE_WIDTH_RATIO  0.25f
#define JCE_ASSET_CELL_PADDING      8.0f

/* ── Console ───────────────────────────────────────────────────────── */

#define JCE_CONSOLE_MAX_ENTRIES     1000
#define JCE_CONSOLE_AUTO_SCROLL     true

/* ── Timeline ──────────────────────────────────────────────────────── */

#define JCE_TIMELINE_ZOOM           1.0f
#define JCE_TIMELINE_ZOOM_MIN       0.1f
#define JCE_TIMELINE_ZOOM_MAX       10.0f
#define JCE_ANIMATION_FPS           30
#define JCE_TIMELINE_TOTAL_FRAMES   600  /* 10 sec at 60fps */
#define JCE_TIMELINE_PX_PER_FRAME   10.0f

/* ── Input ─────────────────────────────────────────────────────────── */

#define JCE_KEY_STATE_ARRAY_SIZE    512
#define JCE_MOUSE_BUTTON_COUNT      8

/* ── Math ──────────────────────────────────────────────────────────── */

#define JCE_HALF_ROTATION_DEG       180.0f
#define JCE_FULL_ROTATION_DEG       360.0f

/* ── Preferences Limits ────────────────────────────────────────────── */

#define JCE_PREF_FPS_MIN            15
#define JCE_PREF_FPS_MAX            240
#define JCE_PREF_GIZMO_SCALE_MIN   0.5f
#define JCE_PREF_GIZMO_SCALE_MAX   3.0f
#define JCE_PREF_CAM_SENS_MIN      0.1f
#define JCE_PREF_CAM_SENS_MAX      5.0f
#define JCE_PREF_PHYSICS_SUBSTEP_MIN 1
#define JCE_PREF_PHYSICS_SUBSTEP_MAX 16

/* ── MSAA Presets ──────────────────────────────────────────────────── */

#define JCE_MSAA_OFF    0
#define JCE_MSAA_2X     2
#define JCE_MSAA_4X     4
#define JCE_MSAA_8X     8
#define JCE_MSAA_16X    16

/* ── Shadow Map Sizes ──────────────────────────────────────────────── */

#define JCE_SHADOW_512   512
#define JCE_SHADOW_1024  1024
#define JCE_SHADOW_2048  2048
#define JCE_SHADOW_4096  4096
#define JCE_SHADOW_8192  8192

#endif /* JCE_EDITOR_DEFAULTS_H */
