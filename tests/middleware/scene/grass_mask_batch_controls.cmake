set(_mask_source "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_grass_mask.c")
function(jce_grass_mask_batch_fixture target kind owner)
    jce_add_unit_test(${target} KIND ${kind}
        SOURCES test_jce_grass_mask_batch.c "${owner}" LINK jce_core
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    target_compile_definitions(${target} PRIVATE
        jce_fs_get_active=grass_mask_test_fs
        jce_fs_get_active_policy=grass_mask_test_policy
        sr_foliage_read_image_blob=grass_mask_test_read
        sr_foliage_blob_free=grass_mask_test_blob_free
        jce_image_decode=grass_mask_test_decode
        jce_image_free=grass_mask_test_image_free)
endfunction()
jce_grass_mask_batch_fixture(test_jce_grass_mask_batch unit "${_mask_source}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_mask_source}")
file(READ "${_mask_source}" _mask_owner)
set(_mask_needle "entry->encoded_size == size && memcmp(entry->encoded, encoded, size) == 0")
string(FIND "${_mask_owner}" "${_mask_needle}" _mask_first)
string(FIND "${_mask_owner}" "${_mask_needle}" _mask_last REVERSE)
if(_mask_first LESS 0 OR NOT _mask_first EQUAL _mask_last)
    message(FATAL_ERROR "Grass mask control requires one exact byte comparison.")
endif()
string(REPLACE "${_mask_needle}" "entry->encoded_size == size" _mask_mutant "${_mask_owner}")
set(_mask_mutant_path "${CMAKE_CURRENT_BINARY_DIR}/jce_sr_grass_mask_omit_bytes.c")
jce_test_write_if_changed("${_mask_mutant_path}" "${_mask_mutant}")
jce_grass_mask_batch_fixture(test_jce_grass_mask_batch_omit_bytes contract "${_mask_mutant_path}")
set_tests_properties(test_jce_grass_mask_batch_omit_bytes PROPERTIES WILL_FAIL TRUE)
