# Whole real Core owner; resource archive fixtures stay in Engine domain.
set(_pak_bounded_owner "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_filesystem.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pak_bounded_owner}")
file(READ "${_pak_bounded_owner}" _pak_bounded_source)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/pak-bounded")
foreach(_case IN ITEMS current old_host old_limit)
    set(_source "${_pak_bounded_source}")
    set(_target test_jce_filesystem_pak_bounded)
    set(_kind integration)
    if(_case STREQUAL "old_host")
        set(_wire "    if (!fs || !key || !key[0] || !max_bytes) goto done;")
        # Restore only host precedence. Keep the new PAK buffer contract so
        # the ordinary binary-sentinel group never probes an old short buffer.
        set(_replacement "${_wire}\n    if (out_data && PHYSFS_isInit() && PHYSFS_exists(key)) {\n        *out_data = jce_fs_read_all(fs, key, out_size);\n        result = *out_data ? JCE_FS_PAK_READ_OK : JCE_FS_PAK_READ_ABSENT;\n        goto done;\n    }")
    elseif(_case STREQUAL "old_limit")
        set(_wire "if (size > max_bytes || size >= (uint64_t)SIZE_MAX) break;")
        set(_replacement "if (size >= (uint64_t)SIZE_MAX) break;")
    endif()
    if(NOT _case STREQUAL "current")
        string(REPLACE "${_wire}" "${_replacement}" _source "${_source}")
        if(_source STREQUAL _pak_bounded_source)
            message(FATAL_ERROR "PAK bounded ${_case} requires its actual owner wire")
        endif()
        string(APPEND _target "_${_case}")
        set(_kind contract)
    endif()
    set(_path "${CMAKE_CURRENT_BINARY_DIR}/pak-bounded/${_case}.c")
    jce_test_write_if_changed("${_path}" "${_source}")
    jce_add_unit_test(${_target} KIND ${_kind}
        SOURCES test_jce_filesystem_pak_bounded.c "${_path}"
        LINK jce_core jce_resource physfs-static
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src/os/core")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/pak-bounded/${_case}")
    set_tests_properties(${_target} PROPERTIES WORKING_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/pak-bounded/${_case}")
    if(NOT _case STREQUAL "current")
        set_tests_properties(${_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
