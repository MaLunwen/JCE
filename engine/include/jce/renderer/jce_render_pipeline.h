/*
 * jce_render_pipeline.h  Unity-style Render Pipeline Asset.
 *
 * A single JSON descriptor (`.rp.json`) that turns rendering features
 * on or off and dials in quality knobs.  Games (and platforms) bind
 * one at engine boot — analogous to URP/HDRP Asset selection in Unity.
 *
 * ===================================================================
 * FIVE-LAYER SETTINGS SYSTEM (low priority -> high; each layer only
 * overrides the fields it sets):
 *
 *   1. CODE DEFAULTS   the preset tables below (preset_low/mid/high/ultra).
 *   2. HARDWARE TIER   auto-detected GPU-score / RAM / cores in
 *                      jce_renderer_caps: score>=7 HIGH, >=4 MEDIUM, else
 *                      LOW; RAM<1024MB or single-core forces LOW.  ULTRA is
 *                      never auto-picked (user-selectable only).
 *   3. PROJECT ASSET   Settings/RenderPipeline.rp.json — the developer
 *                      ships it with the game.  The editor cooks it into the
 *                      game PAK (key settings/render_pipeline.rp.json); the
 *                      runtime resolves CWD -> exe dir -> PAK.  Render
 *                      features + quality knobs live here; changes apply next
 *                      frame (jce_render_pipeline_apply / set_knob /
 *                      set_feature_enabled + end_frame).
 *   4. USER CONFIG     .config/jce.ini — what the player changes.  The
 *                      in-game settings screen's Graphics tab writes render
 *                      choices here; its [performance] section holds the
 *                      BOOT-ONLY memory/thread knobs (see jce_config.h):
 *                        [performance]
 *                        machine_class = auto | low | full
 *                        job_workers   = 0        ; 0=auto, else 1..16
 *   5. ENV VARS        dev / QA / triage; always win.  Force a tier with
 *                      JCE_GPU_TIER=low|medium|high|ultra; JCE_LOW_MEM=1|0;
 *                      each perf feature has a JCE_<NAME> env (see below).
 *
 * TIER PRESETS (layer 1x2):
 *   - LOW    honours the 512 MB / no-discrete-GPU baseline; perf opt-ins
 *            stay AUTO=off so it is byte-identical to the un-optimised path.
 *   - MID    sane defaults for integrated GPUs; the proven draw-call wins
 *            (prim/tex instancing + draw-cmd cache) default ON here and up.
 *   - HIGH   modern desktop discrete.
 *   - ULTRA  everything on; reserved for high-end profiles.
 *
 * PERF TRI-STATES (layer 3 "perf" object; JceRpPerfFeature below):
 *   each key is "auto" | true | false.  Resolution at every engine gate is
 *   ENV first (JCE_PRIM_INSTANCE / JCE_TEX_INSTANCE / JCE_DRAWCMD_CACHE /
 *   JCE_PARALLEL_GATHER / JCE_PARALLEL_SUBMIT / JCE_HIZ_OCCLUSION /
 *   JCE_GPU_SCENE / JCE_FOLIAGE_GPU_CULL / JCE_CROWD_INSTANCE), else the
 *   asset tri-state, else the feature's built-in default.  NOTE:
 *   parallel_gather is mutually exclusive with the instancing batchers (all
 *   gate on !pg_active) — enabling it cancels the draw-call collapse, so it
 *   stays AUTO=off in presets (user opt-in for non-instanceable content).
 *
 * .rp.json v2 EXAMPLE (v1 files without "perf" still load):
 *   {
 *     "$schema": "jce.rp.v2",
 *     "enable_csm": true, "enable_ssao": false, "enable_taa": true,
 *     "shadow_resolution": 512, "csm_cascade_count": 2,
 *     "shadow_filter_quality": 1, "msaa_samples": 1, "post_quality": "high",
 *     "perf": { "prim_instance": "auto", "gpu_scene": false, ... }
 *   }
 *
 * Boot order (per-boot resolution of layers 1-3, then 4/5 layer live):
 *   1. Engine init calls jce_render_pipeline_apply_boot(path).
 *   2. If `path` exists on the host filesystem it is loaded and applied,
 *      else the tier preset (layer 2) is used.
 *   3. default_main re-resolves through the mounted PAK afterwards
 *      (jce_render_pipeline_apply_boot_mounted) so a shipped single-exe game
 *      picks up its packed asset.
 * ===================================================================
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_RENDER_PIPELINE_H
#define JCE_RENDER_PIPELINE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum JceRpQuality {
    JCE_RP_QUALITY_LOW   = 0,
    JCE_RP_QUALITY_MID   = 1,
    JCE_RP_QUALITY_HIGH  = 2,
    JCE_RP_QUALITY_ULTRA = 3
} JceRpQuality;

/* ── Settings S3: CPU/GPU perf-feature toggles ────────────────────
 * The engine's proven perf paths (instancing batchers, draw-cmd cache,
 * parallel CPU paths, GPU culling) historically existed only behind env
 * vars, so shipped games could never enable them and users could never
 * disable them.  Each is a tri-state in the pipeline asset:
 *   -1 auto  = the engine's built-in default for that feature
 *    0 off   = force off
 *    1 on    = force on
 * Env vars still override the asset (dev/forensic precedence). */
typedef enum JceRpPerfFeature {
    JCE_RP_PERF_PRIM_INSTANCE = 0, /* primitive tint-instancing            */
    JCE_RP_PERF_TEX_INSTANCE,      /* texture-array (albedo) instancing    */
    JCE_RP_PERF_DRAWCMD_CACHE,     /* persistent draw-command cache        */
    JCE_RP_PERF_PARALLEL_GATHER,   /* concurrent-ECS color gather          */
    JCE_RP_PERF_PARALLEL_SUBMIT,   /* multi-encoder bgfx submit            */
    JCE_RP_PERF_HIZ_OCCLUSION,     /* Hi-Z GPU occlusion (situational)     */
    JCE_RP_PERF_GPU_SCENE,         /* GPU-driven scene cull (opt-in)       */
    JCE_RP_PERF_FOLIAGE_GPU_CULL,  /* foliage compute cull                 */
    JCE_RP_PERF_CROWD_INSTANCE,    /* GPU crowd skinning (built-in ON)     */
    JCE_RP_PERF_COUNT
} JceRpPerfFeature;

#define JCE_RP_AUTO ((int8_t)-1)

/* The trail length a pipeline gets when its descriptor does not state one --
 * which is every .rp.json written before the key existed.  1.0 is the full
 * frame-to-frame displacement (a 360-degree shutter). */
#define JCE_RP_MOTION_BLUR_DEFAULT 1.0f

typedef struct JceRenderPipelineDesc {
    /* Features */
    bool enable_csm;            /* cascaded shadow maps        */
    bool enable_ssao;
    bool enable_ssr;
    bool enable_taa;
    bool enable_bloom;
    bool enable_volumetric_fog;
    bool enable_gpu_particles;
    /* Per-pixel motion blur.  IMPLEMENTED as of 2026-09-06; it spent its
     * whole life before that set by three editor UIs, serialized to
     * .rp.json, defaulted ON by the ULTRA preset, cooked into the shipped
     * PAK, answered by jce_render_pipeline_feature_enabled("motion_blur")
     * and read by no render pass at all -- the "applied:" log line said
     * mblur=N(unimplemented) so it would not read as a report of work.
     *
     * The consumer is the post-fx composite (jce_postfx_set_motion_blur).
     * It needs a MOTION SOURCE -- the renderer's per-object velocity buffer,
     * or the camera-only reprojection, which needs the camera matrices and a
     * scene depth.  jce_postfx_get_motion_blur_active() answers whether one
     * was there, because "the toggle is on" and "the frame is being blurred"
     * are different facts and this engine has shipped the gap between them. */
    bool enable_motion_blur;
    bool enable_cloth;          /* P3-C.4: soft-body / cloth sim */
    bool enable_stylized_sky;   /* stylized sky dome (mode 3); LOW tier off */

    /* Quality knobs */
    uint16_t shadow_resolution; /* 512 / 1024 / 2048 / 4096    */
    uint8_t  csm_cascade_count; /* 1..4                        */
    /* Shadow FILTER cost tier (per-pixel tap count, not map size):
     *   0 = 1 tap, cascade blend off  (local + CSM hard shadows)
     *   1 = 3x3 PCF everywhere
     *   2 = full (local 3x3, CSM rotated 5x5 + cascade blending)
     * Worst case per pixel under 4 shadowed local lights drops from
     * ~86 taps (tier 2) to ~5 (tier 0) — the dominant fragment cost
     * on the 2008-2010 iGPU baseline. Consumed by the scene renderer
     * via the u_shadowQuality uniform (frame-constant branch in
     * fs_pbr/fs_terrain — no shader permutations). */
    uint8_t  shadow_filter_quality;
    uint8_t  msaa_samples;      /* 1, 2, 4, 8                  */
    /* Resolution the 3D scene renders at, as a fraction of the surface.
     * 1.0 = native, 0.5 = quarter the pixels, 2.0 = supersampled.  The UI is
     * NOT scaled: it is drawn at native resolution over the upsampled scene,
     * which is the point -- text stays crisp while the fill-bound pass gets
     * cheaper.  Same knob as Unity URP's Render Scale, UE's r.ScreenPercentage
     * and Godot's Viewport.scaling_3d_scale.
     *
     * Read through jce_render_pipeline_scene_extent(), NEVER directly: the
     * authored value composes with a low-tier pixel budget, and a second
     * place that multiplies by this field would be a second answer to "how
     * big is the scene target".  It spent a release as the FIRST answer to
     * that question with no readers at all, while the runtime host quietly
     * computed a different one -- see that function. */
    float    render_scale;      /* 0.25 .. 2.0; 1.0 = native */
    JceRpQuality post_quality;

    /* Targets / format.  hdr_color drives ONLY the postfx chain's intermediate
     * format (jce_postfx.c postfx_color_format) and TAA availability; the
     * scene colour target format is chosen from hardware caps alone
     * (jce_offscreen_target.c), so an LDR pipeline still renders the scene
     * into an RGBA16F target. */
    bool hdr_color;             /* true = RGBA16F chain, false = RGBA8 */
    bool depth_prepass;

    /* Settings S3: perf-feature tri-states, indexed by JceRpPerfFeature.
     * -1 auto / 0 force-off / 1 force-on.  Presets fill these; the JSON
     * asset serializes them under the "perf" object. */
    int8_t perf[JCE_RP_PERF_COUNT];

    /* APPEND ONLY BELOW THIS LINE.  This struct is in the frozen public ABI
     * and SDK consumers allocate it by value; a member inserted above shifts
     * every later field for everything already compiled. */

    /* Motion-blur trail length.  0 (the memset value, and the value every
     * .rp.json written before this key existed loads to) means "use the
     * engine default of 1.0" rather than "no blur" -- the ON/OFF decision is
     * enable_motion_blur above, and a preset that says ON must not render
     * unblurred just because it predates the knob.
     *
     * 1.0 smears a pixel across its full frame-to-frame displacement, which
     * is the 360-degree-shutter value; below that is a shorter shutter. */
    float motion_blur_intensity;

    /* SUN ANGULAR DIAMETER, IN DEGREES: the sun's apparent size, which is
     * what decides how fast a shadow's edge softens with distance from
     * whatever cast it (PCSS contact hardening).  The real sun is 0.53; larger
     * values are the usual artistic licence.
     *
     * The unit is the same one Godot's DirectionalLight3D.angular_distance
     * uses, so a value copied from a Godot scene means here what it meant
     * there.  It is a PHYSICAL unit on purpose: the first version of this
     * knob was in shadow-map texels, which made the same authored number
     * produce a different penumbra at every shadow resolution -- and being
     * resolution-independent is the entire reason a blocker search is worth
     * paying for.
     *
     * 0 -- the memset value, and
     * the value every .rp.json written before this key existed loads to --
     * means OFF and byte-identical: the filter radius stays the authored
     * constant, which is what every scene has had.
     *
     * BESIDE shadow_filter_quality rather than on the light or the scene,
     * because it is the same kind of decision: how much the shadow filter is
     * allowed to cost.  The widened kernel is still 25 taps, so the cap that
     * keeps it from breaking into speckle is a constant in the shader, not a
     * second knob -- an unauthored knob is one somebody finds unread later. */
    float sun_soft_size;

    /* Screen-space global illumination.  Default false in every preset except
     * ULTRA: unlike SSAO and SSR it changes a scene's LIGHTING rather than its
     * shading detail, so a project promoted from HIGH must not have its look
     * change underneath it.
     *
     * AT THE END, not beside enable_ssr where it belongs by subject.  This
     * struct is public and its member ORDER is the ABI: inserting the flag
     * next to its siblings moved enable_taa from index 3 to index 4 and
     * everything after it, which check_abi_snapshot.py refused -- correctly.
     * Grouping loses to compatibility here, the same trade dof_enabled above
     * made. */
    bool enable_ssgi;
} JceRenderPipelineDesc;

/* ── Apply / query ─────────────────────────────────────────────── */

/* Apply the descriptor.  Calls the per-feature toggles below and
 * caches the result for jce_render_pipeline_get() / feature queries.
 * NULL is treated as the LOW preset. */
JCE_API void jce_render_pipeline_apply(const JceRenderPipelineDesc *desc);

/* Copy the currently-applied descriptor (or LOW preset on cold call). */
JCE_API void jce_render_pipeline_get(JceRenderPipelineDesc *out);

/* Per-feature query — render-graph code can gate passes off this. */
JCE_API bool jce_render_pipeline_is_feature_enabled(const char *feature);

/* The motion-blur trail length actually in force, with the "0 means the
 * engine default" rule resolved in ONE place.  Every caller that read the
 * field directly would have to repeat that rule, and the first one to forget
 * it turns every pre-existing .rp.json -- which has no such key, so the field
 * memsets to 0 -- into "motion blur on, trail length zero", i.e. a preset
 * that says ON and renders unblurred.  Returns the default before the first
 * apply(). */
JCE_API float jce_render_pipeline_motion_blur_intensity(void);

/* The sun soft size in force, in shadow-map texels.  0 = contact hardening
 * OFF, which is what every .rp.json written before the key existed loads to
 * and what every preset below ULTRA states.  Unlike the motion-blur length,
 * 0 here is a MEANINGFUL value rather than "unset": a fixed-radius filter is
 * a legitimate choice and the cheaper one, so there is no default to resolve
 * and no accessor rule to get wrong.  Returns 0 before the first apply(). */
JCE_API float jce_render_pipeline_sun_soft_size(void);

/* THE resolution the 3D scene renders at, given the surface it will be
 * presented on.  Every offscreen scene target is sized through this and
 * nothing multiplies by render_scale itself.
 *
 * It answers with TWO things composed, in this order:
 *
 *   1. the AUTHORED render_scale, clamped to 0.25..2.0.  This is a decision
 *      somebody made and it applies on every machine, upward included.
 *   2. a pixel BUDGET, applied only on fill-bound hardware (GPU tier LOW, or
 *      MEDIUM without a discrete GPU) and only DOWNWARD.  An iGPU's colour
 *      pass is pixel-count-bound at native resolution, so a large surface has
 *      to give something back to hold 60; a small window keeps rendering 1:1
 *      and stays crisp.  JCE_DYNRES_BUDGET (in megapixels) overrides it.
 *
 * The order matters and is not arbitrary: the budget is a FLOOR under the
 * frame rate, not an opinion about how the game should look, so it may only
 * reduce what the author asked for.  Asking for 2.0 on an iGPU gets you the
 * budget, not a slideshow.
 *
 * Both halves existed before this function and neither reached the other:
 * render_scale was parsed, stored, shown in two editor panels and printed in
 * the "applied:" log line while no renderer read it, and the runtime host's
 * budget scaler ignored the field entirely.  A single reader is the fix; a
 * second caller multiplying by render_scale again would recreate the bug.
 *
 * Never returns 0 in either output for a non-zero surface. */
JCE_API void jce_render_pipeline_scene_extent(uint32_t surface_w,
                                              uint32_t surface_h,
                                              uint32_t *out_w,
                                              uint32_t *out_h);

/* Re-resolve the active pipeline against the CURRENT hardware tier.  The
 * LOW-tier floor is applied during apply() and reads the tier, so a pipeline
 * applied before the tier is known never got clamped.  Callers that change the
 * effective tier must invoke this; the renderer caps layer already does, so
 * the floor holds regardless of whether the tier or the pipeline came first.
 * No-op before the first apply. */
JCE_API void jce_render_pipeline_notify_tier_changed(void);

/* ── P4-E.2: deferred feature toggle ─────────────────────────────
 * jce_render_pipeline_set_feature_enabled() writes into a shadow
 * (pending) descriptor so mid-frame changes don't race with in-flight
 * draw calls.  jce_render_pipeline_end_frame() promotes the pending
 * descriptor to the active one at a safe point (end of frame) and fires
 * the observer if anything changed.  Callers that update the full
 * descriptor at once should continue to use jce_render_pipeline_apply(). */
JCE_API void jce_render_pipeline_set_feature_enabled(const char *feature, bool enabled);
/* Numeric sibling of set_feature_enabled — same deferred pending-desc path.
 * Knobs: "shadow_resolution" (256..8192), "csm_cascade_count" (1..4),
 * "shadow_filter_quality" (0..2), "msaa_samples" (1..16),
 * "render_scale" (0.25..2.0), "post_quality" (0..2). */
JCE_API void jce_render_pipeline_set_knob(const char *name, float value);
JCE_API void jce_render_pipeline_end_frame(void);

/* Settings S3: resolve a perf feature for THIS frame.  Precedence:
 *   env var (memoized at the gate site) > asset tri-state > builtin_default.
 * This only resolves the asset layer: pass the gate's built-in default and
 * keep the env check at the call site.  Cheap (one array read) — safe to
 * call per frame so in-game settings changes land without a restart. */
JCE_API bool jce_render_pipeline_perf_enabled(JceRpPerfFeature f,
                                              bool builtin_default);
/* Stable serialized name for a perf feature (JSON key / UI id). */
JCE_API const char *jce_render_pipeline_perf_name(JceRpPerfFeature f);

/* ── P3-C.4: observer hook ─────────────────────────────────────────
 * Upper-layer subsystems (e.g. cloth/soft-body) register a callback
 * fired whenever the pipeline asset is applied.  Lets them honour
 * HW-tier feature flags without forcing the renderer layer to take
 * a downward dependency on middleware. */
typedef void (*JceRenderPipelineApplyFn)(const JceRenderPipelineDesc *desc,
                                         void *ud);
JCE_API void jce_render_pipeline_set_observer(JceRenderPipelineApplyFn fn,
                                              void *ud);

/* ── JSON I/O ──────────────────────────────────────────────────── */

/* Read `.rp.json` from the host filesystem.  On failure `out` is set
 * to the LOW preset and false is returned. */
/* Shipped single-exe path: load the asset from a mounted PAK/bundle by key
 * (the host-fs loader never fires there — nothing stages loose Settings/). */
struct JcePakArchive;
JCE_API bool jce_render_pipeline_load_pak(const struct JcePakArchive *pak,
                                          const char *pak_key,
                                          JceRenderPipelineDesc *out);
/* Re-run boot resolution once the game's PAKs are mounted (app_init time):
 * host_path from CWD, then from the exe directory, then pak_key from `pak`.
 * Strict no-op when none resolve — the engine-init boot result stands. */
JCE_API void jce_render_pipeline_apply_boot_mounted(const struct JcePakArchive *pak,
                                                    const char *host_path,
                                                    const char *pak_key);
JCE_API bool jce_render_pipeline_load(const char *host_path,
                                      JceRenderPipelineDesc *out);

/* Pretty-print `desc` to `host_path`. */
JCE_API bool jce_render_pipeline_save(const char *host_path,
                                      const JceRenderPipelineDesc *desc);

/* ── Built-in presets ──────────────────────────────────────────── */

JCE_API void jce_render_pipeline_preset_low  (JceRenderPipelineDesc *out);
JCE_API void jce_render_pipeline_preset_mid  (JceRenderPipelineDesc *out);
JCE_API void jce_render_pipeline_preset_high (JceRenderPipelineDesc *out);
JCE_API void jce_render_pipeline_preset_ultra(JceRenderPipelineDesc *out);

/* Convenience: write `out` with the preset matching the current
 * (effective) GPU tier reported by jce_renderer_get_tier(). */
JCE_API void jce_render_pipeline_preset_for_current_tier(
    JceRenderPipelineDesc *out);

/* ── Boot helper ───────────────────────────────────────────────── */

/* Engine-boot autopick: load `host_path` if present, else pick the
 * tier preset.  Logs which path was taken via jce_log_info().
 * Always applies the result; never fails (worst case is LOW preset). */
JCE_API void jce_render_pipeline_apply_boot(const char *host_path);

JCE_EXTERN_C_END

#endif /* JCE_RENDER_PIPELINE_H */
