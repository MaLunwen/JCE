# Restore only the production quota-dependent mesh-face selection. The same
# native Bullet reader fixture must compile and fail ordinary stability checks.
set(_debug_mesh_owner "${CMAKE_SOURCE_DIR}/engine/src/middleware/physics/jce_physics_debug_mesh.hpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_debug_mesh_owner}")
file(READ "${_debug_mesh_owner}" _debug_mesh_source)
set(_debug_mesh_needle "? part.triangles / 2u : sequence.next();")
string(FIND "${_debug_mesh_source}" "${_debug_mesh_needle}" _debug_mesh_first)
string(FIND "${_debug_mesh_source}" "${_debug_mesh_needle}" _debug_mesh_last REVERSE)
if(_debug_mesh_first LESS 0 OR NOT _debug_mesh_first EQUAL _debug_mesh_last)
    message(FATAL_ERROR "Physics mesh control requires one actual face-selection wire")
endif()
string(REPLACE "${_debug_mesh_needle}"
    "? part.triangles / 2u : (static_cast<void>(sequence), ordinal(n, part.triangles, quota));"
    _debug_mesh_old "${_debug_mesh_source}")
set(_debug_mesh_old_owner "${CMAKE_CURRENT_BINARY_DIR}/physics_debug_mesh_old_quota.hpp")
jce_test_write_if_changed("${_debug_mesh_old_owner}" "${_debug_mesh_old}")
jce_add_cxx_test(test_jce_physics_debug_mesh_old_quota KIND contract
    SOURCES test_jce_physics_debug_mesh.cpp
    LINK jce_core jce_physics Bullet::Bullet
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
target_compile_definitions(test_jce_physics_debug_mesh_old_quota PRIVATE
    JCE_PHYSICS_DEBUG_MESH_OWNER="${_debug_mesh_old_owner}")
set_tests_properties(test_jce_physics_debug_mesh_old_quota PROPERTIES WILL_FAIL TRUE)
foreach(_debug_mesh_var IN ITEMS owner source needle first last old old_owner)
    unset(_debug_mesh_${_debug_mesh_var})
endforeach()
unset(_debug_mesh_var)
