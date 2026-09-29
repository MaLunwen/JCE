# editor/src/gizmo/ — Transform & Manipulation Gizmos

## Identity
- **Language**: C++17 (math via `jce_math`)
- **Role**: translate/rotate/scale handles, snap, multi-select pivots. Used inside Scene View.

## File map
- `jce_gizmo.{h,cpp}`         — public API (`begin_frame`, `manipulate(entity, mode)`, `end_frame`).
- `jce_gizmo_draw.cpp`        — handle geometry + bgfx draw lists.
- `jce_gizmo_interact.cpp`    — hit-test, drag math, axis constraint.
- `jce_gizmo_math.h`          — ray-plane / ray-axis / arcball helpers.
- `jce_gizmo_internal.h`      — cross-TU state struct.
- `jce_gizmo_joint.{h,cpp}`   — **P3-C.6** Joint Gizmo overlay. `jce_gizmo_joint_draw_from_info(JcePhysicsJointInfo*)` is the back-end-agnostic core; `jce_gizmo_joint_draw_from_component(scene, owner, JceConstraintComponent*)` is the edit-time helper that resolves world-space anchor/axis from the authoring component so the gizmo is visible without a live physics world. Visual contract: yellow anchor spheres on A & B, cyan A↔B line, axis-coloured primary axis (X=red/Y=green/Z=blue), orange limit geometry (hinge arc / slider segment + ticks / 6DOF wire-box + per-axis arcs via 24-sample polyline). All draws go through `jce_debug_draw_*` — no new render path. Dispatched per-selection from `editor/src/scene/jce_scene_render_draw.cpp::draw_joint_gizmos`, gated by `jce_state_get_show_joint_gizmos()` (toggle in Physics Debugger panel, default on).

- `jce_gizmo_cloth.{h,cpp}`   — **P3-C.4** Cloth gizmo overlay (`jce_gizmo_cloth_draw_from_component`).
- `jce_gizmo_compound_collider.{h,cpp}` — **Per-object compound-collider overlay.** `jce_gizmo_compound_collider_draw_from_component(scene, owner, JceCompoundColliderComponent*)` cooks the model into per-object child shapes and draws each child's wireframe in its own palette colour (box=12 edges, sphere=3 rings, capsule=2 rings+4 sides, convex hull=tight AABB, trimesh=triangle edges capped at 6000 segs). The cook (V-HACD-heavy) is cached keyed by model path + cook settings (`jce_gizmo_compound_collider_clear_cache()` drops it); each frame the cached model-space segments are transformed by the owner's TRS and emitted via `jce_debug_draw_line`. Resolves the model file via `jce_editor_resolve_asset_path`. Dispatched per-selection from `jce_scene_render_draw.cpp::draw_compound_collider_gizmos` (always on for selected entities).

## Rules
1. **No ImGui in math/draw** — interact module reads ImGui IO only at entry; rest is pure C++.
2. **Math via `jce_math`** (vec3, mat4, quat) — never glm, never raw arrays.
3. **Modes**: `TRANSLATE`, `ROTATE`, `SCALE`, `UNIVERSAL`. Snap toggles per mode.
4. **Pivot rules**: local/world toggle is global state in `jce_editor_state`. Multi-select uses bounding-center pivot.
5. **Screen-space scale**: handles are constant pixel size regardless of distance (compute via inverse view-proj).
6. **Undo**: emit single history entry per drag — start on press, commit on release, discard on Esc.
7. **No bgfx headers in `.h`** — only `jce_gizmo.cpp`/`_draw.cpp` may include the renderer.

## Don't
- Don't write directly to entity transforms — go through `jce_scene_*` setters so dirty flags propagate.
- Don't allocate per frame; reuse buffers in `jce_gizmo_internal.h`.
- Don't depend on ImGuizmo — this is our own implementation, intentionally.

## Common tasks
- **Add a new gizmo mode** → extend enum in `jce_gizmo.h`, add hit-test in `_interact.cpp`, geometry in `_draw.cpp`.
- **Tweak snap step** → expose in `editor/src/core/jce_editor_config.cpp`.
