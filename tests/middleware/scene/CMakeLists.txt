cmake_minimum_required(VERSION 3.20)

# Actual private sky-IBL numeric admission; no graphics device.
jce_add_unit_test(test_jce_sky_ibl_refresh
    SOURCES test_jce_sky_ibl_refresh.c
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")

jce_add_unit_test(test_jce_ui_canvas_capacity
    SOURCES test_jce_ui_canvas_capacity.c
    LINK jce_core jce_scene jce_resource jce_ui jce_renderer)

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

jce_add_unit_test(test_jce_sr_particle_expanded
    SOURCES test_jce_sr_particle_expanded.c
    LINK bgfx::bgfx
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
if(MSVC)
    # The standalone fixture includes bx through bgfx's C99 API.
    target_compile_options(test_jce_sr_particle_expanded PRIVATE
        /Zc:preprocessor /Zc:__cplusplus)
endif()

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
include("${CMAKE_CURRENT_SOURCE_DIR}/scene_renderer_view_order_controls.cmake")

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
    LINK    jce_core jce_scene
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")

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

# Day/night body choice shares the environment predicate with color and CSM.
jce_add_unit_test(test_jce_tod_key
    SOURCES test_jce_tod_key.c
    LINK jce_core jce_world
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")

# Foliage: actual bounded energy owner; CPU behavior, not rendered pixels.
jce_add_unit_test(test_jce_foliage_lighting
    SOURCES test_jce_foliage_lighting.c
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")

# Rigid imported MeshRenderer state scope; no native rendering in this fixture.
jce_add_unit_test(test_jce_model_cull_override
    SOURCES test_jce_model_cull_override.c
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")

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
include(comp_set_patch_controls.cmake)
include(point_light_cull_write_controls.cmake)

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
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_water_foam.c"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_water_shore_step.c"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_water_shore_state.c"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_water_shore_foam_step.c"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_water_shore_boundary.c"
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
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/rprobe-bake-resolution")
set_tests_properties(test_jce_rprobe_bake_resolution PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/rprobe-bake-resolution")

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

# Dynamic shadow membership remains unique across full and incremental builds.
jce_add_unit_test(test_jce_sr_unique_index
    SOURCES test_jce_sr_unique_index.c)
target_include_directories(test_jce_sr_unique_index PRIVATE
    "${PROJECT_SOURCE_DIR}/engine/src/middleware/scene")

jce_add_unit_test(test_jce_sr_metadata_key
    SOURCES test_jce_sr_metadata_key.c)
target_include_directories(test_jce_sr_metadata_key PRIVATE
    "${PROJECT_SOURCE_DIR}/engine/src/middleware/scene")

jce_add_unit_test(test_jce_sr_occlusion_history
    SOURCES test_jce_sr_occlusion_history.c)
target_include_directories(test_jce_sr_occlusion_history PRIVATE
    "${PROJECT_SOURCE_DIR}/engine/src/middleware/scene")

jce_add_unit_test(test_jce_scene_stress
    SOURCES test_jce_scene_stress.c
    LINK jce_core jce_scene jce_resource)

jce_add_unit_test(test_jce_scene_component_epochs
    SOURCES test_jce_scene_component_epochs.c
    LINK jce_core jce_scene jce_resource)
target_include_directories(test_jce_scene_component_epochs PRIVATE
    "${PROJECT_SOURCE_DIR}/engine/src")

jce_add_unit_test(test_jce_sr_anim_storage
    SOURCES test_jce_sr_anim_storage.c
        "${PROJECT_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_anim_storage.c"
    LINK jce_core)
target_include_directories(test_jce_sr_anim_storage PRIVATE
    "${PROJECT_SOURCE_DIR}/engine/src/middleware/scene"
    "${PROJECT_SOURCE_DIR}/engine/src")

# Real quaternion scene roundtrip preserves authored bits over repeated restores.
jce_add_unit_test(test_jce_scene_rotation_roundtrip
    SOURCES test_jce_scene_rotation_roundtrip.c
    LINK jce_core jce_scene jce_resource)

# Production depth-target ownership and real Noop allocation/lifetime controls.
jce_add_unit_test(test_jce_shadow_target_lifecycle
    SOURCES test_jce_shadow_target_lifecycle.c
    LINK jce_core jce_scene jce_resource jce_renderer)
target_include_directories(test_jce_shadow_target_lifecycle PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")
# The fixture consumes private JceSceneRenderer state and actual Noop stats.
# Keep bgfx headers/linkage private, matching other scene-owner controls.
target_link_libraries(test_jce_shadow_target_lifecycle PRIVATE bgfx::bgfx)
if(MSVC)
    target_compile_options(test_jce_shadow_target_lifecycle PRIVATE /Zc:preprocessor)
endif()

# Production foliage resident-root ownership through a first-party C99 provider seam.
jce_add_unit_test(test_jce_foliage_buffer_lifecycle
    SOURCES test_jce_foliage_buffer_lifecycle.c
    ${PROJECT_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_foliage_buffer.c
    LINK jce_core)
target_include_directories(test_jce_foliage_buffer_lifecycle PRIVATE
    ${PROJECT_SOURCE_DIR}/engine/src)

# Current-frame static scatter identity and failed preparation cache admission.
jce_add_unit_test(test_jce_foliage_shadow_key
    SOURCES test_jce_foliage_shadow_key.c
    ${PROJECT_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_foliage_shadow_key.c
    ${PROJECT_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_foliage_buffer.c
    ${PROJECT_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_foliage_identity.c
    LINK jce_core)
target_include_directories(test_jce_foliage_shadow_key PRIVATE
    ${PROJECT_SOURCE_DIR}/engine/src)

# Exact private scene Forward+ owner: lazy admission, mode edges and OOM retry.
jce_add_unit_test(test_jce_forwardplus_storage_lifecycle
    SOURCES test_jce_forwardplus_storage_lifecycle.c
    ${PROJECT_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_forwardplus_storage.c
    LINK jce_core jce_renderer)
target_include_directories(test_jce_forwardplus_storage_lifecycle PRIVATE
    ${PROJECT_SOURCE_DIR}/engine/src)

# Actual cooked foliage owner plus real VFS; CPU only, no GPU/window.
jce_add_unit_test(test_jce_sr_foliage_cooked_vfs
    SOURCES test_jce_sr_foliage_cooked_vfs.c
    LINK JCE
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
target_link_libraries(test_jce_sr_foliage_cooked_vfs PRIVATE bgfx::bgfx)
if(MSVC)
    target_compile_options(test_jce_sr_foliage_cooked_vfs PRIVATE /Zc:preprocessor)
endif()

# Exact canvas font-cache owner with counted ports; no core allocator duplicate.
jce_add_unit_test(test_jce_ui_font_cache
    SOURCES test_jce_ui_font_cache.c
        "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_ui_canvas_font_cache.c"
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
        "${CMAKE_SOURCE_DIR}/engine/src")

# Production text-fit math; CPU bounds controls do not certify font pixels.
jce_add_unit_test(test_jce_ui_text_fit
    SOURCES test_jce_ui_text_fit.c
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")

# Exact text submission owner with counted ports; no rasterized GPU evidence.
jce_add_unit_test(test_jce_ui_text_draw
    SOURCES test_jce_ui_text_draw.c
        "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_ui_canvas_text.c"
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")

# Exact resource epoch/flat-fallback/cache owners; real core with font ports.
jce_add_unit_test(test_jce_ui_font_resources
    SOURCES test_jce_ui_font_resources.c
        "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_ui_canvas_resources.c"
        "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_ui_canvas_font_cache.c"
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
        "${CMAKE_SOURCE_DIR}/engine/src")

# Compiled runtime contrast: remove only the actual default Symbols leaf.
# It uses the same strict fixture and exact cache owner, not a mirrored chain.
file(READ "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_ui_canvas_resources.c"
    _ui_symbols_owner)
string(REGEX MATCHALL "if [(]s_uc[.]system_symbols\\[0\\] &&"
    _ui_symbols_wires "${_ui_symbols_owner}")
list(LENGTH _ui_symbols_wires _ui_symbols_wire_count)
if(NOT _ui_symbols_wire_count EQUAL 1)
    message(FATAL_ERROR "UI Symbols control requires one real fallback leaf wire")
endif()
string(REPLACE "if (s_uc.system_symbols[0] &&" "if (false &&"
    _ui_symbols_mutant "${_ui_symbols_owner}")
if(_ui_symbols_mutant STREQUAL _ui_symbols_owner)
    message(FATAL_ERROR "UI Symbols control did not remove the real fallback wire")
endif()
set(_ui_symbols_mutant_path
    "${CMAKE_CURRENT_BINARY_DIR}/jce_ui_canvas_resources_old_symbols.c")
jce_test_write_if_changed("${_ui_symbols_mutant_path}" "${_ui_symbols_mutant}")
jce_add_unit_test(test_jce_ui_font_resources_old_symbols
    KIND contract
    SOURCES test_jce_ui_font_resources.c
        "${_ui_symbols_mutant_path}"
        "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_ui_canvas_font_cache.c"
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
        "${CMAKE_SOURCE_DIR}/engine/src")
set_tests_properties(test_jce_ui_font_resources_old_symbols PROPERTIES WILL_FAIL TRUE)
unset(_ui_symbols_owner)
unset(_ui_symbols_wires)
unset(_ui_symbols_wire_count)
unset(_ui_symbols_mutant)
unset(_ui_symbols_mutant_path)

# Exact grass content/foliage-buffer owners with counted ports; CPU only.
jce_add_unit_test(test_jce_grass_shadow_content
    SOURCES test_jce_grass_shadow_content.c
        "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_grass_shadow_content.c"
        "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_foliage_buffer.c"
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")

# Real procedural terrain -> conversion owner CPU controls; no GPU evidence.
jce_add_unit_test(test_jce_sr_terrain_vertices
    SOURCES test_jce_sr_terrain_vertices.c
    LINK JCE
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
target_link_libraries(test_jce_sr_terrain_vertices PRIVATE bgfx::bgfx)
if(MSVC)
    target_compile_options(test_jce_sr_terrain_vertices PRIVATE /Zc:preprocessor)
endif()

# Production grass CPU admission receipts; no renderer or GPU device.
jce_add_unit_test(test_jce_grass_colour_submit
    SOURCES test_jce_grass_colour_submit.c LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
include("${CMAKE_CURRENT_SOURCE_DIR}/grass_early_gate_controls.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/grass_mask_batch_controls.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/colour_special_phase_controls.cmake")

# Exact cluster transient preparation; counted CPU ports, no bgfx/GPU claim.
jce_add_unit_test(test_jce_fcluster_submit
    SOURCES test_jce_fcluster_submit.c LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
set(_fcluster_header "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_fcluster_submit_internal.h")
file(READ "${_fcluster_header}" _fcluster_owner)
string(REGEX MATCHALL "if \\(allocation.count < count\\)" _fcluster_short_guards "${_fcluster_owner}")
list(LENGTH _fcluster_short_guards _fcluster_short_guard_count)
if(NOT _fcluster_short_guard_count EQUAL 1)
    message(FATAL_ERROR "Foliage cluster mutation requires its unique actual allocation guard")
endif()
string(REPLACE "if (allocation.count < count)" "if (false)" _fcluster_old_unchecked "${_fcluster_owner}")
if(_fcluster_old_unchecked STREQUAL _fcluster_owner)
    message(FATAL_ERROR "Foliage cluster mutation did not change its owner")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fcluster-mutation")
set(_fcluster_old_header "${CMAKE_CURRENT_BINARY_DIR}/fcluster-mutation/jce_sr_fcluster_submit_old_unchecked.h")
unset(_fcluster_previous_header)
if(EXISTS "${_fcluster_old_header}")
    file(READ "${_fcluster_old_header}" _fcluster_previous_header)
endif()
if(NOT _fcluster_previous_header STREQUAL _fcluster_old_unchecked)
    file(WRITE "${_fcluster_old_header}" "${_fcluster_old_unchecked}")
endif()
jce_add_unit_test(test_jce_fcluster_submit_old_unchecked KIND contract
    SOURCES test_jce_fcluster_submit.c LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
        "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
target_compile_definitions(test_jce_fcluster_submit_old_unchecked PRIVATE
    JCE_FCLUSTER_SUBMIT_HEADER="${_fcluster_old_header}")
set_tests_properties(test_jce_fcluster_submit_old_unchecked PROPERTIES WILL_FAIL TRUE)

# Real bounded model diagnostics consume private slots and the shared core trace.
set(_model_diagnostics_owner_path
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_model_diagnostics.c")
file(READ "${_model_diagnostics_owner_path}" _model_diagnostics_owner)
file(READ "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_scene_renderer.c"
    _model_diagnostics_renderer)
string(FIND "${_model_diagnostics_renderer}" "void jce_scene_renderer_destroy(JceSceneRenderer *sr)"
    _model_destroy_start)
string(FIND "${_model_diagnostics_renderer}" "    /* The GPU blendshape module's program"
    _model_destroy_end)
if(_model_destroy_start LESS 0 OR _model_destroy_end LESS _model_destroy_start)
    message(FATAL_ERROR "Model diagnostics needs the actual renderer destroy prefix")
endif()
math(EXPR _model_destroy_length "${_model_destroy_end} - ${_model_destroy_start}")
string(SUBSTRING "${_model_diagnostics_renderer}" ${_model_destroy_start} ${_model_destroy_length}
    _model_diagnostics_destroy)
string(REPLACE "jce_scene_renderer_destroy(" "jce_model_diagnostics_test_destroy_prefix("
    _model_diagnostics_destroy "${_model_diagnostics_destroy}")
string(APPEND _model_diagnostics_destroy "}\n")
foreach(_model_diagnostics_variant IN ITEMS normal old_repeat_warning old_reset old_repeat_trace missing_reset_flush missing_destroy_flush)
    set(_model_diagnostics_mutant "${_model_diagnostics_owner}")
    set(_model_diagnostics_destroy_mutant "${_model_diagnostics_destroy}")
    if(NOT _model_diagnostics_variant STREQUAL "normal")
        set(_model_diagnostics_mutation _model_diagnostics_mutant)
        if(_model_diagnostics_variant STREQUAL "old_repeat_warning")
            set(_model_diagnostics_needle "const bool first = !(sr->model_cache_reported_reasons & bit);")
            set(_model_diagnostics_replacement "const bool first = true;")
        elseif(_model_diagnostics_variant STREQUAL "old_reset")
            set(_model_diagnostics_needle "sr->model_cache_reported_reasons = 0;")
            set(_model_diagnostics_replacement "/* old reset omitted diagnostic rearm */")
        elseif(_model_diagnostics_variant STREQUAL "old_repeat_trace")
            set(_model_diagnostics_needle "const bool first_detail = trace_ready && !(sr->model_diagnostics.detailed_reasons & bit);")
            set(_model_diagnostics_replacement "const bool first_detail = trace_ready;")
        elseif(_model_diagnostics_variant STREQUAL "missing_reset_flush")
            set(_model_diagnostics_needle "(void)sr_model_cache_diagnostics_flush(sr, SR_MODEL_DIAGNOSTICS_RESET);")
            set(_model_diagnostics_replacement "/* Former reset omitted its final diagnostic flush. */")
        else()
            set(_model_diagnostics_mutation _model_diagnostics_destroy_mutant)
            set(_model_diagnostics_needle "if (!sr_model_cache_diagnostics_flush(sr, SR_MODEL_DIAGNOSTICS_DESTROY))")
            set(_model_diagnostics_replacement "if (false)")
        endif()
        string(REPLACE "${_model_diagnostics_needle}" "" _model_diagnostics_without
            "${${_model_diagnostics_mutation}}")
        string(LENGTH "${${_model_diagnostics_mutation}}" _model_diagnostics_before_length)
        string(LENGTH "${_model_diagnostics_without}" _model_diagnostics_without_length)
        string(LENGTH "${_model_diagnostics_needle}" _model_diagnostics_needle_length)
        math(EXPR _model_diagnostics_removed
            "${_model_diagnostics_before_length} - ${_model_diagnostics_without_length}")
        if(NOT _model_diagnostics_removed EQUAL _model_diagnostics_needle_length)
            message(FATAL_ERROR "Model diagnostics control requires exactly one actual wire")
        endif()
        string(REPLACE "${_model_diagnostics_needle}" "${_model_diagnostics_replacement}"
            ${_model_diagnostics_mutation} "${${_model_diagnostics_mutation}}")
        set(_model_diagnostics_target
            "test_jce_model_cache_diagnostics_${_model_diagnostics_variant}")
        set(_model_diagnostics_kind contract)
    else()
        set(_model_diagnostics_target test_jce_model_cache_diagnostics)
        set(_model_diagnostics_kind unit)
    endif()
    # Count the actual occupancy reads in the complete production owner.
    # This typed CPU observation port does not replace any cache behavior.
    set(_model_slot_read "const SrModelCache *entry = sr->model_cache[i];")
    string(REPLACE "${_model_slot_read}" "" _model_without_slot_read "${_model_diagnostics_mutant}")
    string(LENGTH "${_model_diagnostics_mutant}" _model_before_slot_read_length)
    string(LENGTH "${_model_without_slot_read}" _model_without_slot_read_length)
    string(LENGTH "${_model_slot_read}" _model_slot_read_length)
    math(EXPR _model_removed_slot_read "${_model_before_slot_read_length} - ${_model_without_slot_read_length}")
    if(NOT _model_removed_slot_read EQUAL _model_slot_read_length)
        message(FATAL_ERROR "Model diagnostics requires one actual occupancy read")
    endif()
    string(REPLACE "${_model_slot_read}"
        "extern void jce_test_model_slot_scanned(void); jce_test_model_slot_scanned(); ${_model_slot_read}"
        _model_diagnostics_mutant "${_model_diagnostics_mutant}")
    set(_model_diagnostics_source
        "${CMAKE_CURRENT_BINARY_DIR}/model_diagnostics_${_model_diagnostics_variant}.c")
    jce_test_write_if_changed("${_model_diagnostics_source}" "${_model_diagnostics_mutant}")
    set(_model_diagnostics_destroy_path
        "${CMAKE_CURRENT_BINARY_DIR}/model_diagnostics_destroy_${_model_diagnostics_variant}.inc")
    jce_test_write_if_changed("${_model_diagnostics_destroy_path}" "${_model_diagnostics_destroy_mutant}")
    jce_add_unit_test(${_model_diagnostics_target}
        KIND ${_model_diagnostics_kind}
        SOURCES test_jce_model_cache_diagnostics.c "${_model_diagnostics_source}"
        LINK jce_core)
    target_include_directories(${_model_diagnostics_target} PRIVATE
        "${CMAKE_SOURCE_DIR}/engine/src"
        "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    target_link_libraries(${_model_diagnostics_target} PRIVATE bgfx::bgfx)
    target_compile_definitions(${_model_diagnostics_target} PRIVATE
        "JCE_MODEL_DIAGNOSTICS_DESTROY=\"${_model_diagnostics_destroy_path}\"")
    if(MSVC)
        target_compile_options(${_model_diagnostics_target} PRIVATE /Zc:preprocessor)
    endif()
    if(NOT _model_diagnostics_variant STREQUAL "normal")
        set_tests_properties(${_model_diagnostics_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()

# Exact camera-prepass MRT owner with counted textures and borrowing FBOs.
set(_prepass_targets_owner_path
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_prepass_targets.c")
file(READ "${_prepass_targets_owner_path}" _prepass_targets_owner)
jce_add_unit_test(test_jce_prepass_target_lifecycle
    SOURCES test_jce_prepass_target_lifecycle.c "${_prepass_targets_owner_path}"
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
        "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")

# Both contract variants compile the complete actual owner with the unchanged
# fixture. Exact length reduction proves one wire, including semicolon needles.
string(LENGTH "${_prepass_targets_owner}" _prepass_targets_owner_length)
foreach(_prepass_targets_variant IN ITEMS old_single_slot omitted_albedo_cleanup)
    if(_prepass_targets_variant STREQUAL "old_single_slot")
        set(_prepass_targets_needle
            "if (slot->used && slot->target.request.view_base == request->view_base)")
        set(_prepass_targets_replacement "if (slot->used)")
    else()
        set(_prepass_targets_needle
            "provider->destroy_texture(target->albedo, provider->user);")
        set(_prepass_targets_replacement "(void)provider;")
    endif()
    string(REPLACE "${_prepass_targets_needle}" ""
        _prepass_targets_without_wire "${_prepass_targets_owner}")
    string(LENGTH "${_prepass_targets_without_wire}" _prepass_targets_without_length)
    string(LENGTH "${_prepass_targets_needle}" _prepass_targets_needle_length)
    math(EXPR _prepass_targets_removed_length
        "${_prepass_targets_owner_length} - ${_prepass_targets_without_length}")
    if(NOT _prepass_targets_removed_length EQUAL _prepass_targets_needle_length)
        message(FATAL_ERROR "Prepass target ${_prepass_targets_variant} requires one exact actual owner wire")
    endif()
    string(REPLACE "${_prepass_targets_needle}" "${_prepass_targets_replacement}"
        _prepass_targets_mutant "${_prepass_targets_owner}")
    if(_prepass_targets_mutant STREQUAL _prepass_targets_owner)
        message(FATAL_ERROR "Prepass target ${_prepass_targets_variant} did not change its actual owner")
    endif()
    set(_prepass_targets_mutant_path
        "${CMAKE_CURRENT_BINARY_DIR}/jce_sr_prepass_targets_${_prepass_targets_variant}.c")
    unset(_prepass_targets_previous)
    if(EXISTS "${_prepass_targets_mutant_path}")
        file(READ "${_prepass_targets_mutant_path}" _prepass_targets_previous)
    endif()
    if(NOT _prepass_targets_previous STREQUAL _prepass_targets_mutant)
        file(WRITE "${_prepass_targets_mutant_path}" "${_prepass_targets_mutant}")
    endif()
    set(_prepass_targets_target
        "test_jce_prepass_target_lifecycle_${_prepass_targets_variant}")
    jce_add_unit_test(${_prepass_targets_target} KIND contract
        SOURCES test_jce_prepass_target_lifecycle.c "${_prepass_targets_mutant_path}"
        LINK jce_core
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    set_tests_properties(${_prepass_targets_target} PROPERTIES WILL_FAIL TRUE)
endforeach()
unset(_prepass_targets_owner_path)
unset(_prepass_targets_owner)
unset(_prepass_targets_owner_length)
unset(_prepass_targets_variant)
unset(_prepass_targets_needle)
unset(_prepass_targets_replacement)
unset(_prepass_targets_without_wire)
unset(_prepass_targets_without_length)
unset(_prepass_targets_needle_length)
unset(_prepass_targets_removed_length)
unset(_prepass_targets_mutant)
unset(_prepass_targets_mutant_path)
unset(_prepass_targets_previous)
unset(_prepass_targets_target)


# Actual resource-preparation body, its complete async upload polls and legacy
# render/reset admission prefixes. Counted ports use the real private layout.
# They certify bounded owner behavior, not full render passes or GPU completion.
set(_srp_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_scene_renderer.c")
set(_srp_reset_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_environment.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_srp_path}" "${_srp_reset_path}")
file(READ "${_srp_path}" _srp_source)
file(READ "${_srp_reset_path}" _srp_reset_source)
function(_jce_srp_slice SOURCE_VARIABLE START END OUTPUT_VARIABLE)
    string(FIND "${${SOURCE_VARIABLE}}" "${START}" _start)
    string(FIND "${${SOURCE_VARIABLE}}" "${END}" _end)
    if(_start LESS 0 OR _end LESS_EQUAL _start)
        message(FATAL_ERROR "Resource preparation requires its exact production body boundaries")
    endif()
    math(EXPR _length "${_end} - ${_start}")
    string(SUBSTRING "${${SOURCE_VARIABLE}}" ${_start} ${_length} _slice)
    set(${OUTPUT_VARIABLE} "${_slice}" PARENT_SCOPE)
endfunction()
_jce_srp_slice(_srp_source "struct SrTexJob {" "static JceAsyncRunResult sr_tex_worker(" _srp_tex_job)
_jce_srp_slice(_srp_source "static void sr_tex_job_destroy(" "/* sr_resolve_texture /" _srp_tex_poll)
_jce_srp_slice(_srp_source "struct SrModelJob {" "static JceAsyncRunResult sr_model_worker(" _srp_model_job)
_jce_srp_slice(_srp_source "static void sr_model_job_destroy(" "static uint32_t sr_path_hash(" _srp_model_poll)
# Resource preparation ends before the separate linear-render wrapper. Keeping
# that wrapper here would define its public symbol and pull the full renderer
# TU through its ordinary-render call; scene_linear_controls owns that wrapper.
_jce_srp_slice(_srp_source "/* Observe each newly terminal job once" "uint16_t jce_scene_renderer_render_linear(" _srp_prepare)
set(_srp_owner "${_srp_tex_job}\n${_srp_model_job}\n${_srp_tex_poll}\n${_srp_model_poll}\n${_srp_prepare}")
_jce_srp_slice(_srp_source "uint16_t jce_scene_renderer_render(" "    sr_nodraw_refresh_registry(scene" _srp_prefix)
string(REPLACE "jce_scene_renderer_render(" "jce_srp_test_render_prefix(" _srp_prefix "${_srp_prefix}")
string(APPEND _srp_prefix "    (void)_t0_sr;\n    return view_id_base;\n}\n")
_jce_srp_slice(_srp_reset_source "void jce_scene_renderer_reset_entity_caches(" "    sr_release_prepass_targets(sr, SR_PREPASS_TARGET_RESET);" _srp_reset)
string(REPLACE "jce_scene_renderer_reset_entity_caches(" "jce_srp_test_reset_prefix(" _srp_reset "${_srp_reset}")
string(APPEND _srp_reset "}\n")
set(_srp_ports
    jce_texture_upload_cpu=jce_srp_test_texture_upload_cpu
    jce_texture_cpu_free=jce_srp_test_texture_cpu_free
    jce_model_upload_gltf_cpu=jce_srp_test_model_upload_gltf_cpu
    jce_model_gltf_cpu_free=jce_srp_test_model_gltf_cpu_free
    jce_model_gpu_bytes=jce_srp_test_model_gpu_bytes
    jce_texture_set_streaming_uploads=jce_srp_test_streaming_uploads
    jce_async_task_is_terminal=jce_srp_test_task_is_terminal
    jce_async_task_state=jce_srp_test_task_state
    jce_async_task_release=jce_srp_test_task_release
    jce_renderer_get_submitted_payload_stats=jce_srp_test_payload_stats
    jce_renderer_get_frame_index=jce_srp_test_frame_index
    jce_renderer_frame_admission_current_internal=jce_srp_test_frame_admission
    sr_model_cache_failure_hash=jce_srp_test_model_failure
    sr_model_cache_diagnostics_bind=jce_srp_test_model_diagnostics_bind
    jce_scene_renderer_prepare_resources=jce_srp_test_prepare_resources)
foreach(_srp_variant IN ITEMS current missing_round_guard missing_admission missing_render_fallback missing_diagnostic_bind)
    set(_srp_variant_owner "${_srp_owner}")
    set(_srp_variant_prefix "${_srp_prefix}")
    set(_srp_kind unit)
    if(NOT _srp_variant STREQUAL "current")
        set(_srp_kind contract)
        if(_srp_variant STREQUAL "missing_round_guard")
            set(_srp_needle "    const bool reuse = sr->resource_prepared_valid && !arrived && !video_pending &&")
            set(_srp_replacement "    const bool reuse = false && sr->resource_prepared_valid && !arrived && !video_pending &&")
            set(_srp_mutation _srp_variant_owner)
        elseif(_srp_variant STREQUAL "missing_admission")
            set(_srp_needle "        jce_renderer_frame_admission_current_internal(sr->renderer) &&")
            set(_srp_replacement "        true && /* Former missing device/context admission. */")
            set(_srp_mutation _srp_variant_owner)
        elseif(_srp_variant STREQUAL "missing_diagnostic_bind")
            set(_srp_needle "    sr_model_cache_diagnostics_bind(sr, scene, facts.context_generation, facts.frame_index);")
            set(_srp_replacement "    /* Missing real scene/context diagnostics admission. */")
            set(_srp_mutation _srp_variant_owner)
        else()
            set(_srp_needle "    if (!jce_scene_renderer_prepare_resources(sr, scene)) return view_id_base;")
            set(_srp_replacement "    (void)scene; /* Missing legacy resource-prepare fallback. */")
            set(_srp_mutation _srp_variant_prefix)
        endif()
        string(REPLACE "${_srp_needle}" "" _srp_without "${${_srp_mutation}}")
        string(LENGTH "${${_srp_mutation}}" _srp_before_length)
        string(LENGTH "${_srp_without}" _srp_without_length)
        string(LENGTH "${_srp_needle}" _srp_needle_length)
        math(EXPR _srp_removed "${_srp_before_length} - ${_srp_without_length}")
        if(NOT _srp_removed EQUAL _srp_needle_length)
            message(FATAL_ERROR "Resource preparation ${_srp_variant} requires exactly one actual owner wire")
        endif()
        string(REPLACE "${_srp_needle}" "${_srp_replacement}" ${_srp_mutation} "${${_srp_mutation}}")
    endif()
    set(_srp_generated "${CMAKE_CURRENT_BINARY_DIR}/jce_srp_${_srp_variant}.inc")
    set(_srp_prefix_path "${CMAKE_CURRENT_BINARY_DIR}/jce_srp_render_${_srp_variant}.inc")
    set(_srp_reset_generated "${CMAKE_CURRENT_BINARY_DIR}/jce_srp_reset.inc")
    foreach(_srp_output IN ITEMS owner prefix reset)
        if(_srp_output STREQUAL "owner")
            set(_srp_output_path "${_srp_generated}")
            set(_srp_output_text "${_srp_variant_owner}")
        elseif(_srp_output STREQUAL "prefix")
            set(_srp_output_path "${_srp_prefix_path}")
            set(_srp_output_text "${_srp_variant_prefix}")
        else()
            set(_srp_output_path "${_srp_reset_generated}")
            set(_srp_output_text "${_srp_reset}")
        endif()
        unset(_srp_previous)
        if(EXISTS "${_srp_output_path}")
            file(READ "${_srp_output_path}" _srp_previous)
        endif()
        if(NOT _srp_previous STREQUAL _srp_output_text)
            file(WRITE "${_srp_output_path}" "${_srp_output_text}")
        endif()
    endforeach()
    if(_srp_variant STREQUAL "current")
        set(_srp_target test_jce_scene_resource_prepare)
        set(_srp_current_owner "${_srp_generated}")
    else()
        set(_srp_target "test_jce_scene_resource_prepare_${_srp_variant}")
    endif()
    jce_add_unit_test(${_srp_target} KIND ${_srp_kind}
        SOURCES test_jce_scene_resource_prepare.c
        LINK jce_core jce_scene jce_resource jce_renderer bgfx::bgfx flecs::flecs_static
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    target_compile_definitions(${_srp_target} PRIVATE ${_srp_ports}
        jce_scene_video_prepare_pending=jce_srp_test_video_pending
        jce_scene_video_prepare_render=jce_srp_test_video_prepare
        "JCE_SRP_OWNER=\"${_srp_generated}\""
        "JCE_SRP_RENDER_PREFIX=\"${_srp_prefix_path}\""
        "JCE_SRP_RESET_PREFIX=\"${_srp_reset_generated}\"")
    if(MSVC)
        target_compile_options(${_srp_target} PRIVATE /Zc:preprocessor)
    endif()
    if(NOT _srp_variant STREQUAL "current")
        set_tests_properties(${_srp_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()

# Complete scene-video owner, real public ECS lifecycle, counted CPU ports.
# The generated renderer admission prefix comes from its exact production body;
# later passes/backend work are deliberately outside this CPU control.
set(_scene_video_owner_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_scene_video.c")
file(READ "${_scene_video_owner_path}" _scene_video_owner)
file(READ "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_scene_renderer.c" _scene_video_renderer)
string(FIND "${_scene_video_renderer}" "uint16_t jce_scene_renderer_render(" _scene_video_render_start)
string(FIND "${_scene_video_renderer}" "    sr_nodraw_refresh_registry(scene" _scene_video_render_end)
if(_scene_video_render_start LESS 0 OR _scene_video_render_end LESS _scene_video_render_start)
    message(FATAL_ERROR "Scene video requires the exact real renderer admission prefix")
endif()
math(EXPR _scene_video_render_length "${_scene_video_render_end} - ${_scene_video_render_start}")
string(SUBSTRING "${_scene_video_renderer}" ${_scene_video_render_start} ${_scene_video_render_length}
    _scene_video_render_prefix)
string(REPLACE "jce_scene_renderer_render(" "jce_sv_test_render_prefix("
    _scene_video_render_prefix "${_scene_video_render_prefix}")
string(APPEND _scene_video_render_prefix "    (void)_t0_sr;\n    return view_id_base;\n}\n")
set(_scene_video_ports
    jce_video_load_memory=jce_sv_test_video_load_memory
    jce_video_unload=jce_sv_test_video_unload
    jce_video_set_loop=jce_sv_test_video_set_loop
    jce_video_has_ended=jce_sv_test_video_has_ended
    jce_video_advance=jce_sv_test_video_advance
    jce_video_get_frame_counter=jce_sv_test_video_get_frame_counter
    jce_video_get_frame_rgba=jce_sv_test_video_get_frame_rgba
    jce_texture_from_rgba=jce_sv_test_texture_from_rgba
    jce_texture_update_rgba_ref=jce_sv_test_texture_update_rgba_ref
    jce_texture_destroy=jce_sv_test_texture_destroy
    jce_fs_host_read_all=jce_sv_test_read_bytes)
foreach(_scene_video_variant IN ITEMS current old_cpu_upload old_playing_gate old_failed_counter missing_render_wire)
    set(_scene_video_source "${_scene_video_owner}")
    set(_scene_video_prefix "${_scene_video_render_prefix}")
    set(_scene_video_kind unit)
    if(NOT _scene_video_variant STREQUAL "current")
        set(_scene_video_kind contract)
        if(_scene_video_variant STREQUAL "old_cpu_upload")
            set(_scene_video_needle "    ++ctx->advances;")
            set(_scene_video_replacement "    ++ctx->advances;\n    sv_upload_latest_frame(c);")
        elseif(_scene_video_variant STREQUAL "old_playing_gate")
            set(_scene_video_needle "    /* A stopped one-shot may own the last newly published frame. */\n    sv_upload_latest_frame(c);")
            set(_scene_video_replacement "    /* Former transport gate loses a final published frame. */\n    if (c->playing) sv_upload_latest_frame(c);")
        elseif(_scene_video_variant STREQUAL "old_failed_counter")
            set(_scene_video_needle "    if (accepted) c->uploaded_counter = counter;")
            set(_scene_video_replacement "    c->uploaded_counter = counter;")
        else()
            set(_scene_video_needle "    if (!jce_scene_renderer_prepare_resources(sr, scene)) return view_id_base;")
            set(_scene_video_replacement "    (void)scene;")
        endif()
        if(_scene_video_variant STREQUAL "missing_render_wire")
            set(_scene_video_before "${_scene_video_prefix}")
        else()
            set(_scene_video_before "${_scene_video_source}")
        endif()
        string(REPLACE "${_scene_video_needle}" "" _scene_video_without "${_scene_video_before}")
        string(LENGTH "${_scene_video_before}" _scene_video_before_length)
        string(LENGTH "${_scene_video_without}" _scene_video_without_length)
        string(LENGTH "${_scene_video_needle}" _scene_video_needle_length)
        math(EXPR _scene_video_removed "${_scene_video_before_length} - ${_scene_video_without_length}")
        if(NOT _scene_video_removed EQUAL _scene_video_needle_length)
            message(FATAL_ERROR "Scene video ${_scene_video_variant} requires one exact owner wire")
        endif()
        string(REPLACE "${_scene_video_needle}" "${_scene_video_replacement}"
            _scene_video_after "${_scene_video_before}")
        if(_scene_video_variant STREQUAL "missing_render_wire")
            set(_scene_video_prefix "${_scene_video_after}")
        else()
            set(_scene_video_source "${_scene_video_after}")
        endif()
    endif()
    set(_scene_video_generated "${CMAKE_CURRENT_BINARY_DIR}/jce_scene_video_${_scene_video_variant}.c")
    set(_scene_video_prefix_path "${CMAKE_CURRENT_BINARY_DIR}/jce_scene_video_render_${_scene_video_variant}.inc")
    foreach(_scene_video_output IN ITEMS source prefix)
        if(_scene_video_output STREQUAL "source")
            set(_scene_video_output_path "${_scene_video_generated}")
            set(_scene_video_output_text "${_scene_video_source}")
        else()
            set(_scene_video_output_path "${_scene_video_prefix_path}")
            set(_scene_video_output_text "${_scene_video_prefix}")
        endif()
        unset(_scene_video_previous)
        if(EXISTS "${_scene_video_output_path}")
            file(READ "${_scene_video_output_path}" _scene_video_previous)
        endif()
        if(NOT _scene_video_previous STREQUAL _scene_video_output_text)
            file(WRITE "${_scene_video_output_path}" "${_scene_video_output_text}")
        endif()
    endforeach()
    if(_scene_video_variant STREQUAL "current")
        set(_scene_video_target test_jce_scene_video_phases)
    else()
        set(_scene_video_target "test_jce_scene_video_phases_${_scene_video_variant}")
    endif()
    jce_add_unit_test(${_scene_video_target} KIND ${_scene_video_kind}
        SOURCES test_jce_scene_video_phases.c "${_scene_video_generated}"
        LINK jce_core jce_scene jce_resource jce_renderer bgfx::bgfx flecs::flecs_static
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    target_compile_definitions(${_scene_video_target} PRIVATE ${_scene_video_ports}
        ${_srp_ports} "JCE_SRP_OWNER=\"${_srp_current_owner}\""
        "JCE_SCENE_VIDEO_RENDER_PREFIX=\"${_scene_video_prefix_path}\"")
    if(MSVC)
        target_compile_options(${_scene_video_target} PRIVATE /Zc:preprocessor)
    endif()
    if(NOT _scene_video_variant STREQUAL "current")
        set_tests_properties(${_scene_video_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_scene_video_variable IN ITEMS owner_path owner renderer render_start render_end render_length
    render_prefix ports variant source prefix kind needle replacement before without before_length
    without_length needle_length removed after generated prefix_path output output_path output_text previous target)
    unset(_scene_video_${_scene_video_variable})
endforeach()
unset(_scene_video_variable)

foreach(_srp_variable IN ITEMS path reset_path source reset_source tex_job tex_poll model_job model_poll
    prepare owner prefix reset ports variant variant_owner variant_prefix kind needle replacement
    mutation without before_length without_length needle_length removed generated prefix_path
    reset_generated output output_path output_text previous target current_owner)
    unset(_srp_${_srp_variable})
endforeach()
unset(_srp_variable)

# Real SceneJson round-trip and full headless Canvas layout/topmost consumption.
# Seven real public Scene/Canvas consumption groups; no GPU/pixel claim.
jce_add_unit_test(test_jce_ui_restore_order KIND integration
    SOURCES test_jce_ui_restore_order.c
    LINK jce_scene jce_resource jce_ui jce_renderer jce_core)

# Ordinary negatives use complete current owners with only former order wires.
# Full real ECS, serialization, persistent headless Canvas and unchanged tests.
set(_ui_restore_canvas_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_ui_canvas.c")
set(_ui_restore_serial_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_scene_components_json.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_ui_restore_canvas_path}" "${_ui_restore_serial_path}")
file(READ "${_ui_restore_canvas_path}" _ui_restore_canvas)
file(READ "${_ui_restore_serial_path}" _ui_restore_serial)
function(_jce_ui_restore_replace_once SOURCE_VARIABLE NEEDLE REPLACEMENT)
    set(_before "${${SOURCE_VARIABLE}}")
    string(REPLACE "${NEEDLE}" "" _without "${_before}")
    string(LENGTH "${_before}" _before_length)
    string(LENGTH "${_without}" _without_length)
    string(LENGTH "${NEEDLE}" _needle_length)
    math(EXPR _removed "${_before_length} - ${_without_length}")
    if(NOT _removed EQUAL _needle_length)
        message(FATAL_ERROR "UI restore-order control requires exactly one real owner wire")
    endif()
    string(REPLACE "${NEEDLE}" "${REPLACEMENT}" _after "${_before}")
    set(${SOURCE_VARIABLE} "${_after}" PARENT_SCOPE)
endfunction()

set(_ui_restore_old_canvas "${_ui_restore_canvas}")
_jce_ui_restore_replace_once(_ui_restore_old_canvas
    "#define LOG_TAG \"ui_canvas\""
    "static int uc_compare_entity_order(const void *left, const void *right)\n{\n    const JceEntity a = *(const JceEntity *)left;\n    const JceEntity b = *(const JceEntity *)right;\n    return (a > b) - (a < b);\n}\n\n#define LOG_TAG \"ui_canvas\"")
_jce_ui_restore_replace_once(_ui_restore_old_canvas
    "    if (!jce_scene_internal_sort_entities(s, kids, (size_t)n)) {\n        if (heap) JCE_FREE(heap);\n        return;\n    }"
    "    qsort(kids, (size_t)n, sizeof(*kids), uc_compare_entity_order);")
_jce_ui_restore_replace_once(_ui_restore_old_canvas
    "    if (!jce_scene_internal_sort_entities(s, cl->list, (size_t)cl->count)) return false;\n" "")
_jce_ui_restore_replace_once(_ui_restore_old_canvas
    "            if (cso <= kso) break; /* Stable ties retain authored canvas order. */"
    "            if (cso < kso || (cso == kso && cl->list[j] <= key)) break;")

set(_ui_restore_old_serial "${_ui_restore_serial}")
_jce_ui_restore_replace_once(_ui_restore_old_serial
    "        if (!jce_scene_internal_each_authored((JceScene *)scene, ser_entity_cb, &ctx)) {\n            cJSON_Delete(root); return NULL;\n        }"
    "        jce_scene_each_entity((JceScene *)scene, ser_entity_cb, &ctx);")
foreach(_ui_restore_variant IN ITEMS old_handle_order old_unordered_save)
    if(_ui_restore_variant STREQUAL "old_handle_order")
        set(_ui_restore_owner "${_ui_restore_old_canvas}")
    else()
        set(_ui_restore_owner "${_ui_restore_old_serial}")
    endif()
    set(_ui_restore_path "${CMAKE_CURRENT_BINARY_DIR}/jce_ui_restore_${_ui_restore_variant}.c")
    unset(_ui_restore_previous)
    if(EXISTS "${_ui_restore_path}")
        file(READ "${_ui_restore_path}" _ui_restore_previous)
    endif()
    if(NOT _ui_restore_previous STREQUAL _ui_restore_owner)
        file(WRITE "${_ui_restore_path}" "${_ui_restore_owner}")
    endif()
    set(_ui_restore_target "test_jce_ui_restore_order_${_ui_restore_variant}")
    jce_add_unit_test(${_ui_restore_target} KIND contract
        SOURCES test_jce_ui_restore_order.c "${_ui_restore_path}"
        LINK jce_scene jce_resource jce_ui jce_renderer jce_core
            bgfx::bgfx cjson::cjson sdl::sdl
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    if(MSVC)
        target_compile_options(${_ui_restore_target} PRIVATE /Zc:preprocessor)
    endif()
    set_tests_properties(${_ui_restore_target} PROPERTIES WILL_FAIL TRUE)
endforeach()
foreach(_ui_restore_variable IN ITEMS canvas_path serial_path canvas serial old_canvas old_serial
    variant owner path previous target)
    unset(_ui_restore_${_ui_restore_variable})
endforeach()
unset(_ui_restore_variable)

# Complete actual skinned-animation owner, real Scene/clip/player math and typed
# model/IO/thread ports. Both views use the production generation guard; the
# owner gathers the complete short animator snapshot before all six pose passes.
set(_anim_mv_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_anim.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_anim_mv_path}")
file(READ "${_anim_mv_path}" _anim_mv_source)
function(_jce_anim_mv_replace_once SOURCE_VARIABLE NEEDLE REPLACEMENT)
    set(_before "${${SOURCE_VARIABLE}}")
    string(REPLACE "${NEEDLE}" "" _without "${_before}")
    string(LENGTH "${_before}" _before_length)
    string(LENGTH "${_without}" _without_length)
    string(LENGTH "${NEEDLE}" _needle_length)
    math(EXPR _removed "${_before_length} - ${_without_length}")
    if(NOT _removed EQUAL _needle_length)
        message(FATAL_ERROR "Multiview animation control requires exactly one actual owner wire")
    endif()
    string(REPLACE "${NEEDLE}" "${REPLACEMENT}" _after "${_before}")
    set(${SOURCE_VARIABLE} "${_after}" PARENT_SCOPE)
endfunction()
foreach(_anim_mv_variant IN ITEMS current old_first_view_selection old_frame_stamp)
    set(_anim_mv_owner "${_anim_mv_source}")
    set(_anim_mv_kind integration)
    if(_anim_mv_variant STREQUAL "old_first_view_selection")
        _jce_anim_mv_replace_once(_anim_mv_owner
            "    list = &scene_animators;"
            "    /* Former first-view focus-cull membership remains the only list. */")
        set(_anim_mv_kind contract)
    elseif(_anim_mv_variant STREQUAL "old_frame_stamp")
        _jce_anim_mv_replace_once(_anim_mv_owner
            "    if (sr->velocity_frame_driven) sr->skin_anim_gen = sr->vel_frame_gen;\n" "")
        _jce_anim_mv_replace_once(_anim_mv_owner
            "        if (sr->skin_anim_gen == sr->vel_frame_gen && sr->anim_storage_scene == scene) return;"
            "        if (sr->skin_anim_gen == sr->vel_frame_gen && sr->anim_storage_scene == scene) return;\n        sr->skin_anim_gen = sr->vel_frame_gen; /* Former stamp before fallible reserve. */")
        set(_anim_mv_kind contract)
    endif()
    # Include the real private header before overriding the realloc port; a
    # compile-definition macro before CRT/owner headers can rename declarations.
    set(_anim_mv_generated "${CMAKE_CURRENT_BINARY_DIR}/jce_sr_anim_mv_${_anim_mv_variant}.c")
    set(_anim_mv_text "#include \"jce_sr_internal.h\"\nextern void *jce_mv_realloc(void *, size_t);\n#undef JCE_REALLOC\n#define JCE_REALLOC(pointer, bytes) jce_mv_realloc((pointer), (bytes))\n${_anim_mv_owner}")
    unset(_anim_mv_previous)
    if(EXISTS "${_anim_mv_generated}")
        file(READ "${_anim_mv_generated}" _anim_mv_previous)
    endif()
    if(NOT _anim_mv_previous STREQUAL _anim_mv_text)
        file(WRITE "${_anim_mv_generated}" "${_anim_mv_text}")
    endif()
    if(_anim_mv_variant STREQUAL "current")
        set(_anim_mv_target test_jce_scene_anim_multiview)
    else()
        set(_anim_mv_target "test_jce_scene_anim_multiview_${_anim_mv_variant}")
    endif()
    jce_add_unit_test(${_anim_mv_target} KIND ${_anim_mv_kind}
        SOURCES test_jce_scene_anim_multiview.c "${_anim_mv_generated}"
        LINK JCE bgfx::bgfx flecs::flecs_static
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    target_compile_definitions(${_anim_mv_target} PRIVATE
        sr_get_model=jce_mv_get_model
        jce_model_get_skeleton=jce_mv_model_skeleton
        jce_model_anim_count=jce_mv_model_anim_count
        jce_model_get_anim=jce_mv_model_anim
        jce_json_parse_file=jce_mv_parse_file
        jce_thread_pool_shared=jce_mv_thread_pool_shared
        sr_sample_morph_track=jce_mv_sample_morph_track
        sr_deform_morph_prims=jce_mv_deform_morph_prims)
    if(MSVC)
        target_compile_options(${_anim_mv_target} PRIVATE /Zc:preprocessor)
    endif()
    if(NOT _anim_mv_variant STREQUAL "current")
        set_tests_properties(${_anim_mv_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_anim_mv_variable IN ITEMS path source variant owner kind generated text previous target)
    unset(_anim_mv_${_anim_mv_variable})
endforeach()
unset(_anim_mv_variable)

include("${CMAKE_CURRENT_SOURCE_DIR}/probe_capture_lifetime_controls.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/prepass_clear_controls.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/velocity_world_history_controls.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/shadow_cache_owner_controls.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/sky_admission_controls.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/grass_colour_binding_controls.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/colour_pass_seed_controls.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/fcluster_colour_binding_controls.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/fcluster_cache_controls.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/water_colour_binding_controls.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/ssgi_lifetime_controls.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/collect_layout_controls.cmake")

jce_add_unit_test(test_jce_water_spectrum_authoring
    SOURCES test_jce_water_spectrum_authoring.c
    LINK jce_core jce_scene jce_resource)

jce_add_unit_test(test_jce_water_gradient
    SOURCES test_jce_water_gradient.c LINK jce_core)
target_include_directories(test_jce_water_gradient PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
jce_add_unit_test(test_jce_water_gradient_gpu
    SOURCES test_jce_water_gradient_gpu.c
        "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_water_gradient_gpu.c"
    LINK jce_core)
target_include_directories(test_jce_water_gradient_gpu PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
    "$<TARGET_PROPERTY:bgfx::bgfx,INTERFACE_INCLUDE_DIRECTORIES>")
if(MSVC)
    target_compile_options(test_jce_water_gradient_gpu PRIVATE /Zc:preprocessor)
endif()

# Temporal foam is scene-owned; GPU ports validate resources without a device.
jce_add_unit_test(test_jce_water_nearshore
    SOURCES test_jce_water_nearshore.c LINK jce_core jce_scene jce_resource)
target_include_directories(test_jce_water_nearshore PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")

jce_add_unit_test(test_jce_water_foam
    SOURCES test_jce_water_foam.c LINK jce_core jce_scene jce_resource)
target_include_directories(test_jce_water_foam PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
jce_add_unit_test(test_jce_water_foam_gpu
    SOURCES test_jce_water_foam_gpu.c
        "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_water_foam_gpu.c"
    LINK jce_core)
target_include_directories(test_jce_water_foam_gpu PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene"
    "$<TARGET_PROPERTY:bgfx::bgfx,INTERFACE_INCLUDE_DIRECTORIES>")
if(MSVC)
    target_compile_options(test_jce_water_foam_gpu PRIVATE /Zc:preprocessor)
endif()

jce_add_unit_test(test_jce_terrain_surface
    SOURCES test_jce_terrain_surface.c LINK jce_core jce_scene jce_resource)
target_include_directories(test_jce_terrain_surface PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
