/*
 * jce_editor_mesh_predecode.h — keep what the mesh validation pass already
 * decoded, instead of decoding every model twice.
 *
 * THE MEASUREMENT THAT MADE THIS EXIST.  Opening caged_kingdom/hidden_cove
 * (1797 entities, 40 distinct glTF models) on 2026-09-03:
 *
 *     kpi:startup_ms      2477   "we reached the first draw call"
 *     kpi:first_frame_ms  6672   the first frame alone took 4.2 s
 *
 * and inside that first frame, timed per stage:
 *
 *     path resolve      3 ms
 *     file read         8 ms
 *     glTF decode    3350 ms     <-- 88%
 *     GPU upload      450 ms
 *
 * Identical on D3D12 (3.74 s) and OpenGL (3.83 s), so it is CPU work, not a
 * driver.  Meanwhile validate_mesh_assets() had ALREADY decoded every one of
 * those models on a background worker -- purely to check they load -- and
 * called jce_model_gltf_cpu_free() on each result.  The expensive half was
 * being paid twice: once off-thread and thrown away, once on the main thread
 * with the window frozen.
 *
 * This store keeps the first one.  The validation worker hands its decode here
 * instead of freeing it; ed_load_model_cb takes it and pays only the upload.
 *
 * KEYED ON THE RAW COMPONENT PATH (MeshRenderer::mesh_path), not on either
 * side's resolved absolute path.  The two sides resolve independently -- the
 * validator joins scene_dir, the loader runs the editor's mesh resolver -- and
 * a key that only matched when those two agreed would miss silently, which
 * looks exactly like the optimisation not being worth anything.
 *
 * OWNERSHIP.  put() takes ownership on success and leaves it with the caller
 * on failure; take() transfers it out.  Anything unclaimed is freed by
 * reset(), which the scene loader calls before filling the store again, so the
 * store holds at most one scene's worth of CPU mesh data.
 *
 * RACE.  The producer is an async worker, the consumer is the render thread,
 * and the first frame can arrive before the validation finishes.  A miss then
 * is not an error: ed_load_model_cb decodes synchronously exactly as it did
 * before, so the worst case is the old behaviour.
 */

#ifndef JCE_EDITOR_MESH_PREDECODE_H
#define JCE_EDITOR_MESH_PREDECODE_H

#include <jce/renderer/jce_model.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Drop and free everything.  Called before a scene fills the store. */
void jce_editor_mesh_predecode_reset(void);

/* Store `cpu` under `key`.  Returns true when the store took ownership; false
 * when it is full or the key is already present, in which case the CALLER
 * still owns `cpu` and must free it. */
bool jce_editor_mesh_predecode_put(const char *key, JceModelCpu *cpu);

/* Remove and return the decode for `key`, or NULL.  Ownership transfers to the
 * caller, which must upload it (jce_model_upload_gltf_cpu consumes it) or free
 * it (jce_model_gltf_cpu_free). */
JceModelCpu *jce_editor_mesh_predecode_take(const char *key);

/* How many decodes are waiting to be claimed — for the startup KPI line. */
int jce_editor_mesh_predecode_pending(void);

/* One line of hit/miss accounting, logged at the first-frame KPI.  Without
 * it a store that never hits is indistinguishable from one that is not
 * worth anything -- and the first version of this did in fact never hit. */
void jce_editor_mesh_predecode_report(void);

/* Decode wall clock (submit -> last shard) and how long the main thread sat
 * blocked waiting for it, in ms.  Both zero when no scene has been prewarmed.
 * The hit counters say the store works; these say whether decoding is still
 * the long pole.  Implemented in jce_editor_scene_serial.cpp. */
void jce_editor_mesh_predecode_timings(double *decode_ms, double *blocked_ms);

/* Block until the scene's decode task finishes or `timeout_ms` elapses.
 * Implemented in jce_editor_scene_serial.cpp, which owns the task. */
void jce_editor_wait_for_mesh_predecode(unsigned timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_MESH_PREDECODE_H */
