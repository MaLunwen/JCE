/*
 * ck_quest_graph.h — Caged Kingdom storyline quest graph.
 *
 * Loads `quests/main_storyline.json` (see examples/caged_kingdom/SCENES_DESIGN.md
 * appendix A.2.5) and exposes a flat-keyed lookup so the scene director
 * can answer:
 *
 *   - "what scene file backs quest <id>?"
 *   - "what quest comes after <id>?"
 *
 * Quest ids are act-qualified strings (e.g. "act1_m01_wake"), matching
 * the `id` field inside each node.  Lookups against an unknown id return
 * NULL rather than aborting — the director treats that as "end of
 * storyline" and stays on the current scene.
 *
 * The graph is data, not behavior: it has no opinions on triggers, fade,
 * or player state.  Those live in ck_trigger / ck_fade / ck_player_state.
 */
#ifndef CK_QUEST_GRAPH_H
#define CK_QUEST_GRAPH_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceFileSystem JceFileSystem;
typedef struct CkQuestGraph  CkQuestGraph;

/* Load a quest graph from a VFS path (typically "quests/main_storyline.json").
   Returns NULL on missing file, parse error, or version mismatch.  The
   caller owns the returned handle and must call ck_quest_graph_destroy(). */
CkQuestGraph *ck_quest_graph_load_vfs(JceFileSystem *fs, const char *vfs_path);

/* Release all storage held by the graph.  Safe to pass NULL. */
void          ck_quest_graph_destroy(CkQuestGraph *graph);

/* Scene file backing quest `quest_id`, or NULL if unknown.  Returned
   string is owned by the graph and stays valid until destroy(). */
const char   *ck_quest_graph_scene(const CkQuestGraph *graph,
                                   const char *quest_id);

/* Quest id that follows `quest_id`, or NULL if terminal/unknown. */
const char   *ck_quest_graph_next(const CkQuestGraph *graph,
                                  const char *quest_id);

/* Convenience: scene file backing whatever `quest_id` says comes next.
   Returns NULL if `quest_id` is terminal/unknown or its successor has
   no scene mapping. */
const char   *ck_quest_graph_next_scene(const CkQuestGraph *graph,
                                        const char *quest_id);

/* Initial quest id declared at the top level (`"start"`), or NULL if
   the graph file did not specify one. */
const char   *ck_quest_graph_start(const CkQuestGraph *graph);

/* Total number of quest nodes loaded (sum across all acts).  Useful
   mostly for diagnostics. */
size_t        ck_quest_graph_count(const CkQuestGraph *graph);

#ifdef __cplusplus
}
#endif

#endif /* CK_QUEST_GRAPH_H */
