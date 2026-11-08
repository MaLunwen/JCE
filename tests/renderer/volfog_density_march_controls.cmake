set(_vmd_owner "${CMAKE_SOURCE_DIR}/engine/shaders/volfog/fs_volfog.sc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_vmd_owner}")
file(READ "${_vmd_owner}" _vmd_source)
set(_vmd_begin "\n\tfloat transmittance = 1.0;\n")
set(_vmd_end "\tgl_FragColor = vec4(inscatter, transmittance);\n")
set(_vmd_setup "\tbool uniform_medium = height_falloff == 0.0 && !noise_on;\n")
set(_vmd_setup_end "\n\t/* Do NOT unroll.")
set(_vmd_work "\t\tfloat local_density = density;\n")
set(_vmd_body "\t\tif (!uniform_medium)\n\t\t{\n")
set(_vmd_body_end "\t\t}\n\n\t\t/* Sun in-scattering,")
set(_vmd_loop "\t[loop]\n")
set(_vmd_branch "\t\t[branch]\n")
foreach(_vmd_anchor IN ITEMS begin end setup setup_end work body body_end loop branch)
    string(FIND "${_vmd_source}" "${_vmd_${_vmd_anchor}}" _vmd_${_vmd_anchor}_first)
    string(FIND "${_vmd_source}" "${_vmd_${_vmd_anchor}}" _vmd_${_vmd_anchor}_last REVERSE)
    if(_vmd_${_vmd_anchor}_first LESS 0 OR
       NOT _vmd_${_vmd_anchor}_first EQUAL _vmd_${_vmd_anchor}_last)
        message(FATAL_ERROR "Fog march control needs one actual ${_vmd_anchor} boundary")
    endif()
endforeach()
if(NOT (_vmd_begin_first LESS _vmd_setup_first AND
        _vmd_setup_first LESS _vmd_setup_end_first AND
        _vmd_setup_end_first LESS _vmd_work_first AND
        _vmd_work_first LESS _vmd_branch_first AND
        _vmd_branch_first LESS _vmd_body_first AND
        _vmd_body_first LESS _vmd_body_end_first AND
        _vmd_body_end_first LESS _vmd_end_first))
    message(FATAL_ERROR "Fog march control owner boundaries changed order")
endif()
string(LENGTH "${_vmd_end}" _vmd_end_length)
math(EXPR _vmd_length "${_vmd_end_first} + ${_vmd_end_length} - ${_vmd_begin_first}")
string(SUBSTRING "${_vmd_source}" ${_vmd_begin_first} ${_vmd_length} _vmd_current)

# The reference removes only the uniform-medium optimization. Recover its
# original statements from the actual fallback body rather than copying math.
math(EXPR _vmd_length "${_vmd_setup_end_first} - ${_vmd_setup_first}")
string(SUBSTRING "${_vmd_source}" ${_vmd_setup_first} ${_vmd_length} _vmd_setup_block)
string(LENGTH "${_vmd_body}" _vmd_body_length)
math(EXPR _vmd_body_start "${_vmd_body_first} + ${_vmd_body_length}")
math(EXPR _vmd_length "${_vmd_body_end_first} - ${_vmd_body_start}")
string(SUBSTRING "${_vmd_source}" ${_vmd_body_start} ${_vmd_length} _vmd_old_body)
string(REPLACE "\t\t\t" "\t\t" _vmd_old_body "${_vmd_old_body}")
foreach(_vmd_decl IN ITEMS local_density seg Lin)
    set(_vmd_type float)
    if(_vmd_decl STREQUAL "Lin")
        set(_vmd_type vec3)
    endif()
    set(_vmd_assignment "\t\t${_vmd_decl} =")
    string(FIND "${_vmd_old_body}" "${_vmd_assignment}" _vmd_first)
    string(FIND "${_vmd_old_body}" "${_vmd_assignment}" _vmd_last REVERSE)
    if(_vmd_first LESS 0 OR NOT _vmd_first EQUAL _vmd_last)
        message(FATAL_ERROR "Fog march control needs one actual ${_vmd_decl} fallback assignment")
    endif()
    string(REPLACE "${_vmd_assignment}" "\t\t${_vmd_type} ${_vmd_decl} ="
        _vmd_old_body "${_vmd_old_body}")
endforeach()
string(LENGTH "\t\t}\n" _vmd_close_length)
math(EXPR _vmd_length "${_vmd_body_end_first} + ${_vmd_close_length} - ${_vmd_work_first}")
string(SUBSTRING "${_vmd_source}" ${_vmd_work_first} ${_vmd_length} _vmd_work_block)
string(REPLACE "${_vmd_work_block}" "${_vmd_old_body}" _vmd_old "${_vmd_current}")
string(REPLACE "${_vmd_setup_block}" "" _vmd_old "${_vmd_old}")
if(_vmd_old STREQUAL _vmd_current OR _vmd_old MATCHES "uniform_medium|uniform_seg|uniform_Lin")
    message(FATAL_ERROR "Fog march old control did not remove exactly the uniform path")
endif()

foreach(_vmd_variant IN ITEMS current old)
    # Syntax-only adapters: shader swizzles, exact HLSL hints and float literals.
    # No sample, scattering, transmittance or early-exit formula is replaced.
    set(_vmd_adapted "${_vmd_${_vmd_variant}}")
    foreach(_vmd_swizzle IN ITEMS "gl_FragCoord.xy" "u_volfog_color.rgb" "u_volfog_sun_color.rgb")
        string(REPLACE "${_vmd_swizzle}" "${_vmd_swizzle}()" _vmd_adapted "${_vmd_adapted}")
    endforeach()
    string(REPLACE "${_vmd_loop}" "" _vmd_adapted "${_vmd_adapted}")
    string(REPLACE "${_vmd_branch}" "" _vmd_adapted "${_vmd_adapted}")
    string(REPLACE "BRANCH if" "if" _vmd_adapted "${_vmd_adapted}")
    string(REGEX REPLACE "([0-9]+\\.[0-9]+)" "\\1f" _vmd_adapted "${_vmd_adapted}")
    set(_vmd_${_vmd_variant}_path "${CMAKE_CURRENT_BINARY_DIR}/volfog-density-march/${_vmd_variant}.inc.h")
    jce_test_write_if_changed("${_vmd_${_vmd_variant}_path}" "${_vmd_adapted}")
endforeach()
# This slice's language branches surround the stripped [loop]/[branch] hints.
# A second language macro would execute identical CPU semantics.
foreach(_vmd_variant IN ITEMS current old_repeated_exp)
    set(_vmd_target test_jce_volfog_density_march)
    set(_vmd_selected "${_vmd_current_path}")
    set(_vmd_kind unit)
    if(_vmd_variant STREQUAL "old_repeated_exp")
        string(APPEND _vmd_target "_old_repeated_exp")
        set(_vmd_selected "${_vmd_old_path}")
        set(_vmd_kind contract)
    endif()
    jce_add_cxx_test(${_vmd_target} KIND ${_vmd_kind}
        SOURCES test_jce_volfog_density_march.cpp)
    target_compile_definitions(${_vmd_target} PRIVATE
        BGFX_SHADER_LANGUAGE_HLSL=1
        JCE_TEST_VFOG_MARCH_CURRENT="${_vmd_selected}"
        JCE_TEST_VFOG_MARCH_REFERENCE="${_vmd_old_path}")
    if(_vmd_kind STREQUAL "contract")
        set_tests_properties(${_vmd_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_vmd_variable IN ITEMS owner source begin end setup setup_end work body body_end loop branch
        anchor begin_first begin_last end_first end_last setup_first setup_last setup_end_first
        setup_end_last work_first work_last body_first body_last body_end_first body_end_last
        loop_first loop_last branch_first branch_last end_length length current setup_block body_length body_start old_body
        decl type assignment first last close_length work_block old variant adapted swizzle
        current_path old_path target selected kind)
    unset(_vmd_${_vmd_variable})
endforeach()
unset(_vmd_variable)
