# The complete Editor native-module owner against real Windows DLLs/host IO.
# Only the linked-language registration boilerplate is a typed fixture port;
# the C++ registry, image loader, module lifetime and sibling search are real.
if(JCE_PLATFORM_WINDOWS AND TARGET jce_script_vm_cpp AND TARGET jce_script_cpp_module)
    set(_native_image_dir "${CMAKE_CURRENT_BINARY_DIR}/native-module-images")
    set(_native_image_shim "${_native_image_dir}/registration/jce_script_register_linked.h")
    file(MAKE_DIRECTORY "${_native_image_dir}/registration")
    set(_native_image_registration [=[#ifndef JCE_SCRIPT_REGISTER_LINKED_H
#define JCE_SCRIPT_REGISTER_LINKED_H
#define JCE_SCRIPT_LINKED_CPP 1
/* Language registration is outside this image-lifetime fixture. */
static inline int jce_script_register_linked_languages(void (*publish)(void))
{
    if (publish) publish();
    return 0;
}
#endif
]=])
    unset(_native_image_previous)
    if(EXISTS "${_native_image_shim}")
        file(READ "${_native_image_shim}" _native_image_previous)
    endif()
    if(NOT _native_image_previous STREQUAL _native_image_registration)
        file(WRITE "${_native_image_shim}" "${_native_image_registration}")
    endif()

    add_library(jce_editor_native_image_dependency SHARED native_module_image_dependency.c)
    set_target_properties(jce_editor_native_image_dependency PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
        RUNTIME_OUTPUT_DIRECTORY "${_native_image_dir}/fixtures")
    foreach(_native_image_version IN ITEMS 1 2)
        set(_native_image_plugin "jce_editor_native_image_v${_native_image_version}")
        add_library(${_native_image_plugin} MODULE native_module_image_fixture.cpp)
        target_link_libraries(${_native_image_plugin} PRIVATE
            jce_script_cpp_module jce_editor_native_image_dependency)
        target_compile_definitions(${_native_image_plugin} PRIVATE
            JCE_TEST_NATIVE_IMAGE_VERSION=${_native_image_version})
        set_target_properties(${_native_image_plugin} PROPERTIES
            CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS OFF
            RUNTIME_OUTPUT_DIRECTORY "${_native_image_dir}/fixtures"
            LIBRARY_OUTPUT_DIRECTORY "${_native_image_dir}/fixtures")
        jce_configure_test_link_pool(${_native_image_plugin})
    endforeach()
    jce_configure_test_link_pool(jce_editor_native_image_dependency)

    jce_add_cxx_test(test_jce_editor_native_module_images KIND integration
        SOURCES test_jce_editor_native_module_images.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_script_backends.cpp"
        INCLUDES "${_native_image_dir}/registration"
        LINK jce_core jce_platform jce_application jce_script_vm_cpp jce_script_api)
    target_compile_features(test_jce_editor_native_module_images PRIVATE cxx_std_20)
    target_compile_definitions(test_jce_editor_native_module_images PRIVATE
        "JCE_TEST_NATIVE_IMAGE_V1=\"$<TARGET_FILE:jce_editor_native_image_v1>\""
        "JCE_TEST_NATIVE_IMAGE_V2=\"$<TARGET_FILE:jce_editor_native_image_v2>\""
        "JCE_TEST_NATIVE_IMAGE_DEPENDENCY=\"$<TARGET_FILE:jce_editor_native_image_dependency>\""
        "JCE_TEST_NATIVE_IMAGE_DEPENDENCY_NAME=\"$<TARGET_FILE_NAME:jce_editor_native_image_dependency>\""
        "JCE_TEST_NATIVE_IMAGE_SCRIPT_API=\"$<TARGET_FILE:jce_script_api>\""
        "JCE_TEST_NATIVE_IMAGE_SCRIPT_API_NAME=\"$<TARGET_FILE_NAME:jce_script_api>\""
        "JCE_TEST_NATIVE_IMAGE_SCRATCH=\"${_native_image_dir}/test_jce_editor_native_module_images\"")
    add_dependencies(test_jce_editor_native_module_images
        jce_editor_native_image_v1 jce_editor_native_image_v2)
    # The plugin imports the real call-down API. Keep a clean single-target
    # build independent of artifacts left by the separate scripting suite.
    add_custom_command(TARGET test_jce_editor_native_module_images POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:jce_script_api>"
            "$<TARGET_FILE_DIR:test_jce_editor_native_module_images>"
        COMMENT "Staging jce_script_api beside the Editor native-image fixture")
    set_tests_properties(test_jce_editor_native_module_images PROPERTIES
        RUN_SERIAL TRUE ENVIRONMENT "${JCE_TEST_ENVIRONMENT};JCE_SCRIPT_CPP_MODULES=")
    set(_native_image_owner "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_script_backends.cpp")
    set(_native_image_editor "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_native_image_owner}" "${_native_image_editor}")
    file(READ "${_native_image_owner}" _native_image_source)
    set(_native_image_direct_wire "jce_script_vm_cpp_load_library(image.image_path.c_str())")
    string(FIND "${_native_image_source}" "${_native_image_direct_wire}" _native_image_first)
    string(FIND "${_native_image_source}" "${_native_image_direct_wire}" _native_image_last REVERSE)
    if(_native_image_first LESS 0 OR NOT _native_image_first EQUAL _native_image_last)
        message(FATAL_ERROR "Native image control requires one actual image-load wire")
    endif()
    string(REPLACE "${_native_image_direct_wire}" "jce_script_vm_cpp_load_library(abs.c_str())"
        _native_image_old_direct "${_native_image_source}")
    file(READ "${_native_image_editor}" _native_image_editor_source)
    set(_native_image_shutdown_wire "    jce_editor_state_shutdown();\n    jce_editor_script_modules_reload(nullptr);\n    jce_editor_i18n_shutdown();")
    string(FIND "${_native_image_editor_source}" "${_native_image_shutdown_wire}" _native_image_first)
    string(FIND "${_native_image_editor_source}" "${_native_image_shutdown_wire}" _native_image_last REVERSE)
    if(_native_image_first LESS 0 OR NOT _native_image_first EQUAL _native_image_last)
        message(FATAL_ERROR "Native image shutdown control requires one ordered actual state/module/i18n slice")
    endif()
    string(LENGTH "${_native_image_shutdown_wire}" _native_image_length)
    string(SUBSTRING "${_native_image_editor_source}" ${_native_image_first} ${_native_image_length}
        _native_image_shutdown)
    string(REPLACE "    jce_editor_script_modules_reload(nullptr);" "    (void)0;"
        _native_image_missing_shutdown "${_native_image_shutdown}")
    foreach(_native_image_piece IN ITEMS old_direct shutdown missing_shutdown)
        set(_native_image_path "${_native_image_dir}/${_native_image_piece}.inc.h")
        if(_native_image_piece STREQUAL "old_direct")
            set(_native_image_path "${_native_image_dir}/old_direct.cpp")
        endif()
        unset(_native_image_previous)
        if(EXISTS "${_native_image_path}")
            file(READ "${_native_image_path}" _native_image_previous)
        endif()
        if(NOT _native_image_previous STREQUAL _native_image_${_native_image_piece})
            file(WRITE "${_native_image_path}" "${_native_image_${_native_image_piece}}")
        endif()
    endforeach()
    target_compile_definitions(test_jce_editor_native_module_images PRIVATE
        "JCE_TEST_NATIVE_IMAGE_SHUTDOWN=\"${_native_image_dir}/shutdown.inc.h\"")
    get_target_property(_native_image_definitions test_jce_editor_native_module_images COMPILE_DEFINITIONS)
    foreach(_native_image_variant IN ITEMS old_direct missing_shutdown)
        set(_native_image_target "test_jce_editor_native_module_images_${_native_image_variant}")
        set(_native_image_variant_owner "${_native_image_owner}")
        set(_native_image_variant_definitions "${_native_image_definitions}")
        if(_native_image_variant STREQUAL "old_direct")
            set(_native_image_variant_owner "${_native_image_dir}/old_direct.cpp")
        else()
            string(REPLACE "/shutdown.inc.h" "/missing_shutdown.inc.h"
                _native_image_variant_definitions "${_native_image_variant_definitions}")
        endif()
        string(REPLACE "/test_jce_editor_native_module_images\"" "/${_native_image_target}\""
            _native_image_variant_definitions "${_native_image_variant_definitions}")
        jce_add_cxx_test(${_native_image_target} KIND contract
            SOURCES test_jce_editor_native_module_images.cpp "${_native_image_variant_owner}"
            INCLUDES "${_native_image_dir}/registration" "${CMAKE_SOURCE_DIR}/editor/src/core"
            LINK jce_core jce_platform jce_application jce_script_vm_cpp jce_script_api)
        target_compile_features(${_native_image_target} PRIVATE cxx_std_20)
        target_compile_definitions(${_native_image_target} PRIVATE ${_native_image_variant_definitions})
        add_dependencies(${_native_image_target} jce_editor_native_image_v1 jce_editor_native_image_v2)
        add_custom_command(TARGET ${_native_image_target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:jce_script_api>"
                "$<TARGET_FILE_DIR:${_native_image_target}>")
        set_tests_properties(${_native_image_target} PROPERTIES WILL_FAIL TRUE
            RUN_SERIAL TRUE ENVIRONMENT "${JCE_TEST_ENVIRONMENT};JCE_SCRIPT_CPP_MODULES=")
    endforeach()
    foreach(_native_image_var IN ITEMS dir shim registration previous version plugin owner editor source
        direct_wire first last old_direct editor_source shutdown_wire length shutdown missing_shutdown piece path
        definitions variant target variant_owner variant_definitions)
        unset(_native_image_${_native_image_var})
    endforeach()
    unset(_native_image_var)
else()
    jce_test_suite_skipped("editor-native-module-images"
        "requires Windows and the real C++ script module/registry targets")
endif()
