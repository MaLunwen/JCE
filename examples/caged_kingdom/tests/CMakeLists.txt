# Consumer integration: load the authored Caged Kingdom lightning laboratory,
# resolve its loose Lua asset through the standard runtime resolver, and prove
# that the real component-reflection bridge animates leader/return line data.
jce_add_unit_test(test_ck_lightning_lab_runtime
    SOURCES test_ck_lightning_lab_runtime.c
    LINK    JCE)
target_compile_definitions(test_ck_lightning_lab_runtime PRIVATE
    CK_LIGHTNING_SCENE_PATH="${CMAKE_SOURCE_DIR}/examples/caged_kingdom/resources/assets/scenes/lightning_lab.scene.json"
    CK_LIGHTNING_ASSET_ROOT="${CMAKE_SOURCE_DIR}/examples/caged_kingdom/resources/assets")

# Consumer dogfood: the caged_kingdom lightning laboratory is a real
# scene-bound Lua director.  Keep the algorithm headless-testable so its
# staged leader, return stroke, and distance-delayed thunder cannot drift
# between editor Play and the shipped runtime.
jce_add_unit_test(test_ck_lightning_lab
    SOURCES test_ck_lightning_lab.c
    LINK    jce_core jce_script)
target_compile_definitions(test_ck_lightning_lab PRIVATE
    CK_LIGHTNING_SCRIPT_PATH="${CMAKE_SOURCE_DIR}/examples/caged_kingdom/resources/assets/scripts/lightning_lab.lua")

# Consumer dogfood: CK's scientific black-hole lab owns all Kerr equations and
# parameter semantics. The generic Lua host is mocked here so config rejection,
# ABI packing, presets and interaction remain deterministic and headless.
jce_add_unit_test(test_ck_black_hole_lab
    SOURCES test_ck_black_hole_lab.c
    LINK    jce_core jce_script)
target_compile_definitions(test_ck_black_hole_lab PRIVATE
    CK_BLACK_HOLE_SCRIPT_PATH="${CMAKE_SOURCE_DIR}/examples/caged_kingdom/resources/assets/scripts/black_hole_lab.lua")
