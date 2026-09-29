/*
 * api_ai.h  AI / behaviour trees.
 *
 * Behaviour tree authoring and execution via BehaviorTree.CPP C bridge.
 */

#ifndef JCE_API_AI_H
#define JCE_API_AI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/middleware/ai/jce_bt.h>
#include <jce/middleware/ai/jce_perception.h>

/* Completed 2026-08-31.  These headers export JCE_API symbols and were
 * reachable from NO umbrella, so §4's promise -- `#include <jce/api.h>`
 * gives you the whole engine -- did not hold for them.  Several are named
 * in §4's own table by capability.
 */
#include <jce/middleware/ai/jce_ai_ecs.h>
#include <jce/middleware/ai/jce_eqs.h>
#include <jce/middleware/ai/jce_eqs_bt.h>
#include <jce/middleware/ai/jce_graph_astar.h>
#include <jce/middleware/ai/jce_nav_agent.h>
#include <jce/middleware/ai/jce_navmesh.h>
#include <jce/middleware/ai/jce_navmesh_recast.h>
#include <jce/middleware/ai/jce_steering.h>



#ifdef __cplusplus
}
#endif
#endif /* JCE_API_AI_H */
