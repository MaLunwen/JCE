# Complete actual brute-force point loop and its actual direct-BRDF helpers.
set(_ppa_owner "${CMAKE_SOURCE_DIR}/engine/shaders/pbr/fs_pbr_main.sh")
set(_ppa_common_owner "${CMAKE_SOURCE_DIR}/engine/shaders/pbr/pbr_common.sh")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_ppa_owner}" "${_ppa_common_owner}")
file(READ "${_ppa_owner}" _ppa_source)
file(READ "${_ppa_common_owner}" _ppa_common_source)
set(_ppa_begin "    // --- Point lights (brute-force, default) ---\n")
set(_ppa_end "    // --- Spot lights ---\n")
set(_ppa_guard "        if (attenuation == 0.0) continue;\n")
set(_ppa_accumulate "        Lo += cookTorranceBRDFWrap(")
foreach(_ppa_anchor IN ITEMS begin end guard)
    string(FIND "${_ppa_source}" "${_ppa_${_ppa_anchor}}" _ppa_${_ppa_anchor}_first)
    string(FIND "${_ppa_source}" "${_ppa_${_ppa_anchor}}" _ppa_${_ppa_anchor}_last REVERSE)
    if(_ppa_${_ppa_anchor}_first LESS 0 OR
       NOT _ppa_${_ppa_anchor}_first EQUAL _ppa_${_ppa_anchor}_last)
        message(FATAL_ERROR "PBR point control needs one actual ${_ppa_anchor} boundary")
    endif()
endforeach()
if(NOT (_ppa_begin_first LESS _ppa_guard_first AND _ppa_guard_first LESS _ppa_end_first))
    message(FATAL_ERROR "PBR point control guard escaped the actual point loop")
endif()
math(EXPR _ppa_length "${_ppa_end_first} - ${_ppa_begin_first}")
string(SUBSTRING "${_ppa_source}" ${_ppa_begin_first} ${_ppa_length} _ppa_current)
string(FIND "${_ppa_current}" "${_ppa_accumulate}" _ppa_accumulate_first)
string(FIND "${_ppa_current}" "${_ppa_accumulate}" _ppa_accumulate_last REVERSE)
if(_ppa_accumulate_first LESS 0 OR NOT _ppa_accumulate_first EQUAL _ppa_accumulate_last)
    message(FATAL_ERROR "PBR point control needs the sole actual BRDF accumulation")
endif()
# Observe the computed radiance without replacing its formula or accumulation.
string(REPLACE "${_ppa_accumulate}"
    "        jce_test_radiance(i, radiance);\n${_ppa_accumulate}"
    _ppa_current "${_ppa_current}")
string(REPLACE "${_ppa_guard}" "" _ppa_old "${_ppa_current}")
string(REPLACE "${_ppa_guard}" "        if (attenuation <= 0.000001) continue;\n"
    _ppa_epsilon "${_ppa_current}")
if(_ppa_current STREQUAL _ppa_old OR _ppa_current STREQUAL _ppa_epsilon)
    message(FATAL_ERROR "PBR point control did not mutate exactly the actual zero guard")
endif()

# Retain the complete actual direct-light helper chain, including wrapped GGX.
set(_ppa_common_begin "#define PI ")
set(_ppa_common_end "// Fresnel-Schlick with roughness for IBL ambient specular\n")
foreach(_ppa_anchor IN ITEMS common_begin common_end)
    string(FIND "${_ppa_common_source}" "${_ppa_${_ppa_anchor}}" _ppa_${_ppa_anchor}_first)
    string(FIND "${_ppa_common_source}" "${_ppa_${_ppa_anchor}}" _ppa_${_ppa_anchor}_last REVERSE)
    if(_ppa_${_ppa_anchor}_first LESS 0 OR
       NOT _ppa_${_ppa_anchor}_first EQUAL _ppa_${_ppa_anchor}_last)
        message(FATAL_ERROR "PBR point control needs one actual ${_ppa_anchor} boundary")
    endif()
endforeach()
if(NOT _ppa_common_begin_first LESS _ppa_common_end_first)
    message(FATAL_ERROR "PBR point actual direct-BRDF boundaries changed order")
endif()
math(EXPR _ppa_length "${_ppa_common_end_first} - ${_ppa_common_begin_first}")
string(SUBSTRING "${_ppa_common_source}" ${_ppa_common_begin_first} ${_ppa_length} _ppa_common)
string(REGEX REPLACE "([0-9]+\\.[0-9]+)" "\\1f" _ppa_common "${_ppa_common}")
set(_ppa_common_path "${CMAKE_CURRENT_BINARY_DIR}/pbr-point-attenuation/common.inc.h")
jce_test_write_if_changed("${_ppa_common_path}" "${_ppa_common}")

foreach(_ppa_variant IN ITEMS current old epsilon)
    # Syntax-only adapters: vector swizzles, float literals and a CPU entry name.
    # The complete original shadow-slot selection and both sample calls remain.
    set(_ppa_body "${_ppa_${_ppa_variant}}")
    string(REPLACE ".xyz" ".xyz()" _ppa_body "${_ppa_body}")
    string(REGEX REPLACE "([0-9]+\\.[0-9]+)" "\\1f" _ppa_body "${_ppa_body}")
    string(CONCAT _ppa_body "void run()\n{\n" "${_ppa_body}" "}\n")
    set(_ppa_${_ppa_variant}_path
        "${CMAKE_CURRENT_BINARY_DIR}/pbr-point-attenuation/${_ppa_variant}.inc.h")
    jce_test_write_if_changed("${_ppa_${_ppa_variant}_path}" "${_ppa_body}")
endforeach()
foreach(_ppa_variant IN ITEMS current old_fullwork epsilon_cutoff)
    set(_ppa_target test_jce_pbr_point_attenuation)
    set(_ppa_selected "${_ppa_current_path}")
    set(_ppa_kind unit)
    if(_ppa_variant STREQUAL "old_fullwork")
        string(APPEND _ppa_target "_old_fullwork")
        set(_ppa_selected "${_ppa_old_path}")
        set(_ppa_kind contract)
    elseif(_ppa_variant STREQUAL "epsilon_cutoff")
        string(APPEND _ppa_target "_epsilon_cutoff")
        set(_ppa_selected "${_ppa_epsilon_path}")
        set(_ppa_kind contract)
    endif()
    jce_add_cxx_test(${_ppa_target} KIND ${_ppa_kind}
        SOURCES test_jce_pbr_point_attenuation.cpp)
    target_compile_features(${_ppa_target} PRIVATE cxx_std_20)
    target_compile_definitions(${_ppa_target} PRIVATE
        JCE_TEST_PBR_POINT_COMMON="${_ppa_common_path}"
        JCE_TEST_PBR_POINT_CURRENT="${_ppa_selected}"
        JCE_TEST_PBR_POINT_REFERENCE="${_ppa_old_path}")
    if(_ppa_kind STREQUAL "contract")
        set_tests_properties(${_ppa_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_ppa_variable IN ITEMS owner common_owner source common_source begin end guard
        accumulate anchor begin_first begin_last end_first end_last guard_first guard_last
        length current accumulate_first accumulate_last old epsilon common_begin common_end
        common_begin_first common_begin_last common_end_first common_end_last common common_path
        variant body current_path old_path epsilon_path target selected kind)
    unset(_ppa_${_ppa_variable})
endforeach()
unset(_ppa_variable)
