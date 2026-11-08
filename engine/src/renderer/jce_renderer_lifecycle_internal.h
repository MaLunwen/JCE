/* Uninstrumented renderer lifecycle; only the renderer trace adapter calls it. */
#ifndef JCE_RENDERER_LIFECYCLE_INTERNAL_H
#define JCE_RENDERER_LIFECYCLE_INTERNAL_H
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_gfx_types.h>
/* Admission precedes all platform/capability/allocator initialization. */
bool jce_renderer_backend_create_begin_internal(void);
void jce_renderer_backend_create_end_internal(bool created);
void jce_renderer_backend_destroy_end_internal(void);
/* Native-only API-owner read of the existing default-off device DIAG latch.
 * No backend query, clock, allocation or independent policy/registry. */
bool jce_renderer_shader_provenance_policy_internal(void);
JceRenderer *jce_renderer_create_impl(JceWindow *, const JceRendererConfig *);
JceRenderer *jce_renderer_create_headless_impl(void);
JceRenderer *jce_renderer_create_fallback_impl(JceWindow *);
void jce_renderer_destroy_impl(JceRenderer *);
bool jce_renderer_begin_frame_impl(const JceRenderer *, JceWindow *);
bool jce_renderer_begin_frame_3d_impl(const JceRenderer *, JceWindow *,
                                     const JceCamera *, uint16_t);
bool jce_renderer_end_frame_impl(const JceRenderer *);
/* Single actual Web context/epoch authority; no automatic resource recovery.
 * Native, Noop and SDL fallback retain their existing admission policies. */
bool jce_renderer_frame_admission_current_internal(const JceRenderer *);
/* Existing device identity only; false before bind/after unbind or Web loss.
 * API-owner thread, no backend query, allocation or frame advance. */
bool jce_renderer_gpu_observation_context_internal(uint64_t *generation,
                                                    uint32_t *host_frame);
void jce_renderer_trace_snapshot(const JceRenderer *);
/* Engine launch snapshot, bound before app init; no environment parsing here.
 * Phase: 1 bind, 2 changed request, 3 post-app-init, 4 before destroy. Values
 * expose the owner reset flag, not driver acknowledgement or presentation. */
void jce_renderer_launch_vsync_bind_internal(JceRenderer *, bool present,
    bool value, bool requested, bool presentation_available);
void jce_renderer_launch_vsync_observe_internal(JceRenderer *, int phase);
/* Startup-only root snapshot; empty/unset preserves explicit PAK loading.
 * API-owner/current-device admission, no environment read or hot reload. */
bool jce_renderer_shader_dev_bind_internal(JceRenderer *, const char *root);
JceShaderHandle jce_renderer_shader_load_named_internal(const JcePakArchive *,
    const char *vs_base, const char *fs_base);
#endif
