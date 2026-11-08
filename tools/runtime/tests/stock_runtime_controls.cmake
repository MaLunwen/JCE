# Included by the stock SDK consumer's explicit CPU verification target.
# Full production sources/header are mutated; no runtime production test mode.
set(_stock_control_dir "${CMAKE_CURRENT_BINARY_DIR}/stock-control-sources")
file(MAKE_DIRECTORY "${_stock_control_dir}")
set(_stock_payload_owner "${CMAKE_CURRENT_LIST_DIR}/../stock_payload.c")
set(_stock_policy_owner "${CMAKE_CURRENT_LIST_DIR}/../../../engine/include/jce/application/jce_default_main.inc.h")
# In a relocated source/SDK consumer the caller may pass the exact producer
# public header explicitly. No fallback to an unrelated installed SDK.
if(DEFINED JCE_STOCK_POLICY_SOURCE_HEADER)
    set(_stock_policy_owner "${JCE_STOCK_POLICY_SOURCE_HEADER}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_stock_payload_owner}" "${_stock_policy_owner}")
file(READ "${_stock_payload_owner}" _stock_payload_current)
file(READ "${_stock_policy_owner}" _stock_policy_current)
function(_stock_exact_change OUT CURRENT BEFORE AFTER)
    string(FIND "${CURRENT}" "${BEFORE}" _first)
    string(FIND "${CURRENT}" "${BEFORE}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "Stock ordinary control requires one exact production source wire")
    endif()
    string(REPLACE "${BEFORE}" "${AFTER}" _changed "${CURRENT}")
    set(${OUT} "${_changed}" PARENT_SCOPE)
endfunction()
function(_stock_exact_mutant OUT CURRENT BEFORE AFTER)
    _stock_exact_change(_changed "${CURRENT}" "${BEFORE}" "${AFTER}")
    file(GENERATE OUTPUT "${OUT}" CONTENT "${_changed}")
endfunction()
_stock_exact_mutant("${_stock_control_dir}/old-payload.inc.h" "${_stock_payload_current}"
    "read64(footer + 40) != jce_fnv1a64(bytes, (size_t)host) ||\n        read64(footer + 48) != jce_fnv1a64(bytes + (size_t)host,\n                                         (size_t)bundle)"
    "read64(footer + 40) != jce_fnv1a64(bytes, (size_t)host)")
# Remove both independent security gates. A plain, otherwise valid public
# archive must then decode and fail the fixture's unchanged refusal assertion.
_stock_exact_change(_stock_old_secure "${_stock_payload_current}"
    "    if (!archive || !jce_archive_is_secure(archive) ||\n        !jce_archive_is_authenticated(archive) ||\n        jce_archive_auth_status(archive) != JCE_ARCHIVE_AUTH_VALID ||\n        !jce_archive_verify_header(archive)) goto done;"
    "    if (!archive || !jce_archive_verify_header(archive)) goto done;")
_stock_exact_change(_stock_old_secure "${_stock_old_secure}"
    "    if (!entry || (entry->entry_flags &\n            (JCE_ARCHIVE_ENTRY_ENCRYPTED | JCE_ARCHIVE_ENTRY_AUTHENTICATED)) !=\n            (JCE_ARCHIVE_ENTRY_ENCRYPTED | JCE_ARCHIVE_ENTRY_AUTHENTICATED) ||\n"
    "    if (!entry ||\n")
file(GENERATE OUTPUT "${_stock_control_dir}/old-secure.inc.h" CONTENT "${_stock_old_secure}")
# The header's fixed one-entry budget and public count check implement the same
# uniqueness requirement; remove both for an effective ordinary extra-entry wire.
_stock_exact_change(_stock_old_unique "${_stock_payload_current}"
    "        read32(header + 12) != 1 || read64(header + 32) != 32 ||\n" "")
_stock_exact_change(_stock_old_unique "${_stock_old_unique}"
    "    if (jce_archive_count(archive) != 1) goto done;"
    "    /* Former admission accepted additional authenticated entries. */")
file(GENERATE OUTPUT "${_stock_control_dir}/old-unique.inc.h" CONTENT "${_stock_old_unique}")
_stock_exact_mutant("${_stock_control_dir}/old-overlay.inc.h" "${_stock_policy_current}"
    "jce_fs_set_active_policy(s_runtime_fs, JCE_FS_ACTIVE_ISOLATED);"
    "jce_fs_set_active(s_runtime_fs);")
_stock_exact_mutant("${_stock_control_dir}/old-scene.inc.h" "${_stock_policy_current}"
    "if (!scene_path || !JCE_DEFAULT_MAIN_PACKAGED_SCENE_ALLOWED(scene_path)) {\n        LOG_ERROR(\"app\", \"%s\", \"startup scene is outside the verified stock Bundle\");\n        return false;\n    }"
    "if (!scene_path) return false;")
foreach(_stock_variant IN ITEMS payload secure unique)
    if(_stock_variant STREQUAL "payload")
        set(_stock_name jce_stock_payload_old)
    else()
        set(_stock_name "jce_stock_payload_old_${_stock_variant}")
    endif()
    add_executable(${_stock_name} EXCLUDE_FROM_ALL tests/test_stock_payload.c)
    target_include_directories(${_stock_name} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
    target_link_libraries(${_stock_name} PRIVATE JCE::Core)
    target_compile_definitions(${_stock_name} PRIVATE
        JCE_STOCK_PAYLOAD_OWNER="${_stock_control_dir}/old-${_stock_variant}.inc.h")
    add_test(NAME ${_stock_name} COMMAND ${_stock_name})
    set_tests_properties(${_stock_name} PROPERTIES WILL_FAIL TRUE)
endforeach()
foreach(_stock_variant IN ITEMS overlay scene)
    set(_stock_name "jce_stock_policy_old_${_stock_variant}")
    add_executable(${_stock_name} EXCLUDE_FROM_ALL tests/test_stock_policy.c stock_bundle.c)
    target_link_libraries(${_stock_name} PRIVATE JCE::Core)
    target_compile_definitions(${_stock_name} PRIVATE
        JCE_STOCK_POLICY_OWNER="${_stock_control_dir}/old-${_stock_variant}.inc.h")
    add_test(NAME ${_stock_name} COMMAND ${_stock_name})
    set_tests_properties(${_stock_name} PROPERTIES WILL_FAIL TRUE)
endforeach()
