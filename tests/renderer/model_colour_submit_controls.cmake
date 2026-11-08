# Execute the complete actual model owner, including its four colour producers.
set(_model_submit_owner "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_model.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_model_submit_owner}")
file(READ "${_model_submit_owner}" _model_submit_current)
set(_model_submit_direct "jce_enc_submit_implicit(view_id, bgfx_prog, 0, BGFX_DISCARD_ALL);")
# Match the call without its semicolon, which CMake treats as a list separator.
string(REGEX MATCHALL "jce_enc_submit_implicit\\(view_id, bgfx_prog, 0, BGFX_DISCARD_ALL\\)"
    _model_submit_direct_matches "${_model_submit_current}")
list(LENGTH _model_submit_direct_matches _model_submit_direct_count)
if(NOT _model_submit_direct_count EQUAL 3)
    message(FATAL_ERROR "Model colour submit control requires exactly three actual implicit colour submit wires")
endif()
set(_model_submit_indirect "jce_enc_submit_indirect_implicit(view_id, bgfx_prog, ind, indirect_el, 1, 0,\n                                             BGFX_DISCARD_ALL);")
string(FIND "${_model_submit_current}" "${_model_submit_indirect}" _model_submit_first)
string(FIND "${_model_submit_current}" "${_model_submit_indirect}" _model_submit_last REVERSE)
if(_model_submit_first LESS 0 OR NOT _model_submit_first EQUAL _model_submit_last)
    message(FATAL_ERROR "Model colour submit control requires one exact actual indirect colour submit wire")
endif()
string(REPLACE "${_model_submit_direct}" "bgfx_submit(view_id, bgfx_prog, 0, BGFX_DISCARD_ALL);"
    _model_submit_old_raw "${_model_submit_current}")
string(REPLACE "${_model_submit_indirect}"
    "bgfx_submit_indirect(view_id, bgfx_prog, ind, indirect_el, 1, 0,\n                                 BGFX_DISCARD_ALL);"
    _model_submit_old_raw "${_model_submit_old_raw}")
set(_model_submit_dir "${CMAKE_CURRENT_BINARY_DIR}/model-colour-submit")
file(MAKE_DIRECTORY "${_model_submit_dir}")
foreach(_model_submit_variant IN ITEMS current old_raw_model)
    set(_model_submit_target test_jce_model_colour_submit)
    set(_model_submit_kind unit)
    set(_model_submit_include "${_model_submit_owner}")
    if(_model_submit_variant STREQUAL "old_raw_model")
        string(APPEND _model_submit_target "_old_raw_model")
        set(_model_submit_kind contract)
        set(_model_submit_include "${_model_submit_dir}/old_raw_model.inc.c")
        jce_test_write_if_changed("${_model_submit_include}" "${_model_submit_old_raw}")
    endif()
    jce_add_unit_test(${_model_submit_target} KIND ${_model_submit_kind}
        SOURCES test_jce_model_colour_submit.c
            "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_instance_upload.c"
        LINK jce_core
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/renderer"
            "$<TARGET_PROPERTY:bgfx::bgfx,INTERFACE_INCLUDE_DIRECTORIES>")
    target_compile_definitions(${_model_submit_target} PRIVATE
        "JCE_TEST_MODEL_COLOUR_OWNER=\"${_model_submit_include}\"")
    if(MSVC)
        target_compile_options(${_model_submit_target} PRIVATE /Zc:preprocessor)
    endif()
    if(_model_submit_variant STREQUAL "old_raw_model")
        set_tests_properties(${_model_submit_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_model_submit_variable IN ITEMS owner current direct direct_matches direct_count indirect
    first last old_raw dir variant target kind include)
    unset(_model_submit_${_model_submit_variable})
endforeach()
unset(_model_submit_variable)
