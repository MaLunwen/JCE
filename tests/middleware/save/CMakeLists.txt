cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  L3 save-layer unit tests.                                              #
#                                                                         #
#  Pure CPU snapshot framework (versioned section registry).              #
# ----------------------------------------------------------------------- #

jce_add_unit_test(test_jce_snapshot
    SOURCES test_jce_snapshot.c
    LINK    jce_core jce_save)

# Save-migration registry: register a v1->v2->v3 chain, drive it through the
# REAL snapshot load, assert old saves arrive migrated and a missing step is
# reported (not silently corrupt).
jce_add_unit_test(test_jce_save_migration
    SOURCES test_jce_save_migration.c
    LINK    jce_core jce_save)

# The four ACCUMULATED environment values a session earns and cannot re-derive:
# the hour, its monotonic clock, and the wetness/snow integrators.  Case 3 is
# the one a naive round-trip test misses -- restoring the hour without also
# marking the day seeded lets the very next advance re-seed and snap the
# restored save back to the scene's authored start-of-level hour.
jce_add_unit_test(test_jce_save_env_roundtrip
    SOURCES test_jce_save_env_roundtrip.c
    LINK    jce_core jce_save jce_scene jce_resource)
