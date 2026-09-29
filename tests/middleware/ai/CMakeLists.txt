cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  L3 AI-layer pure-logic unit tests.                                     #
#                                                                         #
#  Steering forces and graph A* are pure CPU math with no external        #
#  dependencies (BehaviorTree.CPP / Recast skipped).                      #
# ----------------------------------------------------------------------- #

jce_add_unit_test(test_jce_steering
    SOURCES test_jce_steering.c
    LINK    jce_core jce_ai)

jce_add_unit_test(test_jce_graph_astar
    SOURCES test_jce_graph_astar.c
    LINK    jce_core jce_ai)

# Environment Query System (EQS) core: pure-math generators (GRID/RING) +
# weighted/filtered scoring tests with callback-injected world tests. No
# nav/physics/BT — fully headless and deterministic.
jce_add_unit_test(test_jce_eqs
    SOURCES test_jce_eqs.c
    LINK    jce_core jce_ai)

# Recast/Detour 3D path query (real bake + dtNavMeshQuery::getPolyHeight).
# jce_ai statically links recastnavigation, so jce_core + jce_ai resolves the
# jce_recast_* C surface together with the Detour query path it calls into.
jce_add_unit_test(test_jce_recast_3d_query
    SOURCES test_jce_recast_3d_query.c
    LINK    jce_core jce_ai)

# Recast/Detour dynamic obstacles via dtTileCache (FEATURE 6.1+ slice):
# bakes a tile-cache-backed navmesh, adds/removes cylinder + box obstacles,
# and asserts the real dtTileCache update + dtNavMeshQuery routes around them.
jce_add_unit_test(test_jce_navmesh_obstacles
    SOURCES test_jce_navmesh_obstacles.c
    LINK    jce_core jce_ai)

# Grid A* (jce_navmesh.c) persistent-scratch + generation-stamp rewrite (rank-6):
# detour correctness, cross-query determinism, and zero per-query heap churn.
jce_add_unit_test(test_jce_navmesh_astar_scratch
    SOURCES test_jce_navmesh_astar_scratch.c
    LINK    jce_core jce_ai)

# Bundled deterministic Behavior-Tree node library (Wait / Cooldown /
# SetBlackboard / ClearBlackboard / BlackboardCheck / MoveToTarget / Repeat).
# jce_ai statically links BehaviorTree.CPP, so the real factory + tick path is
# exercised; jce_core provides the blackboard primitive.
jce_add_unit_test(test_jce_bt_library
    SOURCES test_jce_bt_library.c
    LINK    jce_core jce_ai)

# C-ABI exception firewall (dependency/ABI audit P0): duplicate action
# registration and tick-time BT.CPP throws must be contained, never abort.
jce_add_unit_test(test_jce_bt_exception_firewall
    SOURCES test_jce_bt_exception_firewall.c
    LINK    jce_core jce_ai)

# EQS <-> Behavior-Tree adapter (jce_eqs_bt): a registerable "RunEqsQuery"
# action bridging the blackboard to the pure EQS core and back. Headless +
# deterministic (no nav/physics); exercises the action callback directly.
jce_add_unit_test(test_jce_eqs_bt
    SOURCES test_jce_eqs_bt.c
    LINK    jce_core jce_ai)

# A navmesh belongs to the agent it was carved for.  jce_nav_agent_fits is the
# ONE rule both backends ask -- the grid mesh and the Detour mesh -- so this
# pure comparison is where the two unknowns are pinned: an unset agent height
# and an unstated mesh clearance must both mean "no opinion", or the check
# rejects every scene authored before it existed.
jce_add_unit_test(test_jce_nav_agent_fit
    SOURCES test_jce_nav_agent_fit.c
    LINK    jce_core jce_ai)
