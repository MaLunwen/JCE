cmake_minimum_required(VERSION 3.20)
include(render_pipeline_publish_controls.cmake)
include(graphics_api_admission_controls.cmake)
include(render_pipeline_persistence_controls.cmake)
include(postfx_taa_ownership_controls.cmake)
include(model_colour_submit_controls.cmake)

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

jce_add_unit_test(test_jce_skin_deform
    SOURCES test_jce_skin_deform.c
        "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_skin_deform.c"
    LINK jce_core)
target_include_directories(test_jce_skin_deform PRIVATE "${CMAKE_SOURCE_DIR}/engine/src")

jce_add_unit_test(test_jce_fullscreen_effect_contract
    SOURCES test_jce_fullscreen_effect_contract.c
    LINK    jce_core jce_renderer jce_resource)

# Exact readback TU plus the complete actual central submission owner. The
# backend ports count retirement/upload fan-out, not GPU pixels or completion.
set(_readback_renderer "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer.c")
set(_readback_owner "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_render_readback.c")
set(_readback_serial "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer_frame_serial_internal.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_readback_renderer}" "${_readback_owner}" "${_readback_serial}")
file(READ "${_readback_renderer}" _readback_renderer_source)
set(_readback_signature "bool jce_renderer_frame_admission_current_internal(")
set(_readback_closure "\nvoid jce_renderer_destroy_impl(")
string(FIND "${_readback_renderer_source}" "${_readback_signature}" _readback_first)
string(FIND "${_readback_renderer_source}" "${_readback_signature}" _readback_last REVERSE)
string(FIND "${_readback_renderer_source}" "${_readback_closure}" _readback_end)
string(FIND "${_readback_renderer_source}" "${_readback_closure}" _readback_end_last REVERSE)
if(_readback_first LESS 0 OR NOT _readback_first EQUAL _readback_last OR
    _readback_end LESS_EQUAL _readback_first OR NOT _readback_end EQUAL _readback_end_last)
    message(FATAL_ERROR "Readback fixture requires the complete actual admission and central submission owners")
endif()
math(EXPR _readback_length "${_readback_end} - ${_readback_first}")
string(SUBSTRING "${_readback_renderer_source}" ${_readback_first} ${_readback_length}
    _readback_submission)
# This is a wiring guard only; the typed fixture consumes the extracted body.
foreach(_readback_hook IN ITEMS "jce_render_readback_backend_open(r);"
    "jce_render_readback_before_backend_shutdown(r);" "jce_render_readback_after_backend_shutdown(r);")
    string(FIND "${_readback_renderer_source}" "${_readback_hook}" _readback_hook_pos)
    if(_readback_hook_pos LESS 0)
        message(FATAL_ERROR "Readback lifecycle owner hook disappeared: ${_readback_hook}")
    endif()
    string(REPLACE "${_readback_hook}" "" _readback_without_hook "${_readback_renderer_source}")
    string(LENGTH "${_readback_renderer_source}" _readback_original_length)
    string(LENGTH "${_readback_without_hook}" _readback_without_length)
    string(LENGTH "${_readback_hook}" _readback_hook_length)
    math(EXPR _readback_hook_count "(${_readback_original_length} - ${_readback_without_length}) / ${_readback_hook_length}")
    if(_readback_hook STREQUAL "jce_render_readback_backend_open(r);")
        set(_readback_expected_hook_count 2)
    else()
        set(_readback_expected_hook_count 1)
    endif()
    if(NOT _readback_hook_count EQUAL _readback_expected_hook_count)
        message(FATAL_ERROR "Readback fixture requires exact native/headless open and single shutdown hooks")
    endif()
endforeach()
string(FIND "${_readback_renderer_source}" "jce_render_readback_before_backend_shutdown(r);" _readback_before)
string(FIND "${_readback_renderer_source}" "bgfx_shutdown()" _readback_shutdown REVERSE)
string(FIND "${_readback_renderer_source}" "jce_render_readback_after_backend_shutdown(r);" _readback_after)
if(NOT _readback_before LESS _readback_shutdown OR NOT _readback_shutdown LESS _readback_after)
    message(FATAL_ERROR "Readback CPU borrow release must follow actual backend shutdown")
endif()

# Execute the complete actual destroy body, not a copied cleanup sequence.
set(_readback_destroy_signature "\nvoid jce_renderer_destroy_impl(")
set(_readback_destroy_closure "\n/* -- Per-frame ")
string(FIND "${_readback_renderer_source}" "${_readback_destroy_signature}" _readback_destroy_first)
string(FIND "${_readback_renderer_source}" "${_readback_destroy_signature}" _readback_destroy_last REVERSE)
string(FIND "${_readback_renderer_source}" "${_readback_destroy_closure}" _readback_destroy_end)
string(FIND "${_readback_renderer_source}" "${_readback_destroy_closure}" _readback_destroy_end_last REVERSE)
if(_readback_destroy_first LESS 0 OR NOT _readback_destroy_first EQUAL _readback_destroy_last OR
    _readback_destroy_end LESS_EQUAL _readback_destroy_first OR
    NOT _readback_destroy_end EQUAL _readback_destroy_end_last)
    message(FATAL_ERROR "Readback fixture requires the complete actual renderer destroy owner")
endif()
math(EXPR _readback_destroy_length "${_readback_destroy_end} - ${_readback_destroy_first}")
string(SUBSTRING "${_readback_renderer_source}" ${_readback_destroy_first}
    ${_readback_destroy_length} _readback_destroy)
set(_readback_destroy_pair "    JCE_TRACE_SCOPE_CALL(\"renderer.backend_shutdown_cpu\", bgfx_shutdown());\n    jce_render_readback_after_backend_shutdown(r);")
string(FIND "${_readback_destroy}" "${_readback_destroy_pair}" _readback_pair_first)
string(FIND "${_readback_destroy}" "${_readback_destroy_pair}" _readback_pair_last REVERSE)
if(_readback_pair_first LESS 0 OR NOT _readback_pair_first EQUAL _readback_pair_last)
    message(FATAL_ERROR "Readback early-stop control requires one actual backend-return/release pair")
endif()
# The ordinary control mutates only the extracted fixture body. The original
# source still passes the existing configure-time order guard above.
string(REPLACE "${_readback_destroy_pair}"
    "    jce_render_readback_after_backend_shutdown(r);\n    JCE_TRACE_SCOPE_CALL(\"renderer.backend_shutdown_cpu\", bgfx_shutdown());"
    _readback_early_backend_stop "${_readback_destroy}")

file(READ "${_readback_owner}" _readback_actual)
set(_readback_wire "uint32_t next=jce_renderer_submit_frame(queue->renderer);")
string(FIND "${_readback_actual}" "${_readback_wire}" _readback_first)
string(FIND "${_readback_actual}" "${_readback_wire}" _readback_last REVERSE)
if(_readback_first LESS 0 OR NOT _readback_first EQUAL _readback_last)
    message(FATAL_ERROR "Readback direct-frame control requires one actual submission wire")
endif()
string(REPLACE "${_readback_wire}" "uint32_t next=bgfx_frame(false);" _readback_old_direct_frame "${_readback_actual}")
file(READ "${_readback_serial}" _readback_serial_source)
set(_readback_ready "    JceRendererFrameOrder order=jce_renderer_frame_order(now,target);\n    return order==JCE_RENDERER_FRAME_EQUAL || order==JCE_RENDERER_FRAME_AFTER;")
string(FIND "${_readback_serial_source}" "${_readback_ready}" _readback_first)
string(FIND "${_readback_serial_source}" "${_readback_ready}" _readback_last REVERSE)
if(_readback_first LESS 0 OR NOT _readback_first EQUAL _readback_last)
    message(FATAL_ERROR "Readback raw-compare control requires one actual readiness predicate")
endif()
string(REPLACE "${_readback_ready}" "    return now>=target;" _readback_old_raw_compare "${_readback_serial_source}")
set(_readback_ready_trace_wire "        slot->trace_ready=true;")
set(_readback_stop_trace_wire "            slot->trace_backend_stopped=true;")
foreach(_wire IN ITEMS "${_readback_ready_trace_wire}" "${_readback_stop_trace_wire}")
    string(FIND "${_readback_actual}" "${_wire}" _first)
    string(FIND "${_readback_actual}" "${_wire}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "Readback trace controls require one actual ready/stop observation wire")
    endif()
endforeach()
string(REPLACE "${_readback_ready_trace_wire}" "        return;"
    _readback_missing_ready_trace "${_readback_actual}")
string(REPLACE "${_readback_stop_trace_wire}"
    "            slot->trace_ready=true;slot->trace_backend_stopped=true;"
    _readback_stop_claims_ready "${_readback_actual}")
set(_readback_dir "${CMAKE_CURRENT_BINARY_DIR}/readback-lifecycle")
file(MAKE_DIRECTORY "${_readback_dir}")
foreach(_readback_kind IN ITEMS submission old_direct_frame old_raw_compare
    destroy early_backend_stop missing_ready_trace stop_claims_ready)
    set(_readback_path "${_readback_dir}/${_readback_kind}.inc.h")
    set(_readback_content "${_readback_${_readback_kind}}")
    unset(_readback_previous)
    if(EXISTS "${_readback_path}")
        file(READ "${_readback_path}" _readback_previous)
    endif()
    if(NOT _readback_previous STREQUAL _readback_content)
        file(WRITE "${_readback_path}" "${_readback_content}")
    endif()
endforeach()
foreach(_readback_target IN ITEMS test_jce_render_readback_contract
    test_jce_render_readback_old_direct_frame test_jce_render_readback_old_raw_compare
    test_jce_render_readback_early_backend_stop test_jce_render_readback_missing_ready_trace
    test_jce_render_readback_stop_claims_ready)
    if(_readback_target STREQUAL "test_jce_render_readback_contract")
        jce_add_unit_test(${_readback_target} SOURCES test_jce_render_readback_contract.c LINK jce_core)
    else()
        jce_add_unit_test(${_readback_target} KIND contract SOURCES test_jce_render_readback_contract.c LINK jce_core)
        set_tests_properties(${_readback_target} PROPERTIES WILL_FAIL TRUE)
    endif()
    target_link_libraries(${_readback_target} PRIVATE sdl::sdl)
    target_include_directories(${_readback_target} PRIVATE "${CMAKE_SOURCE_DIR}/engine/src"
        "${CMAKE_SOURCE_DIR}/engine/src/renderer"
        "$<TARGET_PROPERTY:bgfx::bgfx,INTERFACE_INCLUDE_DIRECTORIES>")
    set(_readback_destroy_include "${_readback_dir}/destroy.inc.h")
    if(_readback_target STREQUAL "test_jce_render_readback_early_backend_stop")
        set(_readback_destroy_include "${_readback_dir}/early_backend_stop.inc.h")
    endif()
    target_compile_definitions(${_readback_target} PRIVATE
        "JCE_TEST_READBACK_SUBMISSION_OWNER=\"${_readback_dir}/submission.inc.h\""
        "JCE_TEST_READBACK_DESTROY_OWNER=\"${_readback_destroy_include}\"")
    if(_readback_target STREQUAL "test_jce_render_readback_old_direct_frame")
        target_compile_definitions(${_readback_target} PRIVATE
            "JCE_TEST_READBACK_OWNER=\"${_readback_dir}/old_direct_frame.inc.h\"")
    elseif(_readback_target STREQUAL "test_jce_render_readback_old_raw_compare")
        target_compile_definitions(${_readback_target} PRIVATE
            "JCE_TEST_FRAME_SERIAL_HEADER=\"${_readback_dir}/old_raw_compare.inc.h\"")
    endif()
    if(_readback_target STREQUAL "test_jce_render_readback_missing_ready_trace")
        target_compile_definitions(${_readback_target} PRIVATE
            "JCE_TEST_READBACK_OWNER=\"${_readback_dir}/missing_ready_trace.inc.h\"")
    elseif(_readback_target STREQUAL "test_jce_render_readback_stop_claims_ready")
        target_compile_definitions(${_readback_target} PRIVATE
            "JCE_TEST_READBACK_OWNER=\"${_readback_dir}/stop_claims_ready.inc.h\"")
    endif()
    if(MSVC)
        target_compile_options(${_readback_target} PRIVATE /Zc:preprocessor)
    endif()
endforeach()

jce_add_unit_test(test_jce_gpu_survivor_contract
    SOURCES test_jce_gpu_survivor_contract.c
    LINK    jce_core jce_renderer jce_resource)

# Top 5: project render/quality settings carrier (build emits render_settings.json
# from Project Settings > Quality; the shipped runtime loads + applies it).
jce_add_unit_test(test_jce_render_settings
    SOURCES test_jce_render_settings.c
    LINK    jce_core jce_renderer)

jce_add_unit_test(test_jce_pick_geometry
    SOURCES test_jce_pick_geometry.c "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_pick_geometry.c"
    LINK jce_core INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")

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

# Exact PostFX owner with pinned attachment-ref request controls and Noop setup.
jce_add_unit_test(test_jce_postfx_tsr_ownership
    SOURCES test_jce_postfx_tsr_ownership.c
    LINK jce_core jce_renderer jce_resource bgfx::bgfx)
target_include_directories(test_jce_postfx_tsr_ownership PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")
if(MSVC)
    target_compile_options(test_jce_postfx_tsr_ownership PRIVATE /Zc:preprocessor)
endif()

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
    jce_add_unit_test(test_jce_text_real_font
        SOURCES test_jce_text_real_font.c
        LINK jce_core jce_renderer jce_resource harfbuzz::harfbuzz)
    target_include_directories(test_jce_text_real_font PRIVATE
        "${CMAKE_SOURCE_DIR}/engine/src")
    # This fixture consumes the complete font owner with real FT/HB and
    # a Noop atlas, while its original joining oracle stays independent.
    target_link_libraries(test_jce_text_real_font PRIVATE
        bgfx::bgfx Freetype::Freetype sdl::sdl)
    target_compile_definitions(test_jce_text_real_font PRIVATE
        JCE_BUILDING_ENGINE=1
        JCE_TEST_FONT_PATH="${CMAKE_SOURCE_DIR}/engine/ui/fonts/Aileron-Regular.ttf")
    if(MSVC)
        target_compile_options(test_jce_text_real_font PRIVATE /Zc:preprocessor)
    endif()
    # Alternating-order wall samples retain every measurement and prove actual
    # hit/miss deltas. Serial execution excludes only other CTest processes.
    set_tests_properties(test_jce_text_shape_cache PROPERTIES RUN_SERIAL TRUE)
    # renderer/jce_text_shape.h is src-side because nothing outside this
    # renderer shapes text; same pattern as test_jce_root_motion.
    target_include_directories(test_jce_text_shape_cache PRIVATE
        "${CMAKE_SOURCE_DIR}/engine/src")
    target_compile_definitions(test_jce_text_shape_cache PRIVATE
        JCE_TEXT_SHAPE_BUILD_CONFIG="$<CONFIG>")

    # Restore the former direct-mapped policy in the complete current owner.
    # Its deterministic UTF-8 conflicts fail ordinary counter assertions; this
    # control never relies on a timing failure, scheduler noise or owner ASLR.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_text_shape.c")
    file(READ "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_text_shape.c" _shape_owner)
    set(_shape_policy "enum { SHAPE_SLOTS = 256, SHAPE_WAYS = 4 };")
    string(REPLACE "${_shape_policy}" "" _shape_without_policy "${_shape_owner}")
    string(LENGTH "${_shape_owner}" _shape_owner_length)
    string(LENGTH "${_shape_without_policy}" _shape_without_length)
    string(LENGTH "${_shape_policy}" _shape_policy_length)
    math(EXPR _shape_policy_removed "${_shape_owner_length} - ${_shape_without_length}")
    if(NOT _shape_policy_removed EQUAL _shape_policy_length)
        message(FATAL_ERROR "Text-shape control requires one exact production policy")
    endif()
    string(REPLACE "${_shape_policy}" "enum { SHAPE_SLOTS = 256, SHAPE_WAYS = 1 };"
        _shape_old_direct "${_shape_owner}")
    set(_shape_control_path "${CMAKE_CURRENT_BINARY_DIR}/text-shape-controls/old_direct.c")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/text-shape-controls")
    unset(_shape_previous_control)
    if(EXISTS "${_shape_control_path}")
        file(READ "${_shape_control_path}" _shape_previous_control)
    endif()
    if(NOT _shape_previous_control STREQUAL _shape_old_direct)
        file(WRITE "${_shape_control_path}" "${_shape_old_direct}")
    endif()
    jce_add_unit_test(test_jce_text_shape_cache_old_direct KIND contract
        SOURCES test_jce_text_shape_cache.c
        LINK jce_core jce_renderer jce_resource harfbuzz::harfbuzz sdl::sdl)
    target_include_directories(test_jce_text_shape_cache_old_direct PRIVATE
        "${CMAKE_SOURCE_DIR}/engine/src")
    target_compile_definitions(test_jce_text_shape_cache_old_direct PRIVATE
        JCE_BUILDING_ENGINE=1 JCE_TEXT_SHAPE_CONFLICT_CONTROL=1
        JCE_TEST_TEXT_SHAPE_OWNER="${_shape_control_path}")
    set_tests_properties(test_jce_text_shape_cache_old_direct PROPERTIES WILL_FAIL TRUE)
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

# Headless ownership regression. Texture stubs count real staging consumption;
# the production pool is compiled directly without a renderer/device dependency.
jce_add_unit_test(test_jce_model_texture_pool
    SOURCES test_jce_model_texture_pool.c
        "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_model_texture_pool.c"
    LINK jce_core)
target_include_directories(test_jce_model_texture_pool PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# Actual pure mip helper: byte filtering, NPOT tails and checked storage.
jce_add_unit_test(test_jce_texture_mips
    SOURCES test_jce_texture_mips.c
    LINK jce_core)
target_include_directories(test_jce_texture_mips PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# Pure production counter and borrowed-context lifecycle (no graphics device).
jce_add_unit_test(test_jce_upload_metrics
    SOURCES test_jce_upload_metrics.c
        "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_upload_metrics.c"
    LINK jce_core)
target_include_directories(test_jce_upload_metrics PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

jce_add_unit_test(test_jce_upload_payload_integration
    SOURCES test_jce_upload_payload_integration.c
    LINK jce_core jce_renderer jce_resource)

# Exact stock BRDF bytes and custom-size kernel, without a graphics device.
jce_add_unit_test(test_jce_ibl_stock
    SOURCES test_jce_ibl_stock.c LINK jce_core jce_renderer)
target_include_directories(test_jce_ibl_stock PRIVATE "${CMAKE_SOURCE_DIR}/engine/src")

# Production lifecycle adapter plus the same complete central owner already
# extracted for readback controls. The ordinary negative drops only its packet
# call; it retains the real frame call, scope and retirement/upload fan-out.
set(_submission_trace_wire "    jce_renderer_submission_trace_emit(advance, trace_generation,\n        trace_backend_before, s_device.frame_index, trace_ordinal_before);")
string(FIND "${_readback_submission}" "${_submission_trace_wire}" _submission_trace_first)
string(FIND "${_readback_submission}" "${_submission_trace_wire}" _submission_trace_last REVERSE)
if(_submission_trace_first LESS 0 OR NOT _submission_trace_first EQUAL _submission_trace_last)
    message(FATAL_ERROR "Submission trace control requires one actual complete packet wire")
endif()
string(REPLACE "${_submission_trace_wire}"
    "    (void)trace_generation; (void)trace_backend_before; (void)trace_ordinal_before;"
    _submission_trace_missing "${_readback_submission}")
set(_submission_trace_dir "${CMAKE_CURRENT_BINARY_DIR}/submission-trace")
file(MAKE_DIRECTORY "${_submission_trace_dir}")
jce_test_write_if_changed("${_submission_trace_dir}/missing_packet.inc.h" "${_submission_trace_missing}")
foreach(_submission_trace_variant IN ITEMS current missing_submission_packet)
    set(_submission_trace_target test_jce_renderer_trace)
    set(_submission_trace_kind unit)
    set(_submission_trace_owner "${_readback_dir}/submission.inc.h")
    if(_submission_trace_variant STREQUAL "missing_submission_packet")
        string(APPEND _submission_trace_target "_missing_submission_packet")
        set(_submission_trace_kind contract)
        set(_submission_trace_owner "${_submission_trace_dir}/missing_packet.inc.h")
    endif()
    jce_add_unit_test(${_submission_trace_target} KIND ${_submission_trace_kind}
        SOURCES test_jce_renderer_trace.c
            "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer_trace.c"
        LINK jce_core)
    target_include_directories(${_submission_trace_target} PRIVATE "${CMAKE_SOURCE_DIR}/engine/src")
    target_compile_definitions(${_submission_trace_target} PRIVATE
        JCE_TEST_RENDERER_SUBMISSION_OWNER="${_submission_trace_owner}")
    if(_submission_trace_kind STREQUAL "contract")
        set_tests_properties(${_submission_trace_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_submission_trace_var IN ITEMS wire first last missing dir variant target kind owner)
    unset(_submission_trace_${_submission_trace_var})
endforeach()
unset(_submission_trace_var)

# Startup policy/parser plus the full actual apply_transient_limits owner.
# Typed ports do not initialise SDL/bgfx or claim physical memory savings.
set(_renderer_budget_renderer "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer.c")
set(_renderer_budget_header "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer_budget_internal.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_renderer_budget_renderer}" "${_renderer_budget_header}")
file(READ "${_renderer_budget_renderer}" _renderer_budget_source)
set(_renderer_budget_signature "static void apply_transient_limits(bgfx_init_t *init)\n{")
string(FIND "${_renderer_budget_source}" "${_renderer_budget_signature}" _renderer_budget_first)
string(FIND "${_renderer_budget_source}" "${_renderer_budget_signature}" _renderer_budget_last REVERSE)
if(_renderer_budget_first LESS 0 OR NOT _renderer_budget_first EQUAL _renderer_budget_last)
    message(FATAL_ERROR "Renderer budget fixture requires one complete actual apply owner")
endif()
string(SUBSTRING "${_renderer_budget_source}" ${_renderer_budget_first} -1 _renderer_budget_tail)
# Nested branches are indented; the first column-zero closing brace ends this
# function. Preserve every branch, parser, allocator call and startup counter.
string(FIND "${_renderer_budget_tail}" "\n}\n" _renderer_budget_end)
if(_renderer_budget_end LESS 0)
    message(FATAL_ERROR "Renderer budget fixture requires the complete actual function end")
endif()
math(EXPR _renderer_budget_length "${_renderer_budget_end} + 3")
string(SUBSTRING "${_renderer_budget_tail}" 0 ${_renderer_budget_length} _renderer_budget_apply)
string(SUBSTRING "${_renderer_budget_apply}" ${_renderer_budget_end} 3 _renderer_budget_closure)
if(NOT _renderer_budget_closure STREQUAL "\n}\n")
    message(FATAL_ERROR "Renderer budget fixture did not retain its complete function closure")
endif()
set(_renderer_budget_wire "    init->limits.minUniformBufferSize = b.uniform_bytes;\n")
string(FIND "${_renderer_budget_apply}" "${_renderer_budget_wire}" _renderer_budget_first)
string(FIND "${_renderer_budget_apply}" "${_renderer_budget_wire}" _renderer_budget_last REVERSE)
if(_renderer_budget_first LESS 0 OR NOT _renderer_budget_first EQUAL _renderer_budget_last)
    message(FATAL_ERROR "Uniform capacity control requires one actual startup limit wire")
endif()
# Keep the counter and every other policy wire: stale limit/counter disagreement
# must produce ordinary failures from the unchanged runtime assertions.
string(REPLACE "${_renderer_budget_wire}" "" _renderer_budget_missing_uniform_wire
    "${_renderer_budget_apply}")
string(LENGTH "${_renderer_budget_wire}" _renderer_budget_wire_length)
string(LENGTH "${_renderer_budget_missing_uniform_wire}" _renderer_budget_mutant_length)
math(EXPR _renderer_budget_deleted "${_renderer_budget_length} - ${_renderer_budget_mutant_length}")
if(NOT _renderer_budget_deleted EQUAL _renderer_budget_wire_length)
    message(FATAL_ERROR "Uniform capacity control must remove only the single actual limit wire")
endif()
set(_renderer_budget_dir "${CMAKE_CURRENT_BINARY_DIR}/renderer-budget")
file(MAKE_DIRECTORY "${_renderer_budget_dir}")
foreach(_renderer_budget_kind IN ITEMS apply missing_uniform_wire)
    set(_renderer_budget_path "${_renderer_budget_dir}/${_renderer_budget_kind}.inc.h")
    set(_renderer_budget_content "${_renderer_budget_${_renderer_budget_kind}}")
    unset(_renderer_budget_previous)
    if(EXISTS "${_renderer_budget_path}")
        file(READ "${_renderer_budget_path}" _renderer_budget_previous)
    endif()
    if(NOT _renderer_budget_previous STREQUAL _renderer_budget_content)
        file(WRITE "${_renderer_budget_path}" "${_renderer_budget_content}")
    endif()
endforeach()
foreach(_renderer_budget_target IN ITEMS test_jce_renderer_budget
    test_jce_renderer_budget_missing_uniform_wire)
    set(_renderer_budget_kind unit)
    set(_renderer_budget_owner apply)
    if(NOT _renderer_budget_target STREQUAL "test_jce_renderer_budget")
        set(_renderer_budget_kind contract)
        set(_renderer_budget_owner missing_uniform_wire)
    endif()
    jce_add_unit_test(${_renderer_budget_target} KIND ${_renderer_budget_kind}
        SOURCES test_jce_renderer_budget.c INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
    target_compile_definitions(${_renderer_budget_target} PRIVATE
        JCE_RENDERER_BUDGET_APPLY_OWNER="${_renderer_budget_dir}/${_renderer_budget_owner}.inc.h")
endforeach()
set_tests_properties(test_jce_renderer_budget_missing_uniform_wire PROPERTIES WILL_FAIL TRUE)

# Complete stock acquisition profile and shared ShaderSet ownership controls.
include("${CMAKE_CURRENT_SOURCE_DIR}/shader_stock_profile_controls.cmake")

# Actual stock/link owner, provenance KATs and ordinary whole-owner controls.
include("${CMAKE_CURRENT_SOURCE_DIR}/shader_provenance_controls.cmake")

# Actual production offscreen APIs on the bgfx Noop backend, no GPU/window.
include("${CMAKE_CURRENT_SOURCE_DIR}/offscreen_resource_diag_controls.cmake")
jce_add_unit_test(test_jce_offscreen_trace SOURCES test_jce_offscreen_trace.c
    LINK jce_core jce_renderer jce_resource)
target_include_directories(test_jce_offscreen_trace PRIVATE "${CMAKE_SOURCE_DIR}/engine/src")
target_link_libraries(test_jce_offscreen_trace PRIVATE bgfx::bgfx)
if(MSVC)
    target_compile_options(test_jce_offscreen_trace PRIVATE /Zc:preprocessor)
endif()

# Exact callback/PNG writer with GPU request interception and actual file IO.
jce_add_unit_test(test_jce_screenshot_completion
    SOURCES test_jce_screenshot_completion.c
    LINK jce_core SDL3_image::SDL3_image bgfx::bgfx)
target_include_directories(test_jce_screenshot_completion PRIVATE "${CMAKE_SOURCE_DIR}/engine/src")
if(MSVC)
    target_compile_options(test_jce_screenshot_completion PRIVATE /Zc:preprocessor)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/legacy_readback_admission_controls.cmake")

# Complete actual callback vtbl with real trace, TLS consumers and OS threads.
# This never creates a graphics device and does not prove that a packaged bgfx
# library was compiled with its optional CPU profiler callbacks enabled.
jce_add_unit_test(test_jce_bgfx_profiler_trace
    SOURCES test_jce_bgfx_profiler_trace.c
    LINK jce_core SDL3_image::SDL3_image bgfx::bgfx
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
if(MSVC)
    target_compile_options(test_jce_bgfx_profiler_trace PRIVATE /Zc:preprocessor)
endif()

# A complete same-owner negative restores only the three historical no-op
# callback wires. It must compile and fail the unchanged ordinary assertions.
set(_bgfx_cpu_callback_path "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer_bgfx_callback.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_bgfx_cpu_callback_path}")
file(READ "${_bgfx_cpu_callback_path}" _bgfx_cpu_callback_source)
set(_bgfx_cpu_begin_wire "    jce_bgfx_cpu_trace_begin(_name);")
set(_bgfx_cpu_end_wire "    jce_bgfx_cpu_trace_end();")
set(_bgfx_cpu_observe_wire "    jce_bgfx_cpu_profiler_observe_begin();")
foreach(_bgfx_cpu_wire_kind IN ITEMS begin end observe)
    set(_bgfx_cpu_wire "${_bgfx_cpu_${_bgfx_cpu_wire_kind}_wire}")
    string(REPLACE "${_bgfx_cpu_wire}" "" _bgfx_cpu_without_wire "${_bgfx_cpu_callback_source}")
    string(LENGTH "${_bgfx_cpu_callback_source}" _bgfx_cpu_original_length)
    string(LENGTH "${_bgfx_cpu_without_wire}" _bgfx_cpu_without_length)
    string(LENGTH "${_bgfx_cpu_wire}" _bgfx_cpu_wire_length)
    math(EXPR _bgfx_cpu_wire_count "(${_bgfx_cpu_original_length} - ${_bgfx_cpu_without_length}) / ${_bgfx_cpu_wire_length}")
    if(_bgfx_cpu_wire_kind STREQUAL "end")
        set(_bgfx_cpu_expected_count 1)
    else()
        set(_bgfx_cpu_expected_count 2)
    endif()
    if(NOT _bgfx_cpu_wire_count EQUAL _bgfx_cpu_expected_count)
        message(FATAL_ERROR "bgfx CPU callback control requires actual begin/literal/end wires")
    endif()
endforeach()
string(REPLACE "${_bgfx_cpu_begin_wire}" "    (void)_name;"
    _bgfx_cpu_noop_source "${_bgfx_cpu_callback_source}")
string(REPLACE "${_bgfx_cpu_end_wire}" "    (void)_this;"
    _bgfx_cpu_noop_source "${_bgfx_cpu_noop_source}")
string(REPLACE "${_bgfx_cpu_observe_wire}" "    /* historical unobserved callback */"
    _bgfx_cpu_noop_source "${_bgfx_cpu_noop_source}")
set(_bgfx_cpu_control_dir "${CMAKE_CURRENT_BINARY_DIR}/bgfx-cpu-profiler-controls")
file(MAKE_DIRECTORY "${_bgfx_cpu_control_dir}")
set(_bgfx_cpu_noop_path "${_bgfx_cpu_control_dir}/old_noop.c")
unset(_bgfx_cpu_previous)
if(EXISTS "${_bgfx_cpu_noop_path}")
    file(READ "${_bgfx_cpu_noop_path}" _bgfx_cpu_previous)
endif()
if(NOT _bgfx_cpu_previous STREQUAL _bgfx_cpu_noop_source)
    file(WRITE "${_bgfx_cpu_noop_path}" "${_bgfx_cpu_noop_source}")
endif()
jce_add_unit_test(test_jce_bgfx_profiler_trace_old_noop KIND contract
    SOURCES test_jce_bgfx_profiler_trace.c
    LINK jce_core SDL3_image::SDL3_image bgfx::bgfx
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/renderer")
target_compile_definitions(test_jce_bgfx_profiler_trace_old_noop PRIVATE
    JCE_BGFX_PROFILER_CALLBACK_OWNER="${_bgfx_cpu_noop_path}")
if(MSVC)
    target_compile_options(test_jce_bgfx_profiler_trace_old_noop PRIVATE /Zc:preprocessor)
endif()
set_tests_properties(test_jce_bgfx_profiler_trace_old_noop PROPERTIES WILL_FAIL TRUE)


# Generated metadata flows through the complete callback owner and trace ring.
# No graphics device. Debug plus option OFF must still report compiled ON.
foreach(_annotation_case IN ITEMS missing off on debug_off)
    set(_annotation_value -1)
    if(_annotation_case STREQUAL "off")
        set(_annotation_value 0)
    elseif(_annotation_case STREQUAL "on")
        set(_annotation_value 1)
    elseif(_annotation_case STREQUAL "debug_off")
        set(_annotation_value 2)
    endif()
    set(_annotation_target "test_jce_bgfx_annotation_${_annotation_case}")
    jce_add_unit_test(${_annotation_target}
        SOURCES test_jce_bgfx_profiler_trace.c
        LINK jce_core SDL3_image::SDL3_image bgfx::bgfx
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
    target_compile_definitions(${_annotation_target} PRIVATE
        JCE_TEST_ANNOTATION_CASE=${_annotation_value})
    if(MSVC)
        target_compile_options(${_annotation_target} PRIVATE /Zc:preprocessor)
    endif()
endforeach()
foreach(_annotation_control IN ITEMS missing_counter unknown_as_off request_as_compiled)
    if(_annotation_control STREQUAL "missing_counter")
        set(_annotation_wire "        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, \"bgfx.gpu.annotation.compiled\", JCE_RCB_ANNOTATION_COMPILED);")
        set(_annotation_replacement "")
        set(_annotation_value 1)
    elseif(_annotation_control STREQUAL "unknown_as_off")
        set(_annotation_wire "#define JCE_RCB_ANNOTATION_COMPILED (-1)")
        set(_annotation_replacement "#define JCE_RCB_ANNOTATION_COMPILED 0")
        set(_annotation_value -1)
    else()
        set(_annotation_wire "#define JCE_RCB_ANNOTATION_COMPILED 1")
        set(_annotation_replacement "#define JCE_RCB_ANNOTATION_COMPILED JCE_RCB_ANNOTATION_OPTION")
        set(_annotation_value 2)
    endif()
    string(REPLACE "${_annotation_wire}" "" _annotation_removed "${_bgfx_cpu_callback_source}")
    string(LENGTH "${_bgfx_cpu_callback_source}" _annotation_original_length)
    string(LENGTH "${_annotation_removed}" _annotation_removed_length)
    string(LENGTH "${_annotation_wire}" _annotation_wire_length)
    math(EXPR _annotation_count "(${_annotation_original_length} - ${_annotation_removed_length}) / ${_annotation_wire_length}")
    if(NOT _annotation_count EQUAL 1)
        message(FATAL_ERROR "annotation control requires one actual callback owner wire")
    endif()
    string(REPLACE "${_annotation_wire}" "${_annotation_replacement}"
        _annotation_mutant "${_bgfx_cpu_callback_source}")
    set(_annotation_path "${_bgfx_cpu_control_dir}/annotation_${_annotation_control}.c")
    unset(_annotation_previous)
    if(EXISTS "${_annotation_path}")
        file(READ "${_annotation_path}" _annotation_previous)
    endif()
    if(NOT _annotation_previous STREQUAL _annotation_mutant)
        file(WRITE "${_annotation_path}" "${_annotation_mutant}")
    endif()
    set(_annotation_target "test_jce_bgfx_annotation_old_${_annotation_control}")
    jce_add_unit_test(${_annotation_target} KIND contract
        SOURCES test_jce_bgfx_profiler_trace.c
        LINK jce_core SDL3_image::SDL3_image bgfx::bgfx
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/renderer")
    target_compile_definitions(${_annotation_target} PRIVATE
        JCE_TEST_ANNOTATION_CASE=${_annotation_value}
        JCE_BGFX_PROFILER_CALLBACK_OWNER="${_annotation_path}")
    if(MSVC)
        target_compile_options(${_annotation_target} PRIVATE /Zc:preprocessor)
    endif()
    set_tests_properties(${_annotation_target} PROPERTIES WILL_FAIL TRUE)
endforeach()

# Failed attempts consume actual renderer control flow and the complete callback
# owner. Ports replace native work, not the finalization branches under test.
include("${CMAKE_CURRENT_SOURCE_DIR}/bgfx_profiler_attempt_controls.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/pbr_custom_trace_controls.cmake")

# Actual pipeline request/active lifecycle; only caps queries are controlled.
# Isolated CTest processes keep the memoized QA-force parser truthful.
jce_add_unit_test(test_jce_render_pipeline_request_quality
    SOURCES test_jce_render_pipeline_request_quality.c
        "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_render_pipeline.c"
    LINK jce_core jce_resource)
target_include_directories(test_jce_render_pipeline_request_quality PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")
set_tests_properties(test_jce_render_pipeline_request_quality PROPERTIES
    ENVIRONMENT "${JCE_TEST_ENVIRONMENT};JCE_RP_FORCE=")
foreach(_request_force_case IN ITEMS off on numeric)
    set(_request_force_test "test_jce_render_pipeline_request_quality_force_${_request_force_case}")
    add_test(NAME ${_request_force_test}
        COMMAND test_jce_render_pipeline_request_quality "force-${_request_force_case}")
    if(_request_force_case STREQUAL "off")
        set(_request_force_value "stylized_look=0")
    elseif(_request_force_case STREQUAL "on")
        set(_request_force_value "stylized_look=1")
    else()
        set(_request_force_value "render_scale=0.75")
    endif()
    set_tests_properties(${_request_force_test} PROPERTIES
        TIMEOUT 60
        LABELS "unit;renderer" JCE_TEST_KIND unit
        ENVIRONMENT "${JCE_TEST_ENVIRONMENT};JCE_RP_FORCE=${_request_force_value}")
    set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV ${_request_force_test})
endforeach()
unset(_request_force_case)
unset(_request_force_test)
unset(_request_force_value)

add_test(NAME test_jce_render_pipeline_request_quality_cold
    COMMAND test_jce_render_pipeline_request_quality cold)
set_tests_properties(test_jce_render_pipeline_request_quality_cold PROPERTIES
    TIMEOUT 60 LABELS "unit;renderer" JCE_TEST_KIND unit
    ENVIRONMENT "${JCE_TEST_ENVIRONMENT};JCE_RP_FORCE=")
set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV
    test_jce_render_pipeline_request_quality_cold)

# Exact private snapshot emitter and trace core, without third-party headers.
jce_add_unit_test(test_jce_renderer_snapshot
    SOURCES test_jce_renderer_snapshot.c LINK jce_core)
target_include_directories(test_jce_renderer_snapshot PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# Remove only the production global GPU source-frame counter. Same fixture,
# actual trace/export owner; ordinary runtime assertions must reject the gap.
set(_snapshot_header "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer_snapshot_internal.h")
file(READ "${_snapshot_header}" _snapshot_source)
string(REGEX MATCHALL "JCE_RENDER_COUNTER\\(\"gpu\\.source_frame_number\", snapshot->gpu_frame_num\\)"
    _snapshot_id_wires "${_snapshot_source}")
list(LENGTH _snapshot_id_wires _snapshot_id_wire_count)
if(NOT _snapshot_id_wire_count EQUAL 1)
    message(FATAL_ERROR "GPU source-frame mutation requires one actual counter wire")
endif()
string(REPLACE "    JCE_RENDER_COUNTER(\"gpu.source_frame_number\", snapshot->gpu_frame_num);"
    "    (void)snapshot->gpu_frame_num;" _snapshot_mutant "${_snapshot_source}")
if(_snapshot_mutant STREQUAL _snapshot_source)
    message(FATAL_ERROR "GPU source-frame mutation did not change actual owner")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/snapshot-provenance-controls")
set(_snapshot_mutant_header "${CMAKE_CURRENT_BINARY_DIR}/snapshot-provenance-controls/jce_renderer_snapshot_old_source_frame.h")
jce_test_write_if_changed("${_snapshot_mutant_header}" "${_snapshot_mutant}")
jce_add_unit_test(test_jce_renderer_snapshot_old_source_frame
    KIND contract SOURCES test_jce_renderer_snapshot.c LINK jce_core)
target_include_directories(test_jce_renderer_snapshot_old_source_frame PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")
target_compile_definitions(test_jce_renderer_snapshot_old_source_frame PRIVATE
    JCE_TEST_RENDERER_SNAPSHOT_HEADER="${_snapshot_mutant_header}")
set_tests_properties(test_jce_renderer_snapshot_old_source_frame PROPERTIES WILL_FAIL TRUE)
unset(_snapshot_header)
unset(_snapshot_source)
unset(_snapshot_id_wires)
unset(_snapshot_id_wire_count)
unset(_snapshot_mutant)
unset(_snapshot_mutant_header)

# Production queue capacity/encoder boundaries, no window or GPU.
jce_add_unit_test(test_jce_render_queue_capacity
    SOURCES test_jce_render_queue_capacity.c
        "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_instance_upload.c" LINK jce_core)
target_include_directories(test_jce_render_queue_capacity PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")
set_property(TEST test_jce_render_queue_capacity APPEND PROPERTY ENVIRONMENT
    "JCE_PARALLEL_SUBMIT=0" "JCE_RQ_DISABLE_INSTANCING=0")
target_include_directories(test_jce_render_queue_capacity PRIVATE
    "$<TARGET_PROPERTY:bgfx::bgfx,INTERFACE_INCLUDE_DIRECTORIES>")
if(MSVC)
    target_compile_options(test_jce_render_queue_capacity PRIVATE /Zc:preprocessor)
endif()

# Actual impostor TU, isolated backend controls; no renderer-library rebuild.
jce_add_unit_test(test_jce_impostor_upload
    SOURCES test_jce_impostor_upload.c
        "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_instance_upload.c" LINK jce_core)
target_include_directories(test_jce_impostor_upload PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src"
    "$<TARGET_PROPERTY:bgfx::bgfx,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:SDL3_image::SDL3_image,INTERFACE_INCLUDE_DIRECTORIES>")
if(MSVC)
    target_compile_options(test_jce_impostor_upload PRIVATE /Zc:preprocessor)
endif()

# Shared production upload owner with provider-driven headless controls.
jce_add_unit_test(test_jce_instance_upload
    SOURCES test_jce_instance_upload.c
        "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_instance_upload.c" LINK jce_core)
target_include_directories(test_jce_instance_upload PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# Complete production mesh constructor through immutable counted GPU ports.
jce_add_unit_test(test_jce_mesh_create_lifecycle
    SOURCES test_jce_mesh_create_lifecycle.c
    LINK jce_core jce_renderer jce_resource)
target_include_directories(test_jce_mesh_create_lifecycle PRIVATE "${CMAKE_SOURCE_DIR}/engine/src")
target_link_libraries(test_jce_mesh_create_lifecycle PRIVATE bgfx::bgfx)
if(MSVC)
    target_compile_options(test_jce_mesh_create_lifecycle PRIVATE /Zc:preprocessor)
endif()

# Actual SDL_image bytes/pixels and Bundle -> JCEA cooked decode, no GPU.
jce_add_unit_test(test_jce_image_tga
    KIND integration
    SOURCES test_jce_image_tga.c
    LINK jce_core jce_renderer jce_resource
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")

# Exact texture owner once, actual registry/query/removal, counted GPU ports.
# Runtime raw-handle replacement is unsupported, not successful streaming.
jce_add_unit_test(test_jce_texture_residency
    SOURCES test_jce_texture_residency.c
    LINK jce_core jce_renderer jce_resource
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
target_link_libraries(test_jce_texture_residency PRIVATE bgfx::bgfx sdl::sdl)
if(MSVC)
    target_compile_options(test_jce_texture_residency PRIVATE /Zc:preprocessor)
endif()


# Exact first-party allocator observer with real trace/OS atomic ports, no bgfx.
jce_add_unit_test(test_jce_bgfx_allocator_trace
    SOURCES test_jce_bgfx_allocator_trace.c
    LINK jce_core sdl::sdl
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
if(MSVC)
    target_compile_options(test_jce_bgfx_allocator_trace PRIVATE /Zc:preprocessor)
endif()

# Runtime old-FBO-ownership negative control; only the fixture's counted
# provider consumes this exact mutated header. The renderer/Noop owner stays
# unchanged, and no counted fake handle reaches the actual bgfx backend.
set(_offscreen_owner_header "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_offscreen_target_internal.h")
file(READ "${_offscreen_owner_header}" _offscreen_owner_source)
string(REGEX MATCHALL "false, provider->user\\)" _offscreen_owner_markers
    "${_offscreen_owner_source}")
list(LENGTH _offscreen_owner_markers _offscreen_owner_marker_count)
if(NOT _offscreen_owner_marker_count EQUAL 1)
    message(FATAL_ERROR "Offscreen ownership mutant requires one exact production marker")
endif()
string(REPLACE "false, provider->user)" "true, provider->user)"
    _offscreen_old_ownership_source "${_offscreen_owner_source}")
if(_offscreen_old_ownership_source STREQUAL _offscreen_owner_source)
    message(FATAL_ERROR "Offscreen ownership mutation did not change the production owner")
endif()
set(_offscreen_mutant_dir "${CMAKE_CURRENT_BINARY_DIR}/offscreen-ownership-mutant")
file(MAKE_DIRECTORY "${_offscreen_mutant_dir}")
set(_offscreen_mutant_header "${_offscreen_mutant_dir}/jce_offscreen_target_internal_old_fbo_ownership.h")
jce_test_write_if_changed("${_offscreen_mutant_header}" "${_offscreen_old_ownership_source}")
jce_add_unit_test(test_jce_offscreen_trace_old_fbo_ownership
    KIND contract
    SOURCES test_jce_offscreen_trace.c
    LINK jce_core jce_renderer jce_resource
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
target_link_libraries(test_jce_offscreen_trace_old_fbo_ownership PRIVATE bgfx::bgfx)
target_compile_definitions(test_jce_offscreen_trace_old_fbo_ownership PRIVATE
    JCE_TEST_OFFSCREEN_TARGET_INTERNAL_HEADER="${_offscreen_mutant_header}"
    JCE_TEST_OFFSCREEN_ISOLATED_NORMALIZATION=1)
if(MSVC)
    target_compile_options(test_jce_offscreen_trace_old_fbo_ownership PRIVATE /Zc:preprocessor)
endif()
set_tests_properties(test_jce_offscreen_trace_old_fbo_ownership PROPERTIES WILL_FAIL TRUE)

# Exact atomic admission + actual main-thread Noop duplicate/recreate lifecycle.
jce_add_unit_test(test_jce_renderer_admission
    KIND integration
    SOURCES test_jce_renderer_admission.c
    LINK jce_core jce_renderer jce_resource sdl::sdl bgfx::bgfx
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
if(MSVC)
    target_compile_options(test_jce_renderer_admission PRIVATE /Zc:preprocessor)
endif()

# Exact init policy header, actual constructor-prefix wiring and startup logger.
# Counted ports never initialise bgfx, SDL or a GPU. All negatives compile and
# then fail ordinary Unity assertions; they are not source-regex PASS evidence.
set(_display_adapter_header "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer_display_adapter_internal.h")
set(_display_adapter_renderer "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer.c")
set(_display_adapter_log_owner "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer_adapter.inc.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_display_adapter_header}" "${_display_adapter_renderer}" "${_display_adapter_log_owner}")
file(READ "${_display_adapter_header}" _display_adapter_source)
set(_display_adapter_assignment "init->deviceId=state->observation.device_id;")
string(FIND "${_display_adapter_source}" "${_display_adapter_assignment}" _display_adapter_first)
string(FIND "${_display_adapter_source}" "${_display_adapter_assignment}" _display_adapter_last REVERSE)
if(_display_adapter_first LESS 0 OR NOT _display_adapter_first EQUAL _display_adapter_last)
    message(FATAL_ERROR "Display adapter mutant requires one actual device assignment")
endif()
string(REPLACE "${_display_adapter_assignment}" "init->deviceId=0;"
    _display_adapter_old_device "${_display_adapter_source}")
set(_display_adapter_explicit_anchor "    state->effective_vendor=init->vendorId;state->effective_device=init->deviceId;state->applied=false;\n")
string(FIND "${_display_adapter_source}" "${_display_adapter_explicit_anchor}" _display_adapter_first)
string(FIND "${_display_adapter_source}" "${_display_adapter_explicit_anchor}" _display_adapter_last REVERSE)
if(_display_adapter_first LESS 0 OR NOT _display_adapter_first EQUAL _display_adapter_last)
    message(FATAL_ERROR "Display adapter mutant requires one actual request snapshot")
endif()
string(REPLACE "${_display_adapter_explicit_anchor}"
    "${_display_adapter_explicit_anchor}    if(init->vendorId!=BGFX_PCI_ID_NONE || init->deviceId!=0) return false;\n"
    _display_adapter_old_explicit_observation "${_display_adapter_source}")
file(READ "${_display_adapter_renderer}" _display_adapter_renderer_source)
string(FIND "${_display_adapter_renderer_source}" "static bool init_backend_attempt(" _display_adapter_function)
if(_display_adapter_function LESS 0)
    message(FATAL_ERROR "Display adapter fixture requires the actual init attempt owner")
endif()
string(SUBSTRING "${_display_adapter_renderer_source}" ${_display_adapter_function} -1 _display_adapter_attempt)
string(FIND "${_display_adapter_attempt}" "    jce_renderer_caps_api_begin_attempt(" _display_adapter_begin)
string(FIND "${_display_adapter_attempt}" "    apply_transient_limits(&init);" _display_adapter_end)
if(_display_adapter_begin LESS 0 OR _display_adapter_end LESS_EQUAL _display_adapter_begin)
    message(FATAL_ERROR "Display adapter fixture needs the actual init prefix before transient setup")
endif()
math(EXPR _display_adapter_length "${_display_adapter_end} - ${_display_adapter_begin}")
string(SUBSTRING "${_display_adapter_attempt}" ${_display_adapter_begin} ${_display_adapter_length}
    _display_adapter_init_prefix)
set(_display_adapter_wire "    (void)jce_renderer_apply_display_adapter(&init,window,display_adapter,jce_window_observe_display_adapter,jce_window_observe_performance_adapter);\n")
string(FIND "${_display_adapter_init_prefix}" "${_display_adapter_wire}" _display_adapter_first)
string(FIND "${_display_adapter_init_prefix}" "${_display_adapter_wire}" _display_adapter_last REVERSE)
if(_display_adapter_first LESS 0 OR NOT _display_adapter_first EQUAL _display_adapter_last)
    message(FATAL_ERROR "Display adapter fixture requires one actual init policy wire")
endif()
string(REPLACE "${_display_adapter_wire}" "" _display_adapter_missing_wire "${_display_adapter_init_prefix}")
file(READ "${_display_adapter_log_owner}" _display_adapter_log_source)
string(FIND "${_display_adapter_log_source}" "static void log_display_adapter(" _display_adapter_log_begin)
if(_display_adapter_log_begin LESS 0)
    message(FATAL_ERROR "Display adapter fixture requires its actual startup log owner")
endif()
string(SUBSTRING "${_display_adapter_log_source}" ${_display_adapter_log_begin} -1 _display_adapter_startup_log)
set(_display_adapter_dir "${CMAKE_CURRENT_BINARY_DIR}/display-adapter")
file(MAKE_DIRECTORY "${_display_adapter_dir}")
foreach(_display_adapter_kind IN ITEMS old_device old_explicit_observation init_prefix missing_wire startup_log)
    set(_display_adapter_path "${_display_adapter_dir}/${_display_adapter_kind}.inc.h")
    set(_display_adapter_content "${_display_adapter_${_display_adapter_kind}}")
    unset(_display_adapter_previous)
    if(EXISTS "${_display_adapter_path}")
        file(READ "${_display_adapter_path}" _display_adapter_previous)
    endif()
    if(NOT _display_adapter_previous STREQUAL _display_adapter_content)
        file(WRITE "${_display_adapter_path}" "${_display_adapter_content}")
    endif()
endforeach()
foreach(_display_adapter_target IN ITEMS test_jce_renderer_display_adapter
    test_jce_renderer_display_adapter_old_device test_jce_renderer_display_adapter_missing_wire
    test_jce_renderer_display_adapter_old_explicit_observation)
    set(_display_adapter_test_kind unit)
    if(NOT _display_adapter_target STREQUAL "test_jce_renderer_display_adapter")
        set(_display_adapter_test_kind contract)
    endif()
    jce_add_unit_test(${_display_adapter_target} KIND ${_display_adapter_test_kind}
        SOURCES test_jce_renderer_display_adapter.c LINK jce_core jce_platform sdl::sdl
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
    target_include_directories(${_display_adapter_target} PRIVATE
        "$<TARGET_PROPERTY:bgfx::bgfx,INTERFACE_INCLUDE_DIRECTORIES>")
    target_compile_definitions(${_display_adapter_target} PRIVATE
        JCE_RENDERER_DISPLAY_ADAPTER_STARTUP_LOG="${_display_adapter_dir}/startup_log.inc.h")
    if(_display_adapter_target STREQUAL "test_jce_renderer_display_adapter_missing_wire")
        set(_display_adapter_prefix "missing_wire")
    else()
        set(_display_adapter_prefix "init_prefix")
    endif()
    target_compile_definitions(${_display_adapter_target} PRIVATE
        JCE_RENDERER_DISPLAY_ADAPTER_INIT_PREFIX="${_display_adapter_dir}/${_display_adapter_prefix}.inc.h")
    if(MSVC)
        target_compile_options(${_display_adapter_target} PRIVATE /Zc:preprocessor)
    endif()
endforeach()
target_compile_definitions(test_jce_renderer_display_adapter_old_device PRIVATE
    JCE_RENDERER_DISPLAY_ADAPTER_OWNER="${_display_adapter_dir}/old_device.inc.h")
target_compile_definitions(test_jce_renderer_display_adapter_old_explicit_observation PRIVATE
    JCE_RENDERER_DISPLAY_ADAPTER_OWNER="${_display_adapter_dir}/old_explicit_observation.inc.h")
set_tests_properties(test_jce_renderer_display_adapter_old_device
    test_jce_renderer_display_adapter_missing_wire
    test_jce_renderer_display_adapter_old_explicit_observation PROPERTIES WILL_FAIL TRUE)

# Complete texture owner with actual cooked writer/reader/Pak bytes. Counted
# GPU ports establish admission and diagnostics, not driver pixels or support.
set(_texture_failure_owner "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_texture.c")
set(_texture_failure_header "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_texture_diagnostics_internal.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_texture_failure_owner}" "${_texture_failure_header}")
file(READ "${_texture_failure_owner}" _texture_failure_source)
set(_texture_failure_caps_wire "        texture_request_rejected(identity,JCE_TEXTURE_FAIL_CAPS,info,pixel_bytes,need,\n            bgfx_fmt,caps,backend);")
set(_texture_failure_identity_wire "        c->identity     = identity;")
set(_texture_failure_dir "${CMAKE_CURRENT_BINARY_DIR}/texture-failure-controls")
file(MAKE_DIRECTORY "${_texture_failure_dir}")
foreach(_texture_failure_variant IN ITEMS normal silent_caps missing_identity)
    set(_texture_failure_content "${_texture_failure_source}")
    set(_texture_failure_target test_jce_texture_cooked_failure)
    set(_texture_failure_kind unit)
    if(NOT _texture_failure_variant STREQUAL "normal")
        set(_texture_failure_kind contract)
        set(_texture_failure_target "test_jce_texture_cooked_failure_${_texture_failure_variant}")
        if(_texture_failure_variant STREQUAL "silent_caps")
            set(_texture_failure_wire "${_texture_failure_caps_wire}")
        else()
            set(_texture_failure_wire "${_texture_failure_identity_wire}")
        endif()
        string(FIND "${_texture_failure_source}" "${_texture_failure_wire}" _texture_failure_first)
        string(FIND "${_texture_failure_source}" "${_texture_failure_wire}" _texture_failure_last REVERSE)
        if(_texture_failure_first LESS 0 OR NOT _texture_failure_first EQUAL _texture_failure_last)
            message(FATAL_ERROR "Texture failure control requires one exact actual wire")
        endif()
        string(REPLACE "${_texture_failure_wire}" "/* historical omitted wire */"
            _texture_failure_content "${_texture_failure_source}")
    endif()
    set(_texture_failure_path "${_texture_failure_dir}/${_texture_failure_variant}.c")
    unset(_texture_failure_previous)
    if(EXISTS "${_texture_failure_path}")
        file(READ "${_texture_failure_path}" _texture_failure_previous)
    endif()
    if(NOT _texture_failure_previous STREQUAL _texture_failure_content)
        file(WRITE "${_texture_failure_path}" "${_texture_failure_content}")
    endif()
    jce_add_unit_test(${_texture_failure_target} KIND ${_texture_failure_kind}
        SOURCES test_jce_texture_cooked_failure.c LINK jce_core jce_renderer jce_resource bgfx::bgfx sdl::sdl
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/renderer")
    target_compile_definitions(${_texture_failure_target} PRIVATE
        "JCE_TEST_TEXTURE_FAILURE_OWNER=\"${_texture_failure_path}\"")
    if(MSVC)
        target_compile_options(${_texture_failure_target} PRIVATE /Zc:preprocessor)
    endif()
    if(NOT _texture_failure_variant STREQUAL "normal")
        set_tests_properties(${_texture_failure_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()


# Web API admission: exact owner + real version authority + actual post-init tail.
set(_web_api_owner_path "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer_web_api_internal.h")
set(_web_api_renderer_path "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_web_api_owner_path}" "${_web_api_renderer_path}")
file(READ "${_web_api_owner_path}" _web_api_owner)
file(READ "${_web_api_renderer_path}" _web_api_renderer)
string(FIND "${_web_api_renderer}" "    /* Post-init Web API admission uses that exact active context. */" _web_api_tail_start)
string(FIND "${_web_api_renderer}" "\n}\n\n/* Platform-preferred bgfx fallback chain." _web_api_tail_end)
if(_web_api_tail_start LESS 0 OR _web_api_tail_end LESS _web_api_tail_start)
    message(FATAL_ERROR "Web API requires its exact actual renderer post-init tail")
endif()
math(EXPR _web_api_tail_length "${_web_api_tail_end} - ${_web_api_tail_start}")
string(SUBSTRING "${_web_api_renderer}" ${_web_api_tail_start} ${_web_api_tail_length} _web_api_tail)
foreach(_web_api_variant IN ITEMS current old_namespace missing_publish old_epoch_carry missing_init_wire)
    set(_web_api_header "${_web_api_owner}")
    set(_web_api_body "${_web_api_tail}")
    set(_web_api_kind unit)
    if(NOT _web_api_variant STREQUAL "current")
        set(_web_api_kind contract)
        if(_web_api_variant STREQUAL "old_namespace" OR _web_api_variant STREQUAL "missing_publish")
            set(_web_api_needle "    const bool floor = jce_renderer_caps_api_publish_probe(JCE_BACKEND_OPENGLES, api, shading);")
            if(_web_api_variant STREQUAL "old_namespace")
                set(_web_api_replacement "    const bool floor = jce_renderer_caps_api_publish_probe(JCE_BACKEND_OPENGLES, observation.api, shading);")
            else()
                set(_web_api_replacement "    const bool floor = false;")
            endif()
        elseif(_web_api_variant STREQUAL "old_epoch_carry")
            set(_web_api_needle "        observation.epoch_begin == state->epoch && observation.epoch_end == state->epoch)")
            set(_web_api_replacement "        observation.context_id != 0)")
        else()
            set(_web_api_needle "    const bool web_api_ok = from_bgfx_type(type) != JCE_BACKEND_OPENGLES ||\n        jce_renderer_caps_api_admit_web_context();")
            set(_web_api_replacement "    const bool web_api_ok = true;")
        endif()
        if(_web_api_variant STREQUAL "missing_init_wire")
            set(_web_api_before "${_web_api_body}")
        else()
            set(_web_api_before "${_web_api_header}")
        endif()
        string(FIND "${_web_api_before}" "${_web_api_needle}" _web_api_first)
        string(FIND "${_web_api_before}" "${_web_api_needle}" _web_api_last REVERSE)
        if(_web_api_first LESS 0 OR NOT _web_api_first EQUAL _web_api_last)
            message(FATAL_ERROR "Web API ${_web_api_variant} requires one exact owner wire")
        endif()
        string(REPLACE "${_web_api_needle}" "${_web_api_replacement}" _web_api_after "${_web_api_before}")
        if(_web_api_variant STREQUAL "missing_init_wire")
            set(_web_api_body "${_web_api_after}")
        else()
            set(_web_api_header "${_web_api_after}")
        endif()
    endif()
    set(_web_api_header_path "${CMAKE_CURRENT_BINARY_DIR}/jce_web_api_${_web_api_variant}.h")
    set(_web_api_body_path "${CMAKE_CURRENT_BINARY_DIR}/jce_web_api_init_${_web_api_variant}.inc")
    foreach(_web_api_output IN ITEMS header body)
        set(_web_api_output_path "${_web_api_${_web_api_output}_path}")
        set(_web_api_output_text "${_web_api_${_web_api_output}}")
        unset(_web_api_previous)
        if(EXISTS "${_web_api_output_path}")
            file(READ "${_web_api_output_path}" _web_api_previous)
        endif()
        if(NOT _web_api_previous STREQUAL _web_api_output_text)
            file(WRITE "${_web_api_output_path}" "${_web_api_output_text}")
        endif()
    endforeach()
    if(_web_api_variant STREQUAL "current")
        set(_web_api_target test_jce_web_api_admission)
    else()
        set(_web_api_target "test_jce_web_api_admission_${_web_api_variant}")
    endif()
    jce_add_unit_test(${_web_api_target} KIND ${_web_api_kind}
        SOURCES test_jce_web_api_admission.c
        LINK jce_core jce_renderer jce_resource
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/renderer")
    target_include_directories(${_web_api_target} PRIVATE
        "$<TARGET_PROPERTY:bgfx::bgfx,INTERFACE_INCLUDE_DIRECTORIES>")
    if(MSVC)
        target_compile_options(${_web_api_target} PRIVATE /Zc:preprocessor)
    endif()
    target_compile_definitions(${_web_api_target} PRIVATE
        "JCE_WEB_API_OWNER=\"${_web_api_header_path}\""
        "JCE_WEB_API_INIT_TAIL=\"${_web_api_body_path}\"")
    if(NOT _web_api_variant STREQUAL "current")
        set_tests_properties(${_web_api_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_web_api_variable IN ITEMS owner_path renderer_path owner renderer tail_start tail_end tail_length
    tail variant header body kind needle replacement before first last after header_path body_path
    output output_path output_text previous target)
    unset(_web_api_${_web_api_variable})
endforeach()
unset(_web_api_variable)

# Actual Web-context gate and complete central submission owner (CPU ports).
set(_context_submit_path "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer.c")
set(_context_trace_path "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer_trace.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_context_submit_path}" "${_context_trace_path}")
file(READ "${_context_submit_path}" _context_submit_source)
set(_context_submit_begin "bool jce_renderer_frame_admission_current_internal(")
set(_context_submit_end "void jce_renderer_destroy_impl(")
string(FIND "${_context_submit_source}" "${_context_submit_begin}" _context_submit_first)
string(FIND "${_context_submit_source}" "${_context_submit_begin}" _context_submit_first_last REVERSE)
string(FIND "${_context_submit_source}" "${_context_submit_end}" _context_submit_last)
string(FIND "${_context_submit_source}" "${_context_submit_end}" _context_submit_last_last REVERSE)
if(_context_submit_first LESS 0 OR NOT _context_submit_first EQUAL _context_submit_first_last OR
   NOT _context_submit_last EQUAL _context_submit_last_last OR
   NOT _context_submit_last GREATER _context_submit_first)
    message(FATAL_ERROR "Context submission controls require the complete actual shared gate/submission bodies")
endif()
math(EXPR _context_submit_length "${_context_submit_last}-${_context_submit_first}")
string(SUBSTRING "${_context_submit_source}" ${_context_submit_first} ${_context_submit_length} _context_submit_current)
set(_context_submit_wire "    if (!r || r->is_fallback || !jce_renderer_frame_admission_current_internal(r)) {")
string(FIND "${_context_submit_current}" "${_context_submit_wire}" _context_submit_wire_first)
string(FIND "${_context_submit_current}" "${_context_submit_wire}" _context_submit_wire_last REVERSE)
if(_context_submit_wire_first LESS 0 OR NOT _context_submit_wire_first EQUAL _context_submit_wire_last)
    message(FATAL_ERROR "Context submission negative requires one actual central admission wire")
endif()
string(REPLACE "${_context_submit_wire}" "    if (!r || r->is_fallback) {"
    _context_submit_old_gate "${_context_submit_current}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/context-submit")
foreach(_context_submit_variant IN ITEMS current old_gate)
    set(_context_submit_output "${CMAKE_CURRENT_BINARY_DIR}/context-submit/${_context_submit_variant}.inc.h")
    set(_context_submit_content "${_context_submit_${_context_submit_variant}}")
    unset(_context_submit_previous)
    if(EXISTS "${_context_submit_output}")
        file(READ "${_context_submit_output}" _context_submit_previous)
    endif()
    if(NOT _context_submit_previous STREQUAL _context_submit_content)
        file(WRITE "${_context_submit_output}" "${_context_submit_content}")
    endif()
    set(_context_submit_target test_jce_renderer_context_submit)
    set(_context_submit_kind unit)
    if(_context_submit_variant STREQUAL "old_gate")
        string(APPEND _context_submit_target "_old_gate")
        set(_context_submit_kind contract)
    endif()
    jce_add_unit_test(${_context_submit_target} KIND ${_context_submit_kind}
        SOURCES test_jce_renderer_context_submit.c LINK jce_core bgfx::bgfx
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
    if(MSVC)
        target_compile_options(${_context_submit_target} PRIVATE /Zc:preprocessor)
    endif()
    target_compile_definitions(${_context_submit_target} PRIVATE
        JCE_TEST_RENDERER_CONTEXT_SUBMIT_OWNER="${_context_submit_output}")
    if(_context_submit_kind STREQUAL "contract")
        set_tests_properties(${_context_submit_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
# Complete actual lifecycle adapter with its former pointer-only success wire.
file(READ "${_context_trace_path}" _context_trace_current)
set(_context_trace_begin_wire "accepted ? JCE_TRACE_TASK_SUCCEEDED :\n        r && win ? JCE_TRACE_TASK_FAILED : JCE_TRACE_TASK_CANCELLED")
set(_context_trace_end_wire "accepted ? JCE_TRACE_TASK_SUCCEEDED :\n        r ? JCE_TRACE_TASK_FAILED : JCE_TRACE_TASK_CANCELLED")
set(_context_trace_old_success "${_context_trace_current}")
foreach(_context_trace_part IN ITEMS begin end)
    set(_context_trace_wire "${_context_trace_${_context_trace_part}_wire}")
    string(REPLACE "${_context_trace_wire}" "" _context_trace_without "${_context_trace_current}")
    string(LENGTH "${_context_trace_current}" _context_trace_before_length)
    string(LENGTH "${_context_trace_without}" _context_trace_after_length)
    string(LENGTH "${_context_trace_wire}" _context_trace_wire_length)
    math(EXPR _context_trace_removed "${_context_trace_before_length}-${_context_trace_after_length}")
    if(_context_trace_part STREQUAL "begin")
        math(EXPR _context_trace_expected "2*${_context_trace_wire_length}")
        set(_context_trace_old_result "r && win ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_CANCELLED")
    else()
        set(_context_trace_expected ${_context_trace_wire_length})
        set(_context_trace_old_result "r ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_CANCELLED")
    endif()
    if(NOT _context_trace_removed EQUAL _context_trace_expected)
        message(FATAL_ERROR "Context trace negative requires the complete real result-consumption wires")
    endif()
    string(REPLACE "${_context_trace_wire}" "${_context_trace_old_result}"
        _context_trace_old_success "${_context_trace_old_success}")
endforeach()
string(REPLACE "if (accepted && scope.id)" "if (scope.id)"
    _context_trace_old_success "${_context_trace_old_success}")
set(_context_trace_output "${CMAKE_CURRENT_BINARY_DIR}/context-submit/old_success.c")
unset(_context_trace_previous)
if(EXISTS "${_context_trace_output}")
    file(READ "${_context_trace_output}" _context_trace_previous)
endif()
if(NOT _context_trace_previous STREQUAL _context_trace_old_success)
    file(WRITE "${_context_trace_output}" "${_context_trace_old_success}")
endif()
jce_add_unit_test(test_jce_renderer_trace_old_context_success KIND contract
    SOURCES test_jce_renderer_trace.c "${_context_trace_output}" LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/renderer")
target_compile_definitions(test_jce_renderer_trace_old_context_success PRIVATE
    JCE_TEST_RENDERER_SUBMISSION_OWNER="${_readback_dir}/submission.inc.h")
set_tests_properties(test_jce_renderer_trace_old_context_success PROPERTIES WILL_FAIL TRUE)
foreach(_context_submit_var IN ITEMS path source begin end first first_last last last_last length current
    wire wire_first wire_last old_gate variant output content previous target kind)
    unset(_context_submit_${_context_submit_var})
endforeach()
foreach(_context_trace_var IN ITEMS path current wire old_success without before_length after_length
    wire_length removed expected output previous begin_wire end_wire part old_result)
    unset(_context_trace_${_context_trace_var})
endforeach()
unset(_context_submit_var)
unset(_context_trace_var)

# Actual bounded GPU timing observation: complete GPU caps owner and exact
# existing device-context getter. Typed bgfx ports, no real backend/device.
set(_gt_caps_path "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_gpu_caps.c")
set(_gt_context_path "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer.c")
file(READ "${_gt_caps_path}" _gt_caps)
file(READ "${_gt_context_path}" _gt_renderer)
set(_gt_marker "bool jce_renderer_gpu_observation_context_internal(")
string(FIND "${_gt_renderer}" "${_gt_marker}" _gt_first)
if(_gt_first LESS 0)
    message(FATAL_ERROR "GPU timing fixture requires actual context owner")
endif()
string(SUBSTRING "${_gt_renderer}" ${_gt_first} -1 _gt_tail)
string(FIND "${_gt_tail}" "\n#if JCE_PLATFORM_ANDROID" _gt_last)
if(_gt_last LESS 0)
    message(FATAL_ERROR "GPU timing context owner end anchor missing")
endif()
string(SUBSTRING "${_gt_tail}" 0 ${_gt_last} _gt_context)
string(LENGTH "${_gt_marker}" _gt_marker_length)
string(SUBSTRING "${_gt_tail}" ${_gt_marker_length} -1 _gt_after_marker)
string(FIND "${_gt_after_marker}" "${_gt_marker}" _gt_duplicate)
if(NOT _gt_duplicate LESS 0)
    message(FATAL_ERROR "GPU timing context owner must be unique")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_gt_caps_path}" "${_gt_context_path}")
set(_gt_dir "${CMAKE_CURRENT_BINARY_DIR}/gpu-timing-observation")
file(MAKE_DIRECTORY "${_gt_dir}")
foreach(_gt_variant IN ITEMS current missing_available missing_size missing_context old_stats_context)
    set(_gt_owner "${_gt_caps}")
    set(_gt_bridge "${_gt_context}")
    if(_gt_variant STREQUAL "missing_available")
        set(_gt_wire "observation.timer_available = true;")
        set(_gt_replacement "/* omitted actual availability wire */")
    elseif(_gt_variant STREQUAL "missing_size")
        set(_gt_wire "if (!out || out_size < sizeof(*out)) return false;")
        set(_gt_replacement "if (!out) return false;")
    elseif(_gt_variant STREQUAL "old_stats_context")
        set(_gt_wire "if (!jce_renderer_gpu_observation_context_internal(NULL, NULL)) return false;")
        set(_gt_replacement "/* Original unguarded legacy stats lookup. */")
    elseif(_gt_variant STREQUAL "missing_context")
        set(_gt_wire "!jce_renderer_caps_api_web_context_current()")
        set(_gt_replacement "false")
    endif()
    if(NOT _gt_variant STREQUAL "current")
        set(_gt_mutation "${_gt_owner}")
        if(_gt_variant STREQUAL "missing_context")
            set(_gt_mutation "${_gt_bridge}")
        endif()
        string(FIND "${_gt_mutation}" "${_gt_wire}" _gt_wire_first)
        if(_gt_wire_first LESS 0)
            message(FATAL_ERROR "GPU timing ${_gt_variant} requires its actual wire")
        endif()
        string(LENGTH "${_gt_wire}" _gt_wire_length)
        math(EXPR _gt_wire_end "${_gt_wire_first}+${_gt_wire_length}")
        string(SUBSTRING "${_gt_mutation}" ${_gt_wire_end} -1 _gt_wire_tail)
        string(FIND "${_gt_wire_tail}" "${_gt_wire}" _gt_wire_second)
        if(NOT _gt_wire_second LESS 0)
            message(FATAL_ERROR "GPU timing mutation wire must be unique")
        endif()
        string(REPLACE "${_gt_wire}" "${_gt_replacement}" _gt_mutation "${_gt_mutation}")
        if(_gt_variant STREQUAL "missing_context")
            set(_gt_bridge "${_gt_mutation}")
        else()
            set(_gt_owner "${_gt_mutation}")
        endif()
    endif()
    set(_gt_owner_file "${_gt_dir}/${_gt_variant}.c")
    set(_gt_bridge_file "${_gt_dir}/${_gt_variant}-context.inc.h")
    foreach(_gt_output IN ITEMS owner bridge)
        unset(_gt_previous)
        if(EXISTS "${_gt_${_gt_output}_file}")
            file(READ "${_gt_${_gt_output}_file}" _gt_previous)
        endif()
        if(NOT _gt_previous STREQUAL _gt_${_gt_output})
            file(WRITE "${_gt_${_gt_output}_file}" "${_gt_${_gt_output}}")
        endif()
    endforeach()
    set(_gt_target test_jce_gpu_timing_observation)
    set(_gt_kind unit)
    if(NOT _gt_variant STREQUAL "current")
        string(APPEND _gt_target "_${_gt_variant}")
        set(_gt_kind contract)
    endif()
    jce_add_unit_test(${_gt_target} KIND ${_gt_kind}
        SOURCES test_jce_gpu_timing_observation.c LINK jce_core bgfx::bgfx
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/renderer")
    target_compile_definitions(${_gt_target} PRIVATE JCE_BUILDING_ENGINE=1
        JCE_TEST_GPU_CAPS_OWNER="${_gt_owner_file}"
        JCE_TEST_GPU_CONTEXT_OWNER="${_gt_bridge_file}")
    if(MSVC)
        target_compile_options(${_gt_target} PRIVATE /Zc:preprocessor)
    endif()
    if(_gt_kind STREQUAL "contract")
        set_tests_properties(${_gt_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_gt_cleanup IN ITEMS caps_path context_path caps renderer marker first tail last context
    marker_length after_marker duplicate dir variant owner bridge wire replacement mutation wire_first
    wire_length wire_end wire_tail wire_second owner_file bridge_file output previous target kind)
    unset(_gt_${_gt_cleanup})
endforeach()
unset(_gt_cleanup)

# Actual complete camera clone value state + allocation body. This narrowly
# ports tracked allocation for NULL/OOM/retry without changing production APIs.
set(_camera_clone_owner "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_camera.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_camera_clone_owner}")
file(READ "${_camera_clone_owner}" _camera_clone_source)
function(_jce_camera_clone_extract start_marker end_marker output)
    string(FIND "${_camera_clone_source}" "${start_marker}" first)
    string(FIND "${_camera_clone_source}" "${start_marker}" last REVERSE)
    string(FIND "${_camera_clone_source}" "${end_marker}" end)
    string(FIND "${_camera_clone_source}" "${end_marker}" end_last REVERSE)
    if(first LESS 0 OR NOT first EQUAL last OR end LESS_EQUAL first OR NOT end EQUAL end_last)
        message(FATAL_ERROR "Camera clone actual-body bounds must be unique")
    endif()
    math(EXPR length "${end} - ${first}")
    string(SUBSTRING "${_camera_clone_source}" ${first} ${length} content)
    set(${output} "${content}" PARENT_SCOPE)
endfunction()
_jce_camera_clone_extract("struct JceCamera {" "/* Compute forward direction"
    _camera_clone_state)
_jce_camera_clone_extract("JceCamera *jce_camera_clone(const JceCamera *source)"
    "void jce_camera_destroy(JceCamera *cam)" _camera_clone_body)
set(_camera_clone_actual "${_camera_clone_state}\nstatic ${_camera_clone_body}")
set(_camera_clone_needle "    *clone = *source;")
string(FIND "${_camera_clone_actual}" "${_camera_clone_needle}" _camera_clone_first)
string(FIND "${_camera_clone_actual}" "${_camera_clone_needle}" _camera_clone_last REVERSE)
if(_camera_clone_first LESS 0 OR NOT _camera_clone_first EQUAL _camera_clone_last)
    message(FATAL_ERROR "Camera clone full-copy mutation wire must be unique")
endif()
string(REPLACE "${_camera_clone_needle}" "    clone->position = source->position;"
    _camera_clone_old "${_camera_clone_actual}")
set(_camera_clone_dir "${CMAKE_CURRENT_BINARY_DIR}/camera-clone")
file(MAKE_DIRECTORY "${_camera_clone_dir}")
foreach(_camera_clone_variant IN ITEMS actual old)
    set(_camera_clone_content "${_camera_clone_${_camera_clone_variant}}")
    set(_camera_clone_path "${_camera_clone_dir}/${_camera_clone_variant}.inc.h")
    unset(_camera_clone_previous)
    if(EXISTS "${_camera_clone_path}")
        file(READ "${_camera_clone_path}" _camera_clone_previous)
    endif()
    if(NOT _camera_clone_previous STREQUAL _camera_clone_content)
        file(WRITE "${_camera_clone_path}" "${_camera_clone_content}")
    endif()
    if(_camera_clone_variant STREQUAL "actual")
        set(_camera_clone_target test_jce_camera)
    else()
        set(_camera_clone_target test_jce_camera_clone_old_copy)
        jce_add_unit_test(${_camera_clone_target} KIND contract
            SOURCES test_jce_camera.c LINK jce_core jce_renderer)
        set_tests_properties(${_camera_clone_target} PROPERTIES WILL_FAIL TRUE)
    endif()
    target_compile_definitions(${_camera_clone_target} PRIVATE
        JCE_CAMERA_CLONE_OWNER="${_camera_clone_path}")
endforeach()
foreach(_camera_clone_var IN ITEMS owner source state body actual needle first last old dir variant content path previous target)
    unset(_camera_clone_${_camera_clone_var})
endforeach()
unset(_camera_clone_var)

include("${CMAKE_CURRENT_SOURCE_DIR}/renderer_reset_trace_controls.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/ktx_container_controls.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/volfog_shadow_fade_controls.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/volfog_density_march_controls.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/pbr_alpha_order_controls.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/pbr_point_attenuation_controls.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/renderer_perf_snapshot_controls.cmake")

# Actual startup-root helper with typed ports; no device is created here.
set(_shader_dev_owner "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer_shader_dev.inc.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_shader_dev_owner}")
file(READ "${_shader_dev_owner}" _shader_dev_source)
set(_shader_dev_wire "if (root) return shader_load_program_overlay_named(root, pak, vs_base, fs_base);")
string(FIND "${_shader_dev_source}" "${_shader_dev_wire}" _shader_dev_first)
string(FIND "${_shader_dev_source}" "${_shader_dev_wire}" _shader_dev_last REVERSE)
if(_shader_dev_first LESS 0 OR NOT _shader_dev_first EQUAL _shader_dev_last)
    message(FATAL_ERROR "Shader development root control requires one actual overlay routing wire")
endif()
string(REPLACE "${_shader_dev_wire}"
    "if (root) return shader_load_program_named(pak, vs_base, fs_base);"
    _shader_dev_old "${_shader_dev_source}")
set(_shader_dev_dir "${CMAKE_CURRENT_BINARY_DIR}/shader-development-root")
file(MAKE_DIRECTORY "${_shader_dev_dir}")
set(_shader_dev_old_path "${_shader_dev_dir}/old_pak.inc.h")
jce_test_write_if_changed("${_shader_dev_old_path}" "${_shader_dev_old}")
foreach(_shader_dev_variant IN ITEMS current old_pak)
    set(_shader_dev_target test_jce_renderer_shader_dev)
    set(_shader_dev_kind unit)
    set(_shader_dev_selected "${_shader_dev_owner}")
    if(_shader_dev_variant STREQUAL "old_pak")
        string(APPEND _shader_dev_target "_old_pak")
        set(_shader_dev_kind contract)
        set(_shader_dev_selected "${_shader_dev_old_path}")
    endif()
    jce_add_unit_test(${_shader_dev_target} KIND ${_shader_dev_kind}
        SOURCES test_jce_renderer_shader_dev.c LINK jce_core
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
    target_compile_definitions(${_shader_dev_target} PRIVATE
        JCE_TEST_RENDERER_SHADER_DEV_OWNER="${_shader_dev_selected}")
    if(_shader_dev_kind STREQUAL "contract")
        set_tests_properties(${_shader_dev_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_shader_dev_var IN ITEMS owner source wire first last old dir old_path variant target kind selected)
    unset(_shader_dev_${_shader_dev_var})
endforeach()
unset(_shader_dev_var)

include("${CMAKE_CURRENT_LIST_DIR}/render_encoder_abort_controls.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/light_cookie_texture_controls.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/postfx_grade_owner_controls.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/decal_admission_controls.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/optional_stock_admission_controls.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/bgfx_trace_sink_controls.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/gpu_profile_admission_controls.cmake")
