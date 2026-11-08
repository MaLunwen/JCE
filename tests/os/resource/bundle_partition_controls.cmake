# Compile the complete actual pack owner with only its old below-threshold
# omission restored. The unchanged actual-archive assertions must fail normally.
set(_partition_owner "${CMAKE_SOURCE_DIR}/engine/src/resource/jce_bundle_pack.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_partition_owner}")
file(READ "${_partition_owner}" _partition_source)
set(_partition_wire [=[            for (size_t s = 0; s < r->refs.n; ++s) {
                Bundle *local = bv_get(&bundles, r->refs.items[s]);
                if (local) bundle_add_asset(local, r->asset);
            }
]=])
string(FIND "${_partition_source}" "${_partition_wire}" _partition_first)
string(FIND "${_partition_source}" "${_partition_wire}" _partition_last REVERSE)
if(_partition_first LESS 0 OR NOT _partition_first EQUAL _partition_last)
    message(FATAL_ERROR "Expected exactly one actual below-threshold bundle placement wire")
endif()
string(REPLACE "${_partition_wire}" "            /* old ordinary dependency omission */\n"
    _partition_old "${_partition_source}")
set(_partition_generated "${CMAKE_CURRENT_BINARY_DIR}/bundle_partition_old_owner.c")
jce_test_write_if_changed("${_partition_generated}" "${_partition_old}")
jce_add_unit_test(test_jce_bundle_pack_old_partition
    KIND contract
    SOURCES test_jce_bundle_pack.c "${_partition_generated}"
    LINK jce_core jce_resource sdl::sdl cgltf::cgltf cjson::cjson xxHash::xxhash
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/resource")
target_compile_definitions(test_jce_bundle_pack_old_partition PRIVATE JCE_BUILDING_ENGINE=1)
set_tests_properties(test_jce_bundle_pack_old_partition PROPERTIES WILL_FAIL TRUE)
unset(_partition_owner)
unset(_partition_source)
unset(_partition_wire)
unset(_partition_first)
unset(_partition_last)
unset(_partition_old)
unset(_partition_generated)
