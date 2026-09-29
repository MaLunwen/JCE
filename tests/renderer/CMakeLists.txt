cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  L3 renderer-layer unit tests.                                          #
#                                                                         #
#  Only pure-math camera tests are wired here.  Anything that needs a     #
#  bgfx context (render queue flush, pipeline state, etc.) belongs in    #
#  the integration suite, not in unit tests.                              #
# ----------------------------------------------------------------------- #

# jce_resource is needed even though this test never touches a resource:
# linking jce_primitives.o pulls the rest of jce_renderer, whose shader and
# glTF loaders reference jce_pak_* -- the same widening test_jce_postfx_lut
# documents below.  Still pure CPU at runtime: no bgfx context is created and
# the scissor is engine state, not a GPU call.
jce_add_unit_test(test_jce_draw_scissor
    SOURCES test_jce_draw_scissor.c
    LINK    jce_core jce_renderer jce_resource)

jce_add_unit_test(test_jce_camera
    SOURCES test_jce_camera.c
    LINK    jce_core jce_renderer)

jce_add_unit_test(test_jce_fullscreen_effect_contract
    SOURCES test_jce_fullscreen_effect_contract.c
    LINK    jce_core jce_renderer jce_resource)

jce_add_unit_test(test_jce_render_readback_contract
    SOURCES test_jce_render_readback_contract.c
    LINK    jce_core jce_renderer jce_resource)

# Top 5: project render/quality settings carrier (build emits render_settings.json
# from Project Settings > Quality; the shipped runtime loads + applies it).
jce_add_unit_test(test_jce_render_settings
    SOURCES test_jce_render_settings.c
    LINK    jce_core jce_renderer)

jce_add_unit_test(test_jce_skin_palette
    SOURCES test_jce_skin_palette.c
    LINK    jce_core jce_renderer)

jce_add_unit_test(test_jce_local_shadow
    SOURCES test_jce_local_shadow.c
    LINK    jce_core jce_renderer)

# The cascade fit had no test at all, and its header states the contract it is
# trusted to keep: a caster standing between a cascade and the sun must be in
# that cascade's map, or its shadow truncates and the truncation moves with the
# camera.  Pure CPU -- no bgfx, no frame.
jce_add_unit_test(test_jce_csm_caster_coverage
    SOURCES test_jce_csm_caster_coverage.c
    LINK    jce_core jce_renderer)

# A shadow bias has to mean a distance.  This one used to mean a fraction of
# whatever depth range the cascade fit produced -- 89 mm in cascade 0 and
# 8969 mm in cascade 3 of the same frame.
jce_add_unit_test(test_jce_csm_bias_units
    SOURCES test_jce_csm_bias_units.c
    LINK    jce_core jce_renderer)

jce_add_unit_test(test_jce_ies
    SOURCES test_jce_ies.c
    LINK    jce_core jce_renderer jce_resource)

# The render graph's own self-test, which had no caller anywhere in the tree
# until 2026-08-31.  jce_resource comes along for the same reason the scissor
# test documents above: linking any jce_renderer object pulls the shader and
# glTF loaders, which reference jce_pak_*.
jce_add_unit_test(test_jce_render_graph_selftest
    SOURCES test_jce_render_graph_selftest.c
    LINK    jce_core jce_renderer jce_resource)
# The render-graph header moved to engine/src/renderer on 2026-08-31 -- it was
# public API that cannot be used, see the block at the top of that header.
# Same shape test_jce_cloud_coverage uses to reach an engine-internal header.
target_include_directories(test_jce_render_graph_selftest PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# The 2D quad batcher behind jce_text.c's glyph runs.  Reaches an
# engine-internal header the same way test_jce_render_graph_selftest does.
jce_add_unit_test(test_jce_primitives_selftest
    SOURCES test_jce_primitives_selftest.c
    LINK    jce_core jce_renderer jce_resource)
target_include_directories(test_jce_primitives_selftest PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

jce_add_unit_test(test_jce_scene_pick
    SOURCES test_jce_scene_pick.c
    LINK    jce_core jce_renderer)

jce_add_unit_test(test_jce_light_cluster
    SOURCES test_jce_light_cluster.c
    LINK    jce_core jce_renderer)

# Forward+ clustered lighting ROUND A — pure CPU packing of the light cluster
# into the GPU grid/index/param texel arrays (headless; no bgfx device).
jce_add_unit_test(test_jce_forwardplus_upload
    SOURCES test_jce_forwardplus_upload.c
    LINK    jce_core jce_renderer)

# TAA CPU helper — Halton jitter sequence + projection patcher + camera
# history record (pure math, bgfx-free; locks jce_taa.c behaviour).
jce_add_unit_test(test_jce_taa
    SOURCES test_jce_taa.c
    LINK    jce_core jce_renderer)

# FEATURE 8.2 — particle sub-emitters + events (pure CPU sim, no GPU context).
jce_add_unit_test(test_jce_particles_subemitter
    SOURCES test_jce_particles_subemitter.c
    LINK    jce_core jce_renderer)

# FEATURE 8.3 — per-property lifetime curves + flipbook UV (pure CPU sim).
jce_add_unit_test(test_jce_particles_depth
    SOURCES test_jce_particles_depth.c
    LINK    jce_core jce_renderer)

# FEATURE 8.2/8.3 — *.particles.json (de)serialization round-trip through the
# real jce_particles_desc_load_json (curves + flipbook + nested sub-emitter).
jce_add_unit_test(test_jce_particles_serial
    SOURCES test_jce_particles_serial.c
    LINK    jce_core jce_renderer)

# Pure cooked-texture pixel-chunk sizing (uses jce_tex_encoded_size/mips from
# jce_resource).  Needs engine/src on the include path for the internal header.
jce_add_unit_test(test_jce_tex_cooked_size
    SOURCES test_jce_tex_cooked_size.c
    LINK    jce_core jce_resource)
target_include_directories(test_jce_tex_cooked_size PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# 3D-LUT loader: slice-reorder (jce_lut_strip_to_volume) + half-texel math.
# Binds the PRODUCTION reorder in jce_texture.c — the test used to compile its
# own identical copy, so it could pass while the shipped loader was broken.
# Still pure CPU at runtime (no bgfx context is created); only the LINK widens.
# jce_resource is required because pulling jce_texture.o drags its cooked-
# texture helpers (jce_tex_cooked_pixel_size / jce_tex_decode_to_rgba8, which
# live in jce_resource — see the note in engine/CMakeLists.txt); test_jce_ies
# above already links this exact set for the same reason.  engine/src is on the
# include path for the bgfx-free internal declaration (as test_jce_tex_cooked_size).
jce_add_unit_test(test_jce_postfx_lut
    SOURCES test_jce_postfx_lut.c
    LINK    jce_core jce_renderer jce_resource)
target_include_directories(test_jce_postfx_lut PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# 16-bit greyscale PNG must not reach SDL_image's libpng (heap overrun).
# Decodes a real such PNG through the jce_image dispatcher; links jce_renderer
# for the decode service (which pulls SDL_image + the vendored stb build).
jce_add_unit_test(test_jce_image_gray16
    SOURCES test_jce_image_gray16.c
    LINK    jce_core jce_renderer)

# Dependency-CONFIGURATION guard, not a renderer test.  conanfile.py sets
# harfbuzz:with_glib=False for licensing (LGPL glib subtree out of the SDK),
# which swaps HarfBuzz's Unicode backend from glib to built-in UCDN.
# jce_text.c derives script + direction from that backend, so a regression
# there renders Arabic/Hebrew left-to-right with no other symptom.  Links
# harfbuzz directly because JCE's shaping path needs a live bgfx atlas and
# cannot run headless.
find_package(harfbuzz CONFIG QUIET)
if(TARGET harfbuzz::harfbuzz)
    jce_add_unit_test(test_jce_text_unicode_backend
        SOURCES test_jce_text_unicode_backend.c
        LINK    harfbuzz::harfbuzz)
else()
    message(STATUS "tests: harfbuzz target unavailable — skipping "
                   "test_jce_text_unicode_backend")
endif()

# The shape cache, and it is testable headless BECAUSE jce_text_shape takes an
# hb_font_t rather than a JceFont -- a JceFont needs a rasterised bgfx atlas,
# which is the same reason the test above talks to HarfBuzz directly.  Both
# halves matter here: that the cached run equals what HarfBuzz produced, and
# that the second lookup does not run HarfBuzz at all.  The second is the one
# nothing else can see -- a cache that re-shapes every frame draws identical
# pixels.
if(TARGET harfbuzz::harfbuzz)
    jce_add_unit_test(test_jce_text_shape_cache
        SOURCES test_jce_text_shape_cache.c
        LINK    jce_core jce_renderer jce_resource harfbuzz::harfbuzz)
    # renderer/jce_text_shape.h is src-side because nothing outside this
    # renderer shapes text; same pattern as test_jce_root_motion.
    target_include_directories(test_jce_text_shape_cache PRIVATE
        "${CMAKE_SOURCE_DIR}/engine/src")
else()
    message(STATUS "tests: harfbuzz target unavailable — skipping "
                   "test_jce_text_shape_cache")
endif()

# Locks the LOW-tier floor as an ORDER-INDEPENDENT property: the clamp lives
# inside jce_render_pipeline_apply and reads the hardware tier, so applying
# before the tier is resolved silently skipped it.
jce_add_unit_test(test_jce_render_pipeline_tier_clamp
    SOURCES test_jce_render_pipeline_tier_clamp.c
    LINK    jce_core jce_renderer jce_resource)

jce_add_unit_test(test_jce_view_bands
    SOURCES test_jce_view_bands.c
    LINK    jce_core jce_renderer)
target_include_directories(test_jce_view_bands PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# ----------------------------------------------------------------------- #
#  Previously-unregistered renderer suites.                                #
#                                                                          #
#  Tracked in git, named in no build file, never compiled.  Registered so  #
#  the backend contract and the Hi-Z occlusion policy are actually guarded  #
#  before the world-simulation work leans on them.                         #
# ----------------------------------------------------------------------- #

# NOT REGISTERED: test_jce_backend_contract.c is STALE, not merely orphaned.
# It is written against `JceRenderBackendContract` (fields ndc_depth_scale,
# ndc_depth_bias, shadow_uv_flip_y, screen_uv_flip_y), a type that exists
# nowhere in engine/ — so it cannot compile.  Its intent is still correct and
# still wanted: "backend-space semantics come from runtime caps, not
# shader-language macros" is exactly the contract the sky, water and
# volumetric passes depend on.  Registering it must therefore wait until that
# API is actually introduced; wiring it up now would only add a permanently
# broken target.  Left tracked and unregistered ON PURPOSE, with this note so
# it does not read as another accidental orphan.

# Hi-Z occlusion policy: self-contained scalar math, no bgfx device.
jce_add_unit_test(test_jce_hiz_occlusion
    SOURCES test_jce_hiz_occlusion.c)

# Cloud density field: Perlin-Worley base remap, per-type height gradients,
# detail erosion at the silhouette, seamless XZ tiling.  Pure CPU so the field
# can be baked deterministically with no GPU and no compute shader.
jce_add_unit_test(test_jce_cloud_noise
    SOURCES test_jce_cloud_noise.c
            "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_cloud_noise.c"
    LINK    jce_core)
target_include_directories(test_jce_cloud_noise PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/renderer")

# Top-down cloud transmittance.  Multiplies the sun over the whole world, so
# every failure mode is global and none of them looks like a failure: zeros are
# night, ones are a clear sky, a half-texel offset is wind.
jce_add_unit_test(test_jce_cloud_shadow
    SOURCES test_jce_cloud_shadow.c
            "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_cloud_shadow.c"
            "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_cloud_noise.c"
    LINK    jce_core)
target_include_directories(test_jce_cloud_shadow PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/renderer"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Sky stylisation grade.  Design acceptance criterion 7: provably a no-op at
# identity parameters -- a drifting "disabled" grade is a global colour shift
# that would be hunted in the scattering code.
jce_add_unit_test(test_jce_sky_stylise
    SOURCES test_jce_sky_stylise.c
            "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_sky_stylise.c"
    LINK    jce_core)
target_include_directories(test_jce_sky_stylise PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/renderer"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Coverage must mean coverage: the shader thresholds the baked field, so the
# field has to be uniform or the slider does nothing until it is nearly 1.
jce_add_unit_test(test_jce_cloud_coverage
    SOURCES test_jce_cloud_coverage.c
            "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_cloud_noise.c"
    LINK    jce_core)
target_include_directories(test_jce_cloud_coverage PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/renderer"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Rayleigh coefficients: the only wavelength-dependent term in the sky, and
# therefore the only reason it is blue.  Header-only, so no extra source --
# the test also pins fs_sky.sc's hard-coded copy of the same triple.
jce_add_unit_test(test_jce_rayleigh
    SOURCES test_jce_rayleigh.c
    LINK    jce_core)
target_include_directories(test_jce_rayleigh PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/renderer"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Impostor card placement: where a far-card sits and how big it is.
# The superseded formula agreed with the correct one for every unrotated,
# uniformly-scaled instance, so it survived until a scene with 1790 rotated
# and 1788 non-uniformly-scaled instances made it visible.  The cases here are
# chosen to DISTINGUISH the two -- an identity transform proves nothing.
jce_add_unit_test(test_jce_impostor_place
    SOURCES test_jce_impostor_place.c
    LINK    jce_core jce_renderer jce_resource)

# A CLOSED occlusion portal hides what is behind it, and every case where
# occlusion is not provable must stay visible.
jce_add_unit_test(test_jce_sr_portal
    SOURCES test_jce_sr_portal.c
    LINK    JCE)

# SH9 deringing must remove the negative lobe WITHOUT touching L0, without
# shifting hue, and without altering a probe that never rang.
jce_add_unit_test(test_jce_sh9_dering
    SOURCES test_jce_sh9_dering.c
    LINK    JCE)

# ReflectionProbe.mode selects the source cubemap; a non-container custom
# path must fall back rather than burn one of the eight cache slots.
jce_add_unit_test(test_jce_rprobe_source
    SOURCES test_jce_rprobe_source.c
    LINK    JCE)

# The Quality tier a user picks in Project Settings is persisted ONLY to
# machine-local editor state, so it does not survive a build.  The proposed
# repair -- have Apply write Settings/RenderPipeline.rp.json, which the build
# already stages and the runtime already loads -- is only honest if a tier
# preset survives that round trip, and if the four tiers are distinct on disk.
# Measured here before anything is built on it.
jce_add_unit_test(test_jce_quality_tier_asset_roundtrip
    SOURCES test_jce_quality_tier_asset_roundtrip.c
    LINK    JCE)

# Transparents sorted by camera depth ALONE, so two coplanar quads had no
# defined order and flickered as the camera moved -- JceDrawCmd had no field to
# say which wins (the 16 bits are the old `_pad`).  Observes ORDER through
# jce_rq_analyse's merge count, because the queue exposes no entry accessor and
# adding one for a test would be public API with a single caller.
jce_add_unit_test(test_jce_rq_transparent_priority
    SOURCES test_jce_rq_transparent_priority.c
    LINK    JCE)

# Project Settings > Quality has carried pixel_light_count per level since the
# tab existed, with nothing to hand it to.  The load-bearing case is the
# DEFAULT: budget 0 must select light-for-light what two independent top-N
# passes did, or this lands a silent visual change in every existing project.
# The reflection probe bake's two convolution steps advanced a progress bar and
# did nothing: the .irr.ktx sidecar was a copy of the specular faces and the
# container declared one mip.  Pure CPU integrals, so no GPU here.
jce_add_unit_test(test_jce_probe_convolution
    SOURCES test_jce_probe_convolution.c
    LINK    JCE)
target_include_directories(test_jce_probe_convolution PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# Soft particles (Unity's softParticlesEnabled): the per-level checkbox had no
# depth fade in either particle path to reach.  Everything here is CPU-side --
# the knob, the authored field, and that enabling it REQUESTS the depth
# pre-pass the fade reads.
jce_add_unit_test(test_jce_soft_particles
    SOURCES test_jce_soft_particles.c
    LINK    JCE)
target_include_directories(test_jce_soft_particles PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

jce_add_unit_test(test_jce_pixel_light_budget
    SOURCES test_jce_pixel_light_budget.c
    LINK    JCE)
target_include_directories(test_jce_pixel_light_budget PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")
# jce_sr_internal.h includes <bgfx/c99/bgfx.h> for the handle types in
# JceSceneRenderer.  The test never CALLS bgfx -- sr_select_lights touches only
# frame_sel_* -- but it must see the declarations to size the struct, so bgfx
# is linked for its interface headers alone.
target_link_libraries(test_jce_pixel_light_budget PRIVATE bgfx::bgfx)
if(MSVC)
    # bx/platform.h refuses to compile without it, and this test is not
    # routed through jce_target_defaults() which sets it for engine code.
    target_compile_options(test_jce_pixel_light_budget PRIVATE /Zc:preprocessor)
endif()

# JceSpriteAnimatorComponent.loop was authored, serialised and drawn as a
# checkbox that reached nothing: looping was a property of the Aseprite tag
# and jce_sprite.h had no player-side setter, so there was no door for the
# component's answer to go through.  Pure CPU: a sheet built from a JSON
# atlas on disk, no GPU, no image decode (the image path is never opened by
# the player).
jce_add_unit_test(test_jce_sprite_loop
    SOURCES test_jce_sprite_loop.c
    LINK    jce_core jce_renderer jce_resource)

# Eye adaptation.  The LAW only -- no textures, no GPU -- because every
# property that matters here fails on screen as "the exposure feels wrong",
# which is not a description anybody can act on: sign, convergence, overshoot
# on a dropped frame, the clamp, and what log2(0) does on the first frame.
jce_add_unit_test(test_jce_auto_exposure
    SOURCES test_jce_auto_exposure.c
    LINK    jce_core jce_renderer)

# The material's bgfx stencil word, value by value.  The GPU measurement says
# the word reaches the pixels; it cannot say that LESS is not LEQUAL or that
# INCR saturates where INCR_WRAP rolls -- and most wrong mappings still draw A
# picture, just the wrong one at exactly the pixels a portal edge is made of.
# bgfx::bgfx is on the LINK line for its HEADERS: the engine links bgfx
# privately, so the constants this test compares against do not propagate.
# Still headless -- no device is created.
jce_add_unit_test(test_jce_material_stencil
    SOURCES test_jce_material_stencil.c
    LINK    jce_core jce_renderer jce_resource bgfx::bgfx)
if(MSVC)
    # bx/platform.h refuses to compile without it, and jce_add_unit_test does
    # not set it (no other unit test includes a bgfx header).
    target_compile_options(test_jce_material_stencil PRIVATE /Zc:preprocessor)
endif()

# A .mat.json that names a parent: what the screenshot cannot say.  The
# picture shows an empty child rendering as its parent; it cannot show that a
# child stating ONE key keeps the rest, that a chain resolves, that a cycle
# does not hang, or that Save keeps the link and stores only the overrides --
# and every one of those still draws A material, just the wrong one.
jce_add_unit_test(test_jce_material_inherit
    SOURCES test_jce_material_inherit.c
    LINK    jce_core jce_renderer jce_resource)

# The "0 = the engine default" rule for the motion-blur trail length.  Every
# .rp.json in existence predates that key, so a wrong answer here renders an
# UNBLURRED frame while the toggle says ON -- indistinguishable from the defect
# the feature was written to close.
jce_add_unit_test(test_jce_motion_blur_intensity
    SOURCES test_jce_motion_blur_intensity.c
    LINK    jce_core jce_renderer jce_resource)
