# The complete actual sampler owner with only the historical roster cap restored.
set(_sampler_capacity_owner "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_sampler.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sampler_capacity_owner}")
file(READ "${_sampler_capacity_owner}" _sampler_capacity_source)
set(_sampler_capacity_wire "#define SAMP_MAX_THREADS 128u")
string(REPLACE "${_sampler_capacity_wire}" "" _sampler_capacity_without "${_sampler_capacity_source}")
string(LENGTH "${_sampler_capacity_source}" _sampler_capacity_before)
string(LENGTH "${_sampler_capacity_without}" _sampler_capacity_after)
string(LENGTH "${_sampler_capacity_wire}" _sampler_capacity_width)
math(EXPR _sampler_capacity_removed "${_sampler_capacity_before} - ${_sampler_capacity_after}")
if(NOT _sampler_capacity_removed EQUAL _sampler_capacity_width)
    message(FATAL_ERROR "sampler capacity control requires exactly one current roster cap")
endif()
string(REPLACE "${_sampler_capacity_wire}" "#define SAMP_MAX_THREADS 64u"
    _sampler_capacity_source "${_sampler_capacity_source}")
set(_sampler_capacity_selected "${CMAKE_CURRENT_BINARY_DIR}/sampler-capacity/old_cap64.c")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sampler-capacity")
jce_test_write_if_changed("${_sampler_capacity_selected}" "${_sampler_capacity_source}")
jce_add_unit_test(test_jce_sampler_ownership_old_cap64 KIND contract
    SOURCES test_jce_sampler_ownership.c LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_CURRENT_SOURCE_DIR}/stubs")
target_compile_definitions(test_jce_sampler_ownership_old_cap64 PRIVATE
    JCE_TEST_SAMPLER_OWNER="${_sampler_capacity_selected}")
set_tests_properties(test_jce_sampler_ownership_old_cap64 PROPERTIES WILL_FAIL TRUE)
foreach(_sampler_capacity_key IN ITEMS owner source wire without before after width removed selected)
    unset(_sampler_capacity_${_sampler_capacity_key})
endforeach()
unset(_sampler_capacity_key)
