# Complete private encoder owner; removing only its failed-preparation retirement
# must compile and fail the same ordinary assertions as the current owner passes.
set(_encoder_abort_owner "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_render_encoder.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_encoder_abort_owner}")
file(READ "${_encoder_abort_owner}" _encoder_abort_current)
set(_encoder_abort_wire "        if (explicit_encoder) bgfx_encoder_discard(explicit_encoder, BGFX_DISCARD_ALL);\n        else bgfx_discard(BGFX_DISCARD_ALL);")
string(FIND "${_encoder_abort_current}" "${_encoder_abort_wire}" _encoder_abort_first)
string(FIND "${_encoder_abort_current}" "${_encoder_abort_wire}" _encoder_abort_last REVERSE)
if(_encoder_abort_first LESS 0 OR NOT _encoder_abort_first EQUAL _encoder_abort_last)
    message(FATAL_ERROR "Encoder abort control requires one actual prepared-draw retirement wire")
endif()
string(REPLACE "${_encoder_abort_wire}" "        (void)explicit_encoder; /* Old rejected-submit return. */"
    _encoder_abort_old "${_encoder_abort_current}")
set(_encoder_abort_dir "${CMAKE_CURRENT_BINARY_DIR}/render-encoder-abort")
file(MAKE_DIRECTORY "${_encoder_abort_dir}")
foreach(_encoder_abort_variant IN ITEMS current missing_abort)
    set(_encoder_abort_target test_jce_render_encoder_abort)
    set(_encoder_abort_kind unit)
    set(_encoder_abort_include "${_encoder_abort_owner}")
    if(_encoder_abort_variant STREQUAL "missing_abort")
        string(APPEND _encoder_abort_target "_missing_abort")
        set(_encoder_abort_kind contract)
        set(_encoder_abort_include "${_encoder_abort_dir}/missing_abort.inc.h")
        jce_test_write_if_changed("${_encoder_abort_include}" "${_encoder_abort_old}")
    endif()
    jce_add_unit_test(${_encoder_abort_target} KIND ${_encoder_abort_kind}
        SOURCES test_jce_render_encoder_abort.c
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/renderer"
            "$<TARGET_PROPERTY:bgfx::bgfx,INTERFACE_INCLUDE_DIRECTORIES>")
    target_compile_definitions(${_encoder_abort_target} PRIVATE
        "JCE_TEST_RENDER_ENCODER_OWNER=\"${_encoder_abort_include}\"")
    if(MSVC)
        target_compile_options(${_encoder_abort_target} PRIVATE /Zc:preprocessor)
    endif()
    if(_encoder_abort_variant STREQUAL "missing_abort")
        set_tests_properties(${_encoder_abort_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_encoder_abort_variable IN ITEMS owner current wire first last old dir
    variant target kind include)
    unset(_encoder_abort_${_encoder_abort_variable})
endforeach()
unset(_encoder_abort_variable)
