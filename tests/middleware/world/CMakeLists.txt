cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  L4 world-layer unit tests.                                             #
# ----------------------------------------------------------------------- #

jce_add_unit_test(test_jce_time_of_day
    SOURCES test_jce_time_of_day.c
    LINK    jce_core jce_world)

jce_add_unit_test(test_jce_sky
    SOURCES test_jce_sky.c
    LINK    jce_core jce_world)

jce_add_unit_test(test_jce_trigger_volume
    SOURCES test_jce_trigger_volume.c
    LINK    jce_core jce_world)

jce_add_unit_test(test_jce_weather
    SOURCES test_jce_weather.c
    LINK    jce_core jce_world jce_resource)

jce_add_unit_test(test_jce_road_network
    SOURCES test_jce_road_network.c
    LINK    jce_core jce_world)

jce_add_unit_test(test_jce_spawn_manager
    SOURCES test_jce_spawn_manager.c
    LINK    jce_core jce_world)

jce_add_unit_test(test_jce_gas
    SOURCES test_jce_gas.c
    LINK    jce_core jce_world)

# Links the full engine aggregate (jce_runtime_create pulls scene/physics/script).
jce_add_unit_test(test_jce_world_streamer_churn
    SOURCES test_jce_world_streamer_churn.c
    LINK    JCE)

# Atmospheric transmittance: the sunset must be physics, not an authored curve.
jce_add_unit_test(test_jce_atmosphere
    SOURCES test_jce_atmosphere.c
    LINK    jce_core jce_world)

# Sky-derived ambient: SH9 projection replaces hardcoded ambient literals.
jce_add_unit_test(test_jce_sky_sh9
    SOURCES test_jce_sky_sh9.c
    LINK    jce_core jce_world)

# The ToD sky gradient must be a projection of the sky model, not a second
# hand-authored model.
jce_add_unit_test(test_jce_tod_sky_gradient
    SOURCES test_jce_tod_sky_gradient.c
    LINK    jce_core jce_world)

# The ToD snapshot must actually reach consumers: fog colour/density and
# exposure were computed every frame and read by nothing.
jce_add_unit_test(test_jce_tod_publish
    SOURCES test_jce_tod_publish.c
    LINK    jce_core jce_world)

# Precomputed transmittance + multiple-scattering tables.  The property under
# test is AGREEMENT: sampling the table must match evaluating the integral it
# was baked from, because that is what lets the GPU sky and the CPU lighting
# authority read one atmosphere instead of two.
jce_add_unit_test(test_jce_atmosphere_lut
    SOURCES test_jce_atmosphere_lut.c
    LINK    jce_core jce_world)

# The single authoritative environment state (plan section 2).  The value here
# is not the struct but the INVARIANT: advancing is pure and framerate-
# independent, sanitize closes every hole, and the derived accessors are
# functions of the state alone -- so two subsystems reading it agree by
# construction rather than by anyone remembering to keep them in sync.
jce_add_unit_test(test_jce_environment
    SOURCES test_jce_environment.c
    LINK    jce_core jce_world)

# TriggerVolume.fire_stay is a per-trigger STAY opt-in and .tag reaches the
# event payload.  Both directions asserted.
jce_add_unit_test(test_jce_trigger_fire_stay_tag
    SOURCES test_jce_trigger_fire_stay_tag.c
    LINK    jce_core jce_world jce_resource)
