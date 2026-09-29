cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  L4 scene-layer unit tests.                                             #
#                                                                         #
#  Pure-logic modules from engine/src/middleware/scene that do not need   #
#  bgfx or SDL initialised live here.                                     #
# ----------------------------------------------------------------------- #

# Renderer instance-order radix sort (header-only, no renderer dependency).
jce_add_unit_test(test_jce_sr_radix_sort
    SOURCES test_jce_sr_radix_sort.c
    LINK    jce_core)

# Canvas component iterator (replaces a full world walk in the UI overlay).
jce_add_unit_test(test_jce_scene_each_canvas
    SOURCES test_jce_scene_each_canvas.c
    LINK    jce_core jce_scene jce_resource)

jce_add_unit_test(test_jce_lod
    SOURCES test_jce_lod.c
    LINK    jce_core jce_scene jce_resource)

jce_add_unit_test(test_jce_space_partition
    SOURCES test_jce_space_partition.c
    LINK    jce_core jce_scene jce_resource)

jce_add_unit_test(test_jce_scene_clear
    SOURCES test_jce_scene_clear.c
    LINK    jce_core jce_scene jce_resource)

# Per-entity transform generation: set_transform bumps only the moved entity's
# subtree gen (not the global structural epoch), so a moving entity no longer
# invalidates the renderer's static world-cache for the whole scene.
jce_add_unit_test(test_jce_xform_gen
    SOURCES test_jce_xform_gen.c
    LINK    jce_core jce_scene jce_resource)

# Hierarchy world-matrix composition and cycle-safe keep-world reparenting.
jce_add_unit_test(test_jce_scene_world_transform
    SOURCES test_jce_scene_world_transform.c
    LINK    jce_core jce_scene jce_resource)

# Scene-authored primary cameras are the authority for runtime/editor preview
# startup.  This guards parented world poses, projection values, and strict
# rejection of ambiguous primary-camera authoring.
jce_add_unit_test(test_jce_scene_camera_binding
    SOURCES test_jce_scene_camera_binding.c
    LINK    jce_core jce_scene jce_renderer jce_resource)

# Aerial-perspective per-fragment fog factor math: pure C mirror of
# engine/shaders/pbr/fog_apply.sh (LINEAR/EXP/EXP2 + Wronski height integral).
# No engine link needed — self-contained scalar math + Unity.
jce_add_unit_test(test_jce_aerial_fog_math
    SOURCES test_jce_aerial_fog_math.c)

# Stylized sky dome serialization: round-trip + absent-key default.
jce_add_unit_test(test_jce_sky_dome_serialize
    SOURCES test_jce_sky_dome_serialize.c
    LINK    jce_core jce_scene jce_renderer jce_resource)

# Stage 1b.6 grass — GrassField component round-trip + blade-mesh builder
# (uv.y monotonic) + scatter determinism.  jce_resource closes jce_scene's
# private jce_renderer->resource edge (same as the foliage scatter test).
# Task 4: blade-mesh builder sr_grass_build_blade forward-declared in the test,
# resolved via jce_scene link (no jce_sr_internal.h needed at test compile time).
jce_add_unit_test(test_jce_grass_field
    SOURCES test_jce_grass_field.c
    LINK    jce_core jce_scene jce_resource)

# REF-001 (dedup audit) — the prefab loader must accept BOTH on-disk shapes:
# the engine's flat {scene:{entities:[...]}} form AND the editor's nested
# {prefab:{root:{...children...}}} form.  Before REF-001 an editor-authored
# prefab instantiated silently empty; every other prefab test round-trips the
# flat writer against the flat reader, so nothing caught it.
jce_add_unit_test(test_jce_prefab_tree_format
    SOURCES test_jce_prefab_tree_format.c
    LINK    jce_core jce_scene jce_resource)

# Per-character TOON authoring (stylized-slice §5.6) — round-trips the new
# MeshRenderer.toon flag + cel/rim/outline knobs through the REAL component JSON
# serializer; an entity with no toon keys reloads toon=false (legacy byte-id).
jce_add_unit_test(test_jce_scene_toon_authoring
    SOURCES test_jce_scene_toon_authoring.c
    LINK    jce_core jce_scene jce_resource)

# Direction A1: engine-side world partitioning — spatial-hash one authored
# scene into the streaming roster + per-cell fragment trees (pure transform
# over the real scene (de)serialiser).  jce_resource provides the partitioner.
jce_add_unit_test(test_jce_world_partition
    SOURCES test_jce_world_partition.c
    LINK    jce_core jce_scene jce_resource)

# Direction A2: minimal .glb writer (HLOD proxy meshes) — header/chunk framing,
# valid JSON, accessor counts, binary round-trip.
jce_add_unit_test(test_jce_glb_write
    SOURCES test_jce_glb_write.c
    LINK    jce_core jce_resource)

# Direction A2: HLOD bake — merge (world-transform) + simplify + .glb write.
jce_add_unit_test(test_jce_hlod_bake
    SOURCES test_jce_hlod_bake.c
    LINK    jce_core jce_resource)

# Static-batch member table: the sub-ranges that make a merged group cullable
# again.  jce_mesh_merge and jce_static_batch had NO tests before this -- a
# lossless merge that REPLACES geometry in a shipped scene, unexercised.
# The first assertion is the one that matters most: a group entirely on screen
# must still be ONE submit, because a coalescer that emitted one per member
# would be correct AND a regression.
jce_add_unit_test(test_jce_static_batch_members
    SOURCES test_jce_static_batch_members.c
    LINK    jce_core jce_resource)

# Deterministic AI-scene foundation: strict bounded semantic recipes and the
# platform-stable recipe -> FrozenPlan compiler.
jce_add_unit_test(test_jce_scene_recipe
    SOURCES test_jce_scene_recipe.c
    LINK    jce_core jce_scene jce_resource)

jce_add_unit_test(test_jce_scene_compiler
    SOURCES test_jce_scene_compiler.c
    LINK    jce_core jce_scene jce_resource)

jce_add_unit_test(test_jce_scene_transaction
    SOURCES test_jce_scene_transaction.c
    LINK    jce_core jce_scene jce_resource)

# C5-01 / C5-04: a generation-0 handle is bit-identical to a bare index, so
# the old resolve path treated every gen-0 entity as "no generation" and a
# stale handle resolved to whoever recycled the slot — reads AND writes.
jce_add_unit_test(test_jce_scene_entity_generation
    SOURCES test_jce_scene_entity_generation.c
    LINK    jce_core jce_scene jce_resource)

# Shared entity-envelope writers: "disabledComponents" and "layer" used to be
# hand-copied across three writers.  De-duplicating them risks silently
# changing the on-disk scene format, so these assert exact output — including
# the cases where the correct output is nothing.
jce_add_unit_test(test_jce_scene_envelope_writers
    SOURCES test_jce_scene_envelope_writers.c
    LINK    jce_core jce_scene jce_resource)

jce_add_unit_test(test_jce_component_disable
    SOURCES test_jce_component_disable.c
    LINK    jce_core jce_scene jce_resource)

jce_add_unit_test(test_jce_scene_particles_state
    SOURCES test_jce_scene_particles_state.c
    LINK    jce_core jce_scene jce_resource jce_renderer)

# jce_scene_async registers with the player loop, which lives in jce_application.
jce_add_unit_test(test_jce_scene_async_progress
    SOURCES test_jce_scene_async_progress.c
    LINK    jce_core jce_scene jce_resource jce_application)

# GPU Scene / MDI adaptive dispatch policy. Pure workload math: no bgfx device.
jce_add_unit_test(test_jce_sr_gpu_policy
    SOURCES test_jce_sr_gpu_policy.c)
target_include_directories(test_jce_sr_gpu_policy PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")

# A render pipeline may ask for the camera depth pre-pass.  sr_wants_depth_
# prepass() is pure policy -- an applied JceRenderPipelineDesc and four bools,
# no renderer and no bgfx device -- but jce_sr_internal.h needs bgfx's headers
# to size JceSceneRenderer, same as test_jce_pixel_light_budget.
jce_add_unit_test(test_jce_sr_depth_prepass_gate
    SOURCES test_jce_sr_depth_prepass_gate.c
    LINK    JCE)
target_include_directories(test_jce_sr_depth_prepass_gate PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")
target_link_libraries(test_jce_sr_depth_prepass_gate PRIVATE bgfx::bgfx)
if(MSVC)
    target_compile_options(test_jce_sr_depth_prepass_gate PRIVATE /Zc:preprocessor)
endif()

# Primitive-instancing material memo. Pure signature policy: no bgfx device.
jce_add_unit_test(test_jce_sr_prim_material_memo
    SOURCES test_jce_sr_prim_material_memo.c)
target_include_directories(test_jce_sr_prim_material_memo PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")

# Scene-renderer bgfx view ordering + band ownership.  The source has been in
# the tree for a while without ever being registered here, so nothing ran it --
# including when the remap window was widened to a full permutation, which is
# exactly the change it exists to guard.
jce_add_unit_test(test_jce_scene_renderer_view_order
    SOURCES test_jce_scene_renderer_view_order.c
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_scene_renderer_view_order.c")
target_include_directories(test_jce_scene_renderer_view_order PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")

# Scene-scoped string interning. The pointers it hands out live inside
# components that flecs memcpys between tables, so stability across growth is a
# correctness property, not a nicety.
jce_add_unit_test(test_jce_str_intern
    SOURCES test_jce_str_intern.c
    LINK    jce_core jce_scene jce_resource)

# ----------------------------------------------------------------------- #
#  World-simulation suites (terrain / water / foliage).                    #
#                                                                          #
#  These sources were tracked in git but registered in NO build file, so   #
#  they had never compiled and never run -- the same defect this file      #
#  already records for test_jce_scene_renderer_view_order above.  They are #
#  the gates the terrain, water and vegetation designs depend on, so they  #
#  are registered before that work starts rather than after.               #
# ----------------------------------------------------------------------- #

# Terrain: 16-bit heightmap import/export round-trip (+-1 LSB) and resample.
jce_add_unit_test(test_jce_terrain_heightmap_io
    SOURCES test_jce_terrain_heightmap_io.c
    LINK    jce_core jce_scene jce_resource)

# Terrain: hole (cut-cell) parity between the render mesh and the collision
# mesh.  Holes are the one place where geometry and physics must agree by
# construction rather than by tolerance.
jce_add_unit_test(test_jce_terrain_holes
    SOURCES test_jce_terrain_holes.c
    LINK    jce_core jce_scene jce_resource)

# Terrain: tiled/streamed residency, LRU eviction and splat sampling.
jce_add_unit_test(test_jce_terrain_tiling
    SOURCES test_jce_terrain_tiling.c
    LINK    jce_core jce_scene jce_resource)

# Water: Gerstner surface math and component round-trip.
jce_add_unit_test(test_jce_water
    SOURCES test_jce_water.c
    LINK    jce_core jce_scene jce_resource)

# Water: Tessendorf FFT ocean.  The test #includes jce_water_fft.c directly to
# reach its statics, so it needs both the module directory (for that include)
# and engine/src (the module's own "os/core/jce_memory.h").  It must NOT also
# link jce_scene, or the translation unit would be defined twice.
# jce_water_fft.c now builds its initial spectrum through jce_ocean_spectrum,
# so both FFT tests need that translation unit.  It is listed as a SOURCE (not
# #included) because the FFT includes only its HEADER -- listing it here is the
# non-duplicating half of the pattern documented on test_jce_ocean_spectrum.
jce_add_unit_test(test_jce_water_fft
    SOURCES test_jce_water_fft.c
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_ocean_spectrum.c"
    LINK    jce_core)
target_include_directories(test_jce_water_fft PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
    "${CMAKE_SOURCE_DIR}/engine/src")

# CSM: the split vec4 the shadow shader reads.
#
# Linked against jce_scene for sr_pack_csm_splits, which is an INTERNAL symbol
# (jce_sr_internal.h) rather than public ABI -- the packing is a renderer
# detail, but the rule it enforces is not, so it is worth executing.
jce_add_unit_test(test_jce_csm_splits
    SOURCES test_jce_csm_splits.c
    LINK    jce_core jce_scene jce_resource)

# Water: the DISTURBANCE layer -- linearised shallow water over a varying bed.
# Linked against jce_scene rather than compiled in, because the solver is a
# public engine module and a test that built its own copy would stop noticing
# when the shipped one changed.
#
# The load-bearing assertion in here is ENERGY. A wave solver is easy to make
# plausible and hard to make correct: a sign error, a Laplacian that is not
# self-adjoint, a CFL violation and an unstable boundary all produce something
# that ripples, and all four show up as energy GROWING.
jce_add_unit_test(test_jce_water_ripple
    SOURCES test_jce_water_ripple.c
    LINK    jce_core jce_scene)

# Water: the disturbance layer's SEAMS -- scene ownership, and the term the
# buoyancy pass adds. The solver's own tests cannot see either.
jce_add_unit_test(test_jce_water_ripple_wiring
    SOURCES test_jce_water_ripple_wiring.c
    LINK    jce_core jce_scene jce_resource)

# Water: buoyancy sampling, including the world-vs-local XZ regression guard.
jce_add_unit_test(test_jce_water_buoyancy
    SOURCES test_jce_water_buoyancy.c
    LINK    jce_core jce_scene jce_resource)

# Foliage: scatter determinism -- the kept set must be identical whether or not
# slope rejection fires, which is what makes placement cookable.
jce_add_unit_test(test_jce_foliage_scatter
    SOURCES test_jce_foliage_scatter.c
    LINK    jce_core jce_scene jce_resource)

# Foliage: density-mask acceptance, including that the maskless random sequence
# stays byte-identical when masking is enabled.
jce_add_unit_test(test_jce_foliage_density_mask
    SOURCES test_jce_foliage_density_mask.c
    LINK    jce_core jce_scene jce_resource)

# ----------------------------------------------------------------------- #
#  Remaining previously-unregistered suites.                               #
#                                                                          #
#  Same defect as above: tracked in git, named in no build file, never     #
#  compiled.  Registered together so the directory's suite count matches   #
#  its source count and a future orphan is visible as a diff.              #
# ----------------------------------------------------------------------- #

# Component authoring round-trips through the real component JSON serializer.
jce_add_unit_test(test_jce_scene_anim_authoring
    SOURCES test_jce_scene_anim_authoring.c
    LINK    jce_core jce_scene jce_resource)

jce_add_unit_test(test_jce_scene_gas_authoring
    SOURCES test_jce_scene_gas_authoring.c
    LINK    jce_core jce_scene jce_resource)

jce_add_unit_test(test_jce_scene_morph_authoring
    SOURCES test_jce_scene_morph_authoring.c
    LINK    jce_core jce_scene jce_resource jce_animation)

jce_add_unit_test(test_jce_scene_netvar_authoring
    SOURCES test_jce_scene_netvar_authoring.c
    LINK    jce_core jce_scene jce_resource jce_net)

jce_add_unit_test(test_jce_scene_sim_lod_authoring
    SOURCES test_jce_scene_sim_lod_authoring.c
    LINK    jce_core jce_scene jce_resource)

jce_add_unit_test(test_jce_scene_rendering_settings
    SOURCES test_jce_scene_rendering_settings.c
    LINK    jce_core jce_scene jce_resource)

# Light-probe SH9 authoring.  Directly relevant to the environment-lighting
# work: SH9 irradiance is what replaces the hardcoded ambient literals.
jce_add_unit_test(test_jce_scene_lightprobe_sh9
    SOURCES test_jce_scene_lightprobe_sh9.c
    LINK    jce_core jce_scene jce_resource)

# Cross-entity reference remapping on prefab instantiation / scene merge.
jce_add_unit_test(test_jce_scene_ref_remap
    SOURCES test_jce_scene_ref_remap.c
    LINK    jce_core jce_scene jce_resource)

jce_add_unit_test(test_jce_scene_queries
    SOURCES test_jce_scene_queries.c
    LINK    jce_core jce_scene jce_resource)

jce_add_unit_test(test_jce_scene_cloth
    SOURCES test_jce_scene_cloth.c
    LINK    jce_core jce_scene jce_resource jce_physics)

jce_add_unit_test(test_jce_camera_impulse
    SOURCES test_jce_camera_impulse.c
    LINK    jce_core jce_scene jce_resource)

jce_add_unit_test(test_jce_world_origin
    SOURCES test_jce_world_origin.c
    LINK    jce_core jce_scene jce_resource)

# Sequencer: track evaluation, easing and event dispatch.
jce_add_unit_test(test_jce_scene_sequence_player
    SOURCES test_jce_scene_sequence_player.c
    LINK    jce_core jce_scene jce_resource)

# The sequencer's UI vocabulary.  JceSeqPropId could not address a single UI
# element, so every canvas animation in this tree is a script.  The name
# round-trip is already covered by the whole-enum walk in the file above (that
# is what catches a misaligned entry in the positional names table); this file
# covers what a name cannot: support gating, reading one field while writing
# another, a colour track erasing a fade track, and uirect.* resolving a
# different component order than the layout walk -- which would not fail, it
# would animate a rect nobody draws.
jce_add_unit_test(test_jce_seq_ui_props
    SOURCES test_jce_seq_ui_props.c
    LINK    jce_core jce_scene jce_resource)

# The render-target inspector's table.  An inspector is the one tool where the
# failure it is built to find and its own worst failure are the same picture:
# a stale target does not show nothing, it shows last frame's pixels.  So
# every assertion in this file is about ABSENCE -- which targets must NOT be
# listed for a given set of validity bits -- and a happy-path test would pass
# on a table that ignores every flag.  Needs engine/src on the include path
# for the internal header that makes the table reachable without a GPU.
jce_add_unit_test(test_jce_sr_debug_targets
    SOURCES test_jce_sr_debug_targets.c
    LINK    jce_core jce_scene jce_resource)
target_include_directories(test_jce_sr_debug_targets PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

jce_add_unit_test(test_jce_sequencer_easing_events
    SOURCES test_jce_sequencer_easing_events.c
    LINK    jce_core jce_scene jce_resource)

# comp_set (the scripting partial-update seam) must MERGE, not replace.  Every
# parse_* memsets and then fills from literal defaults, so a one-key patch used
# to revert every other field of the component.
jce_add_unit_test(test_jce_comp_set_partial
    SOURCES test_jce_comp_set_partial.c
    LINK    jce_core jce_scene jce_resource)

# The reservation table must equal what the view-order builder actually names.
# A compile-time gate is only as strong as the table it consults, and the table
# lived in a comment until the ECS-UI overlay landed on a named id.
jce_add_unit_test(test_jce_view_reservation_table
    SOURCES test_jce_view_reservation_table.c
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_scene_renderer_view_order.c"
    LINK    jce_core)
target_include_directories(test_jce_view_reservation_table PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")

# RectTransform resolution is Unity UGUI (anchors from the BOTTOM-left, +Y up).
# Nothing covered this: every other UI test pins anchor/pivot to {0,0} with a
# full-height rect, where the UGUI and the (wrong) top-left readings coincide.
jce_add_unit_test(test_jce_ui_rect_transform
    SOURCES test_jce_ui_rect_transform.c
    LINK    jce_core jce_scene jce_resource jce_ui)

# Reparenting must not abort on a name already taken in the new scope.
jce_add_unit_test(test_jce_scene_reparent_name_clash
    SOURCES test_jce_scene_reparent_name_clash.c
    LINK    JCE)

# Runtime UI canvas widgets and their dispatch paths.
jce_add_unit_test(test_jce_ui_widgets
    SOURCES test_jce_ui_widgets.c
    LINK    jce_core jce_scene jce_resource jce_ui)

jce_add_unit_test(test_jce_ui_dropdown
    SOURCES test_jce_ui_dropdown.c
    LINK    jce_core jce_scene jce_resource jce_ui)

# jce_renderer is here for jce_draw_get_scissor ONLY: the clip the canvas asks
# for is renderer state now (see jce_primitives.h), and a test that could not
# read it back would be asserting the ScrollView math while the thing that was
# actually broken -- which rectangle reaches the GPU -- stayed unobservable.
# Still headless: the canvas is created with renderer == NULL and no bgfx
# context exists.
jce_add_unit_test(test_jce_ui_scrollview
    SOURCES test_jce_ui_scrollview.c
    LINK    jce_core jce_scene jce_resource jce_ui jce_renderer)

# Distance-field glyphs, and the atlas ceiling that made them reachable.  Needs
# the module-internal header for uc_get_font / uc_font_px_clamp: the font cache
# and the ceiling are canvas concerns, not public API.
jce_add_unit_test(test_jce_ui_text_sdf
    SOURCES test_jce_ui_text_sdf.c
    LINK    jce_core jce_scene jce_resource jce_ui jce_renderer)
target_include_directories(test_jce_ui_text_sdf PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")

# Weighted per-child sizing and the grid constraint: assertions read where the
# child ACTUALLY DREW (jce_ui_canvas_entity_rect after a real headless render),
# not the arithmetic, so a packed rect nobody consumes still fails.
jce_add_unit_test(test_jce_ui_layout_element
    SOURCES test_jce_ui_layout_element.c
    LINK    jce_core jce_scene jce_resource jce_ui jce_renderer)

# Same-named siblings used to ABORT the process on reparent, so this file is
# its own negative control: before the fix it kills the runner rather than
# reporting a failure.
jce_add_unit_test(test_jce_scene_reparent_names
    SOURCES test_jce_scene_reparent_names.c
    LINK    jce_core jce_scene jce_resource)

# The 129th UI child of a node used to be invisible and unclickable; these
# assertions click the LAST row of a long list, which is the thing a user
# would notice, rather than an internal count.
jce_add_unit_test(test_jce_ui_child_capacity
    SOURCES test_jce_ui_child_capacity.c
    LINK    jce_core jce_scene jce_resource jce_ui jce_renderer)

jce_add_unit_test(test_jce_ui_inputfield
    SOURCES test_jce_ui_inputfield.c
    LINK    jce_core jce_scene jce_resource jce_ui)

# The canvas scaler's Match Width Or Height curve.  The load-bearing
# assertion is BIT-identity at 0.5: every scene written before that field
# existed parses to 0.5, and powf(x,.5)*powf(y,.5) is not bit-identical to
# sqrtf(x*y) -- one ULP there is a pixel in a project nobody touched.
jce_add_unit_test(test_jce_ui_canvas_scaler
    SOURCES test_jce_ui_canvas_scaler.c
    LINK    jce_core jce_scene jce_resource jce_ui)

jce_add_unit_test(test_jce_ui_button_dispatch
    SOURCES test_jce_ui_button_dispatch.c
    LINK    jce_core jce_scene jce_resource jce_script jce_ui)

jce_add_unit_test(test_jce_ui_value_dispatch
    SOURCES test_jce_ui_value_dispatch.c
    LINK    jce_core jce_scene jce_resource jce_script jce_ui)

# Mesh LOD cook: the test includes the resource-private cook header directly.
jce_add_unit_test(test_jce_mesh_lod
    SOURCES test_jce_mesh_lod.c
    LINK    jce_core jce_resource)
target_include_directories(test_jce_mesh_lod PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/resource"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Model cache eviction policy: self-contained scalar policy, no engine link.
jce_add_unit_test(test_jce_model_cache_evict
    SOURCES test_jce_model_cache_evict.c)

# Entity names must be the names the caller authored.  Scene entities live in
# the flecs ROOT scope, so an authored name that collides with a flecs built-in
# ("Empty", "Target", "Prefab", ...) used to be silently renamed to
# "<name>_<id>", breaking every lookup by the authored name.  This was the
# single root cause behind four previously-unregistered authoring tests.
jce_add_unit_test(test_jce_scene_entity_name_collision
    SOURCES test_jce_scene_entity_name_collision.c
    LINK    jce_core jce_scene jce_resource)

# Buoyancy must query the surface the GPU draws.  The FFT ocean is parametric,
# so the height at world p is not h(p) -- it needs the displacement inverted.
# jce_water_fft.c now builds its initial spectrum through jce_ocean_spectrum,
# so both FFT tests need that translation unit.  It is listed as a SOURCE (not
# #included) because the FFT includes only its HEADER -- listing it here is the
# non-duplicating half of the pattern documented on test_jce_ocean_spectrum.
jce_add_unit_test(test_jce_water_fft_inverse
    SOURCES test_jce_water_fft_inverse.c
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_ocean_spectrum.c"
    LINK    jce_core)
target_include_directories(test_jce_water_fft_inverse PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Modern ocean spectra (JONSWAP / PM / TMA + Donelan-Banner spreading + swell).
# Replaces raw Phillips, which has no fetch parameter and poor high-k
# convergence.  Zero runtime cost: evaluated once when the initial spectrum is
# built, never per frame.
# The test #includes the .c directly (same pattern as test_jce_water_fft), so
# the module must NOT also be listed as a source or every symbol is duplicated.
jce_add_unit_test(test_jce_ocean_spectrum
    SOURCES test_jce_ocean_spectrum.c
    LINK    jce_core)
target_include_directories(test_jce_ocean_spectrum PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")

# Phacelle directional noise + Fast Gully Erosion.  Clean-room implementation
# from the published mathematics; the reference is MPL-2.0 and is NOT vendored.
jce_add_unit_test(test_jce_phacelle
    SOURCES test_jce_phacelle.c
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_phacelle.c"
    LINK    jce_core)
target_include_directories(test_jce_phacelle PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")

# Terrain sky occlusion by horizon sweep.  Sky ambient applied unoccluded
# lights the inside of a canyon exactly like open sky; this bakes what each
# cell can actually see, plus a bent normal so the DIRECTION of incoming sky
# light is right too, not just its magnitude.
jce_add_unit_test(test_jce_horizon
    SOURCES test_jce_horizon.c
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_horizon.c"
    LINK    jce_core)
target_include_directories(test_jce_horizon PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")

# The single authority per water body: one wave model, ONE clock, one sampling
# contract.  Before it, the renderer and the physics solver each accumulated
# their own water time, so a boat bobbed to a wave that was not the wave under
# it.  Links the real jce_ocean_spectrum / jce_water / jce_water_fft units --
# the agreement it guarantees is only meaningful against production code.
jce_add_unit_test(test_jce_water_field
    SOURCES test_jce_water_field.c
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_water_field.c"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_water.c"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_water_fft.c"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_ocean_spectrum.c"
    LINK    jce_core)
target_include_directories(test_jce_water_field PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Sky occlusion bake: terrain feeds its NORMALIZED height grid to the horizon
# sweep, which needs world Y.  Forgetting the max_height factor flattens every
# elevation angle and the whole field reads "open sky" -- a plausible-looking
# result that is entirely wrong, so the canyon/flat pair is checked from both
# sides.  jce_horizon.c is a SOURCE (the test includes only headers).
jce_add_unit_test(test_jce_terrain_sky_occlusion
    SOURCES test_jce_terrain_sky_occlusion.c
    LINK    jce_core jce_scene jce_resource)

# One loaded terrain per path, shared by renderer / pick / physics.  The value
# under test is IDENTITY and INVALIDATION -- five independent loaders of one
# asset are five copies that can disagree, and before this there was no
# mechanism at all by which an editor sculpt could reach the collider.
jce_add_unit_test(test_jce_terrain_cache
    SOURCES test_jce_terrain_cache.c
    LINK    jce_core jce_scene jce_resource)
target_include_directories(test_jce_terrain_cache PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Fast Gully Erosion over a whole height grid + the ridge channel.  What is
# assertable about a procedural filter is that it changed something, in the
# right direction, inside the storage range, reproducibly -- and that a zeroed
# parameter struct still erodes rather than silently doing nothing.
jce_add_unit_test(test_jce_terrain_erosion
    SOURCES test_jce_terrain_erosion.c
    LINK    jce_core jce_scene jce_resource)

# Thermal / talus erosion.  Unlike the gully filter this MOVES material rather
# than inventing it, so mass conservation is a hard invariant -- and the first
# thing asserted, because a pass that quietly adds or destroys height is wrong
# however good it looks.
jce_add_unit_test(test_jce_terrain_thermal
    SOURCES test_jce_terrain_thermal.c
    LINK    jce_core jce_scene jce_resource)

# The authoring entry points must REFUSE a terrain with no resident grid.
# A tiled/procedural terrain reports the FULL w/h and a NULL heights pointer,
# so every "is the grid big enough" guard passes and only a null check saves
# you.  Three entry points had none, and a "procedural": true meta reaches
# them through the editor's Load button.  These tests fail by crashing, which
# is the only honest way to test a null dereference.
jce_add_unit_test(test_jce_terrain_authoring_guard
    SOURCES test_jce_terrain_authoring_guard.c
    LINK    jce_core jce_scene jce_resource)

# Foliage surface-type rules: altitude band + ridge/crease preference, the two
# axes slope alone cannot express.  The load-bearing assertion is that neither
# rule consumes randomness -- a ruled scatter must be a strict SUBSET of the
# unruled one at the same seed, or a cooked bake and a runtime scatter would
# silently disagree instance for instance.
jce_add_unit_test(test_jce_foliage_surface_rules
    SOURCES test_jce_foliage_surface_rules.c
    LINK    jce_core jce_scene jce_resource)

# Cooked foliage placement: bake once, ship the list, load instead of
# scattering.  The load-bearing assertion is that a cooked list and a live
# scatter at the same seed are BIT-identical -- the feature is only sound
# because the surface rules consume no randomness.
jce_add_unit_test(test_jce_foliage_cooked
    SOURCES test_jce_foliage_cooked.c
    LINK    jce_core jce_scene jce_resource)

# Concentric-ring ocean geometry.  The value is DISTRIBUTION, not vertex count,
# so the tests check that spacing grows with distance, that the centre is
# capped, and that every interior edge is shared by exactly two triangles --
# a T-junction on a displaced surface is a visible crack, not a subtle one.
jce_add_unit_test(test_jce_water_rings
    SOURCES test_jce_water_rings.c
    LINK    jce_core jce_scene jce_resource)

# CSM shadow-cache caster key.  Every property here is invisible: a wrong key
# does not crash or warn, it freezes a shadow while its caster walks away.
# The combiner must be order-independent (workers finish in any order) and
# NOT self-cancelling (XOR erases a caster folded twice, and an erased caster
# is one whose movement stops invalidating the cache).
jce_add_unit_test(test_jce_shadow_caster_key
    SOURCES test_jce_shadow_caster_key.c
    LINK    jce_core)
target_include_directories(test_jce_shadow_caster_key PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Sun-motion quantisation: the CSM cache compares the light VP bit-exactly, so a
# day/night cycle defeats it entirely unless the sun the shadows use is held
# still between meaningful moves.
jce_add_unit_test(test_jce_shadow_sun
    SOURCES test_jce_shadow_sun.c
    LINK    jce_core jce_scene jce_resource)
target_include_directories(test_jce_shadow_sun PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Sub-texel shadow-caster culling.  Ships already; every way it can be wrong
# removes shadows, and a missing shadow reports nothing.
jce_add_unit_test(test_jce_shadow_lod
    SOURCES test_jce_shadow_lod.c
    LINK    jce_core)
target_include_directories(test_jce_shadow_lod PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Cascade-blend tier policy.  Dither without a temporal resolve is screen-door
# noise that ships looking like a broken filter, not like a missing feature.
jce_add_unit_test(test_jce_shadow_blend
    SOURCES test_jce_shadow_blend.c
    LINK    jce_core jce_scene jce_resource)
target_include_directories(test_jce_shadow_blend PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Contact-shadow march parameters.  Both ends of the ray-length range fail
# quietly and in opposite directions.
jce_add_unit_test(test_jce_contact_shadow
    SOURCES test_jce_contact_shadow.c
    LINK    jce_core jce_scene jce_resource)
target_include_directories(test_jce_contact_shadow PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Beer-Lambert water absorption.  A scalar extinction where three are needed
# renders as grey haze, which reads as a fog setting rather than as a defect.
jce_add_unit_test(test_jce_water_optics
    SOURCES test_jce_water_optics.c
    LINK    jce_core jce_scene jce_resource)
target_include_directories(test_jce_water_optics PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Caustic gain.  1/jacobian diverges at a fold; unclamped it renders as bloom
# and gets "fixed" by turning bloom down.
jce_add_unit_test(test_jce_water_caustics
    SOURCES test_jce_water_caustics.c
    LINK    jce_core jce_scene jce_resource)
target_include_directories(test_jce_water_caustics PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Shoreline foam from water THINNESS.  The term it replaces measured the radius
# from the quad centre, so it foamed in open water and not at the shore.
jce_add_unit_test(test_jce_water_shoreline
    SOURCES test_jce_water_shoreline.c
    LINK    jce_core jce_scene jce_resource)
target_include_directories(test_jce_water_shoreline PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Hashed alpha testing.  The property that makes it correct is statistical --
# surviving coverage must equal alpha -- and a shader cannot be asked that.
jce_add_unit_test(test_jce_hashed_alpha
    SOURCES test_jce_hashed_alpha.c
    LINK    jce_core jce_scene jce_resource)
target_include_directories(test_jce_hashed_alpha PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Masked-caster bucketing.  Ships OFF: the trade has not been measured, and a
# perf flag whose "off" is not byte-identical makes every A/B meaningless.
jce_add_unit_test(test_jce_shadow_bucket
    SOURCES test_jce_shadow_bucket.c
    LINK    jce_core jce_scene jce_resource)
target_include_directories(test_jce_shadow_bucket PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
    "${CMAKE_SOURCE_DIR}/engine/src")
jce_add_unit_test(test_jce_scene_fullscreen_effect
    SOURCES test_jce_scene_fullscreen_effect.c
    LINK    jce_core jce_scene jce_resource)

# The scene's environment must advance with NO RENDERER: jce_environment_advance
# had exactly one caller and it was inside the scene renderer, so a headless
# dedicated server's clock, wetness, snow and sun were frozen for the life of
# the process -- and jce_environment_is_daytime(), the predicate gameplay asks
# "is it night?" with, answered TRUE forever.  Invisible in every configuration
# a developer looks at, because those all have a renderer in them.
jce_add_unit_test(test_jce_scene_environment_clock
    SOURCES test_jce_scene_environment_clock.c
    LINK    jce_core jce_scene jce_resource)

# The ENGINE half of the prefab-instance contract the EDITOR writes.  The editor
# emits an instance as "overrides":[...] plus ONLY the differing components and
# an EMPTY children array -- correct and compact, provided the loader knows to
# instantiate the source first.  The engine's did not: `overrides` appeared zero
# times in jce_scene_serial.c and jce_scene_components_json.c, so a shipped
# level rebuilt every placed prefab as one bare entity holding the overridden
# rows and nothing else.
jce_add_unit_test(test_jce_prefab_override_engine_load
    SOURCES test_jce_prefab_override_engine_load.c
    LINK    jce_core jce_scene jce_resource)

# The retired Animator migrates onto SkeletalAnimator on load, taking the
# rig from the entity's MeshRenderer; an explicit SkeletalAnimator wins.
jce_add_unit_test(test_jce_animator_migrate
    SOURCES test_jce_animator_migrate.c
    LINK    jce_core jce_scene jce_resource)

# Camera.clear_mode must reach the renderer's sky toggle, and the two
# overlay modes must resolve to sky-off rather than to no-clear.
jce_add_unit_test(test_jce_camera_clear_mode
    SOURCES test_jce_camera_clear_mode.c
    LINK    JCE)

# A probe bakes at the resolution its own component authors; the observable
# is the face size in the written KTX header, not the desc we filled.
jce_add_unit_test(test_jce_rprobe_bake_resolution
    SOURCES test_jce_rprobe_bake_resolution.c
    LINK    JCE)

# GrassField.cast_shadow: which blades reach the depth pass, as counts and
# ranges -- the run set IS the submit decision.
jce_add_unit_test(test_jce_grass_shadow
    SOURCES test_jce_grass_shadow.c
    LINK    JCE)

# The tag registry looked names up by full string and stored them
# truncated, so a long tag minted a new slot on every lookup.
jce_add_unit_test(test_jce_tag_registry
    SOURCES test_jce_tag_registry.c
    LINK    JCE)

# LineRenderer / TrailRenderer material_path: the material -> image resolution
# and the arc-length texture parameter.  Linked against jce_scene so it runs
# the SHIPPED sr_material_albedo_path / sr_ribbon_arc_u (both internal symbols
# in jce_sr_ribbon.h) rather than a private copy that could drift.
jce_add_unit_test(test_jce_sr_ribbon
    SOURCES test_jce_sr_ribbon.c
    LINK    jce_core jce_scene jce_resource)
target_include_directories(test_jce_sr_ribbon PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
    "${CMAKE_SOURCE_DIR}/engine/src")

# "PointLight" / "SpotLight" are READ-ONLY compat component types (only the
# unified "Light" row has a serializer), so nothing in the editor's own round
# trip ever exercised them -- and both dropped castsShadow/shadowBias.
jce_add_unit_test(test_jce_light_compat_parsers
    SOURCES test_jce_light_compat_parsers.c
    LINK    JCE)

# Baked light probes must vary with the SHADED POINT, not the camera.  Linked
# against jce_scene so it runs the shipped sr_probe_set_* (internal symbols in
# jce_sr_light_probe.h) rather than a private copy that could drift.
jce_add_unit_test(test_jce_sr_light_probe
    SOURCES test_jce_sr_light_probe.c
    LINK    jce_core jce_scene jce_resource)
target_include_directories(test_jce_sr_light_probe PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Both scene loaders overwrote all six inline PBR factors from material_path
# unconditionally, so tinting three of 200 rocks was discarded on the next
# open -- silently, with the authored value still in the scene file.  The
# load-bearing case is mask 0: every existing scene must load byte-identically.
# Every component in the registry, added the way an SDK / script / CLI adds one
# (a zeroed payload through the type-erased setter), then saved and reloaded.
# One component checked by hand turned up a segfault; this asks the question
# about the whole authoring surface instead.
jce_add_unit_test(test_jce_authoring_surface_sweep
    SOURCES test_jce_authoring_surface_sweep.c
    LINK    JCE)

# The mechanism an entity-scoped undo record is built on: capture one entity's
# components AND its component set, restore by removing what the set lacks then
# parsing the components back.  The editor used to serialise the whole scene
# twice per edit (715 ms + 1.1 s at 50k entities) and restore by clearing it.
jce_add_unit_test(test_jce_entity_scoped_restore
    SOURCES test_jce_entity_scoped_restore.c
    LINK    JCE)

# A zeroed component through the public setter segfaulted the serialiser: the
# interned string fields were assumed non-NULL by every consumer and enforced
# by none.  Found by a harness written to time undo snapshots.
jce_add_unit_test(test_jce_zeroed_component_serialises
    SOURCES test_jce_zeroed_component_serialises.c
    LINK    JCE)

jce_add_unit_test(test_jce_material_override_mask
    SOURCES test_jce_material_override_mask.c
    LINK    JCE)

# JceVirtualCameraComponent.vcam_name was authored, serialised and shown in the
# VCam Manager while nothing under engine/src looked at it -- selection is by
# priority, so the name was a label in a panel and "cut to the camera called
# BossIntro" could not be expressed at all.
jce_add_unit_test(test_jce_vcam_by_name
    SOURCES test_jce_vcam_by_name.c
    LINK    jce_core jce_scene jce_resource)

# The safe area a phone's notch, punch-hole and gesture bar carve out of the
# drawable.  Tests jce_ui_canvas_root_rect, the ONE derivation the renderer
# uses, so the notch's effect can be asserted without standing up a scene or
# a device.  Each case is paired with its opposite: the load-bearing
# assertion is that opting OUT still lays out over the whole drawable.
jce_add_unit_test(test_jce_ui_canvas_safe_area
    SOURCES test_jce_ui_canvas_safe_area.c
    LINK    jce_core jce_scene jce_resource jce_ui)

# The scene could not say where an entity actually IS.  get_world_matrix
# composed the parent chain for RENDERING and everything else read the raw
# JceTransform -- which for a child is an offset, not a place.  Physics read
# it as a place, so a trigger parented to a door at x=4 spawned its collider
# at the world origin.  Unity/UE/Godot all expose both halves; this had only
# the local one.  The load-bearing case is that a ROOT reads back byte-for-
# byte, because every scene authored before this is unparented.
jce_add_unit_test(test_jce_scene_world_pose
    SOURCES test_jce_scene_world_pose.c
    LINK    jce_core jce_scene jce_resource)

# A weapon in a hand has to survive saving the scene.  JceBoneAttachment names
# another entity, and entity ids are NOT stable across save/load -- so the
# load-bearing assertion is that the reference points at the same entity BY
# NAME, not that it is non-zero.  A stale id is non-zero too, and in a scene
# with more than one entity it is a valid id for the wrong object.
jce_add_unit_test(test_jce_bone_attachment_roundtrip
    SOURCES test_jce_bone_attachment_roundtrip.c
    LINK    jce_core jce_scene jce_resource)

# Author-declared script parameters (Unity's [SerializeField], Godot's
# @export).  The ENTITY kind is a cross-entity reference and must join the
# loader's remap -- asserted BY NAME, since a stale id is non-zero too.  The
# clamp cases count rows in the SAVED TEXT: a reload cannot report them,
# because the parser clamps as well and would hand back a legal-looking 8
# whether or not the serialiser read off the end of the array.
jce_add_unit_test(test_jce_script_params_roundtrip
    SOURCES test_jce_script_params_roundtrip.c
    LINK    jce_core jce_scene jce_resource)

# The SCRIPT declaring what it wants from an author -- Unity's
# [SerializeField], Godot's @export.  Until this, the Inspector showed what the
# AUTHOR declared by typing a name and picking a kind, and the script had no
# say; that one fact is why BOTH scripting.inspector.exposed-fields and
# editor.inspector.script-exposed-fields were behind.  The load-bearing cases
# are not "it finds a declaration" (a scanner that returns everything passes
# that) but that a MERGE never destroys an authored value, and that a
# MALFORMED declaration yields NO parameter rather than an invented one.  Pure
# functions over text: no scene, no device, no case may skip.
jce_add_unit_test(test_jce_script_exports
    SOURCES test_jce_script_exports.c
    LINK    jce_core jce_scene)
