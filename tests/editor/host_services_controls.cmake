# Compile complete actual event, pre-render should_quit and maintenance bodies.
# Typed destinations borrow the existing owners; no copied host loop or GPU owner.
file(READ "${CMAKE_SOURCE_DIR}/editor/src/jce_editor_main.cpp" _editor_host_main)
file(READ "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor.cpp" _editor_host_core)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/editor/src/jce_editor_main.cpp"
    "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor.cpp")
string(REGEX MATCHALL "static void editor_app_event[(]const JceEvent [*]event, void [*]ud[)]"
    _editor_host_event_signatures "${_editor_host_main}")
string(REGEX MATCHALL "static void editor_poll_cpu_services[(]void[)]"
    _editor_host_poll_signatures "${_editor_host_main}")
string(REGEX MATCHALL "static bool editor_should_quit[(]void [*]ud[)]"
    _editor_host_quit_signatures "${_editor_host_main}")
string(REGEX MATCHALL "void jce_editor_tick_cpu_services[(]float dt, bool refresh_ui_clock[)]"
    _editor_host_maintenance_signatures "${_editor_host_core}")
foreach(_editor_host_signatures IN ITEMS _editor_host_event_signatures _editor_host_poll_signatures
        _editor_host_quit_signatures _editor_host_maintenance_signatures)
    list(LENGTH ${_editor_host_signatures} _editor_host_signature_count)
    if(NOT _editor_host_signature_count EQUAL 1)
        message(FATAL_ERROR "Host CPU services require unique actual owner signatures")
    endif()
endforeach()
string(FIND "${_editor_host_main}" "static void editor_app_event(const JceEvent *event, void *ud)" _editor_host_main_begin)
string(FIND "${_editor_host_main}" "/* ── JCE entry point" _editor_host_main_end)
string(FIND "${_editor_host_core}"
    "void jce_editor_tick_cpu_services(float dt, bool refresh_ui_clock)" _editor_host_core_begin)
string(FIND "${_editor_host_core}" "\nvoid jce_editor_update(JceWindow *window)" _editor_host_core_end)
if(_editor_host_main_begin LESS 0 OR _editor_host_main_end LESS_EQUAL _editor_host_main_begin OR
        _editor_host_core_begin LESS 0 OR _editor_host_core_end LESS_EQUAL _editor_host_core_begin)
    message(FATAL_ERROR "Host CPU service actual-body extraction bounds invalid")
endif()
math(EXPR _editor_host_main_size "${_editor_host_main_end} - ${_editor_host_main_begin}")
math(EXPR _editor_host_core_size "${_editor_host_core_end} - ${_editor_host_core_begin}")
string(SUBSTRING "${_editor_host_main}" ${_editor_host_main_begin} ${_editor_host_main_size} _editor_host_main_body)
string(SUBSTRING "${_editor_host_core}" ${_editor_host_core_begin} ${_editor_host_core_size} _editor_host_core_body)
string(CONCAT _editor_host_normal "${_editor_host_core_body}\n${_editor_host_main_body}")
string(REGEX MATCHALL "editor_poll_cpu_services[(][)]" _editor_host_poll_wires "${_editor_host_main_body}")
list(LENGTH _editor_host_poll_wires _editor_host_poll_wire_count)
if(NOT _editor_host_poll_wire_count EQUAL 1)
    message(FATAL_ERROR "Host CPU service missing-wire control requires unique actual should_quit call")
endif()
string(REPLACE "    editor_poll_cpu_services();" "    (void)&editor_poll_cpu_services;"
    _editor_host_old_wire "${_editor_host_normal}")
if(_editor_host_old_wire STREQUAL _editor_host_normal)
    message(FATAL_ERROR "Host CPU service missing-wire mutation did not change owner")
endif()
# Restore only the missing native-close admission in the complete same owner.
set(_editor_host_close_wire "event->type == JCE_EVENT_QUIT || event->type == JCE_EVENT_WINDOW_CLOSE")
string(FIND "${_editor_host_normal}" "${_editor_host_close_wire}" _editor_host_close_first)
string(FIND "${_editor_host_normal}" "${_editor_host_close_wire}" _editor_host_close_last REVERSE)
if(_editor_host_close_first LESS 0 OR NOT _editor_host_close_first EQUAL _editor_host_close_last)
    message(FATAL_ERROR "Host old-quit-only control requires its unique actual close admission")
endif()
string(REPLACE "${_editor_host_close_wire}" "event->type == JCE_EVENT_QUIT"
    _editor_host_old_quit_only "${_editor_host_normal}")
if(_editor_host_old_quit_only STREQUAL _editor_host_normal)
    message(FATAL_ERROR "Host old-quit-only mutation did not change its actual owner")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/host-services-mutation")
foreach(_editor_host_variant IN ITEMS normal old_wire old_quit_only)
    set(_editor_host_body_var "_editor_host_${_editor_host_variant}")
    set(_editor_host_body_path "${CMAKE_CURRENT_BINARY_DIR}/host-services-mutation/jce_editor_host_${_editor_host_variant}.h")
    unset(_editor_host_previous)
    if(EXISTS "${_editor_host_body_path}")
        file(READ "${_editor_host_body_path}" _editor_host_previous)
    endif()
    if(NOT _editor_host_previous STREQUAL ${_editor_host_body_var})
        file(WRITE "${_editor_host_body_path}" "${${_editor_host_body_var}}")
    endif()
    file(READ "${_editor_host_body_path}" _editor_host_written)
    if(NOT "${_editor_host_written}" STREQUAL "${${_editor_host_body_var}}")
        message(FATAL_ERROR "Host compiler input differs from its selected complete owner")
    endif()
endforeach()
foreach(_editor_host_target IN ITEMS test_jce_editor_host_services
        test_jce_editor_host_services_missing_wire test_jce_editor_host_services_old_quit_only)
    if(_editor_host_target STREQUAL "test_jce_editor_host_services")
        set(_editor_host_kind integration)
        set(_editor_host_variant normal)
    elseif(_editor_host_target STREQUAL "test_jce_editor_host_services_missing_wire")
        set(_editor_host_kind contract)
        set(_editor_host_variant old_wire)
    else()
        set(_editor_host_kind contract)
        set(_editor_host_variant old_quit_only)
    endif()
    jce_add_cxx_test(${_editor_host_target} KIND ${_editor_host_kind}
        SOURCES test_jce_editor_host_services.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_test_run.cpp"
        LINK jce_core)
    target_compile_features(${_editor_host_target} PRIVATE cxx_std_20)
    target_compile_definitions(${_editor_host_target} PRIVATE
        JCE_TEST_EDITOR_HOST_BODIES="${CMAKE_CURRENT_BINARY_DIR}/host-services-mutation/jce_editor_host_${_editor_host_variant}.h"
        JCE_TEST_RUNNER_CHILD_EXE="$<TARGET_FILE:test_jce_editor_test_runner>")
    add_dependencies(${_editor_host_target} test_jce_editor_test_runner)
endforeach()
set_tests_properties(test_jce_editor_host_services_missing_wire
    test_jce_editor_host_services_old_quit_only PROPERTIES WILL_FAIL TRUE)
jce_editor_recorder_host_control("${_editor_host_normal}")
