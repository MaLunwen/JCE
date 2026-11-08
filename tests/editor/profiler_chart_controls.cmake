# Actual Profiler timing history, graph and series-consumption owner.
# Complete actual state/sample/ring/chart and real series calls; no GPU is created.
set(_profiler_chart_owner "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_panel_profiler.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_profiler_chart_owner}")
file(READ "${_profiler_chart_owner}" _profiler_chart_source)
function(_jce_profiler_chart_extract output start_marker end_marker)
    string(FIND "${_profiler_chart_source}" "${start_marker}" first)
    string(FIND "${_profiler_chart_source}" "${start_marker}" last REVERSE)
    string(FIND "${_profiler_chart_source}" "${end_marker}" end)
    string(FIND "${_profiler_chart_source}" "${end_marker}" end_last REVERSE)
    if(first LESS 0 OR end LESS 0 OR NOT first EQUAL last OR NOT end EQUAL end_last OR end LESS_EQUAL first)
        message(FATAL_ERROR "Profiler chart actual slice markers must each be unique: ${output}")
    endif()
    math(EXPR length "${end} - ${first}")
    string(SUBSTRING "${_profiler_chart_source}" ${first} ${length} content)
    set(${output} "${content}" PARENT_SCOPE)
endfunction()
_jce_profiler_chart_extract(_profiler_chart_state
    "constexpr int   kHistoryLen" "/* Rebuild the per-view row snapshot")
# Signature-neutral so tests-first imports the unchanged production sampler.
set(_profiler_chart_sample_start "void push_sample(float dt_ms)")
string(FIND "${_profiler_chart_source}" "const JceFrameStats *push_sample(float dt_ms)"
    _profiler_chart_sample_start_position)
if(NOT _profiler_chart_sample_start_position LESS 0)
    set(_profiler_chart_sample_start "const JceFrameStats *push_sample(float dt_ms)")
endif()
_jce_profiler_chart_extract(_profiler_chart_refill
    "void refill_view_rows(const JceFrameStats *st)" "${_profiler_chart_sample_start}")
string(REPLACE "    s_prof.view_rows.clear();"
    "    ++chart_ports.refill;\n    s_prof.view_rows.clear();"
    _profiler_chart_refill_counted "${_profiler_chart_refill}")
string(REPLACE "std::snprintf(r.name" "test_counted_view_snprintf(r.name"
    _profiler_chart_refill_counted "${_profiler_chart_refill_counted}")
_jce_profiler_chart_extract(_profiler_chart_samples
    "${_profiler_chart_sample_start}" "/* PlotLines callbacks per series.")
_jce_profiler_chart_extract(_profiler_chart_debugger
    "extern \"C\" void jce_panel_profiler_draw_view_table(void)"
    "extern \"C\" void jce_panel_profiler_request_tab(int idx)")
_jce_profiler_chart_extract(_profiler_chart_rings
    "float ring_idx(int idx)" "void format_bytes")
_jce_profiler_chart_extract(_profiler_chart_draw
    "void draw_area_chart(" "/* View-id ownership conflicts")
_jce_profiler_chart_extract(_profiler_chart_calls
    "    {\n        float cur_cpu" "    /* ── Engine CPU phases")
_jce_profiler_chart_extract(_profiler_chart_cpu_prefix
    "void draw_content(void)" "    /* UI update wall interval and independent backend wait observations. */")
# Keep actual sampling/epoch/admission and all six ring reductions. Count only
# the existing reduction call port; the production calculations remain intact.
string(REPLACE "compute_stats(" "test_counted_compute_stats("
    _profiler_chart_cpu_counted "${_profiler_chart_cpu_prefix}")
set(_profiler_chart_complete "${_profiler_chart_state}\n${_profiler_chart_refill_counted}\n${_profiler_chart_samples}\n${_profiler_chart_rings}\n${_profiler_chart_draw}\nvoid test_actual_profiler_series(float cpu_avg,float cpu_mx,float gpu_avg,float gpu_mx,float w_avg,float w_mx,float v_avg,float v_mx)\n${_profiler_chart_calls}\n")
string(APPEND _profiler_chart_complete
    "\n${_profiler_chart_cpu_counted}    ++chart_ports.presentations;\n}\n${_profiler_chart_debugger}\n")
set(_profiler_chart_dir "${CMAKE_CURRENT_BINARY_DIR}/profiler-chart")
file(MAKE_DIRECTORY "${_profiler_chart_dir}")
foreach(_profiler_chart_variant IN ITEMS current missing_mask missing_gap missing_covered_gate
    old_covered_refill missing_visible_refill missing_debugger_refill)
    set(_profiler_chart_content "${_profiler_chart_complete}")
    if(_profiler_chart_variant STREQUAL "missing_mask")
        set(_profiler_chart_needle "return jce_profiler_history_value(s_prof.gpu_ms[(int)ring_idx(idx)], s_prof.gpu_available[(int)ring_idx(idx)]);")
        set(_profiler_chart_replacement "return jce_profiler_history_value(s_prof.gpu_ms[(int)ring_idx(idx)], true);")
    elseif(_profiler_chart_variant STREQUAL "missing_gap")
        set(_profiler_chart_needle "            if (!jce_profiler_history_known(a) || !jce_profiler_history_known(b)) continue;\n")
        set(_profiler_chart_replacement "")
    elseif(_profiler_chart_variant STREQUAL "missing_covered_gate")
        set(_profiler_chart_needle "    if (jce_editor_game_view_maximized() &&\n        ImGui::GetWindowViewport() == ImGui::GetMainViewport()) {\n        return;\n    }\n")
        set(_profiler_chart_replacement "    /* Former covered CPU presentation still ran. */\n")
    elseif(_profiler_chart_variant STREQUAL "old_covered_refill")
        # Put derived copying back before the presentation guard.
        set(_profiler_chart_return "    return st;\n}")
        string(FIND "${_profiler_chart_content}" "${_profiler_chart_return}" _profiler_chart_first)
        string(FIND "${_profiler_chart_content}" "${_profiler_chart_return}" _profiler_chart_last REVERSE)
        if(_profiler_chart_first LESS 0 OR NOT _profiler_chart_first EQUAL _profiler_chart_last)
            message(FATAL_ERROR "Profiler eager-copy return must be unique")
        endif()
        string(REPLACE "${_profiler_chart_return}"
            "    if (st) refill_view_rows(st);\n${_profiler_chart_return}"
            _profiler_chart_content "${_profiler_chart_content}")
        set(_profiler_chart_needle "    if (frame_stats) refill_view_rows(frame_stats);\n")
        set(_profiler_chart_replacement "")
    elseif(_profiler_chart_variant STREQUAL "missing_visible_refill")
        set(_profiler_chart_needle "    if (frame_stats) refill_view_rows(frame_stats);\n")
        set(_profiler_chart_replacement "")
    elseif(_profiler_chart_variant STREQUAL "missing_debugger_refill")
        set(_profiler_chart_needle "${_profiler_chart_debugger}")
        string(REPLACE "            refill_view_rows(st);\n" ""
            _profiler_chart_replacement "${_profiler_chart_debugger}")
    endif()
    if(NOT _profiler_chart_variant STREQUAL "current")
        string(FIND "${_profiler_chart_content}" "${_profiler_chart_needle}" _profiler_chart_first)
        string(FIND "${_profiler_chart_content}" "${_profiler_chart_needle}" _profiler_chart_last REVERSE)
        if(_profiler_chart_first LESS 0 OR NOT _profiler_chart_first EQUAL _profiler_chart_last)
            message(FATAL_ERROR "Profiler chart ${_profiler_chart_variant} actual wire must be unique")
        endif()
        string(REPLACE "${_profiler_chart_needle}" "${_profiler_chart_replacement}"
            _profiler_chart_content "${_profiler_chart_content}")
    endif()
    set(_profiler_chart_path "${_profiler_chart_dir}/${_profiler_chart_variant}.inc.h")
    unset(_profiler_chart_previous)
    if(EXISTS "${_profiler_chart_path}")
        file(READ "${_profiler_chart_path}" _profiler_chart_previous)
    endif()
    if(NOT _profiler_chart_previous STREQUAL _profiler_chart_content)
        file(WRITE "${_profiler_chart_path}" "${_profiler_chart_content}")
    endif()
    if(_profiler_chart_variant STREQUAL "current")
        set(_profiler_chart_target test_jce_profiler_observation)
    else()
        set(_profiler_chart_target "test_jce_profiler_chart_${_profiler_chart_variant}")
        jce_add_cxx_test(${_profiler_chart_target} KIND contract
            SOURCES test_jce_profiler_observation.cpp)
        set_tests_properties(${_profiler_chart_target} PROPERTIES WILL_FAIL TRUE)
    endif()
    target_compile_features(${_profiler_chart_target} PRIVATE cxx_std_20)
    target_compile_definitions(${_profiler_chart_target} PRIVATE
        JCE_PROFILER_CHART_OWNER="${_profiler_chart_path}")
endforeach()
foreach(_profiler_chart_var IN ITEMS owner source state sample_start sample_start_position refill refill_counted samples rings draw calls debugger cpu_prefix cpu_counted complete dir variant content needle replacement first last path previous target return)
    unset(_profiler_chart_${_profiler_chart_var})
endforeach()
unset(_profiler_chart_var)
