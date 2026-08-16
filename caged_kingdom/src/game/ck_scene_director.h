/*
 * ck_scene_director.h  Runtime scene swapper for caged_kingdom.
 *
 * Owns the lifetime of a single JceScene + JceSceneRenderer pair and
 * provides the *runtime* "load this level next" entry point.
 *
 * Backed by jce_scene_clear() (engine), so transitions reuse the same
 * scene handle and renderer caches.  Bookkeeping for the level the
 * player "carries" across transitions lives in CkPlayerState, also
 * owned by the director.
 *
 * v1 transport: directly call ck_scene_director_load(dir, "scenes/X.scene.json")
 * v1.1 (not yet wired): quest-graph driven; see SCENES_DESIGN.md A.2.5.
 *
 * Allocations: JCE_CALLOC / JCE_FREE.
 * Logging tag : "ck_director".
 */

#ifndef CK_SCENE_DIRECTOR_H
#define CK_SCENE_DIRECTOR_H

#include <stdbool.h>

#include "ck_player_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations — we only hold pointers in the header to keep
   the include surface small.  Definitions come from the engine in .c. */
typedef struct JceScene         JceScene;
typedef struct JceSceneRenderer JceSceneRenderer;
typedef struct JceRenderer      JceRenderer;
typedef struct JcePakArchive    JcePakArchive;
typedef struct JceAudio         JceAudio;

typedef struct CkSceneDirector CkSceneDirector;

/* Create with the engine subsystems the director will need to load
   scenes and drive the renderer/runtime.  Renderer and PAK must outlive
   the director; audio is optional and must outlive it when supplied.
   On success, the director also loads the storyline quest graph from
   `quests/main_storyline.json`; if that file is missing transitions
   simply won't fire (the rest of the director still works). */
CkSceneDirector *ck_scene_director_create(JceRenderer *renderer,
                                          JcePakArchive *pak,
                                          JceAudio *audio);

void ck_scene_director_destroy(CkSceneDirector *dir);

/* First scene of a session.  Initialises CkPlayerState defaults and
   loads `vfs_path` into the freshly created scene.  Returns false if
   the asset cannot be loaded; in that case the director is still valid
   (empty scene) but the caller should treat it as a fatal startup error. */
bool ck_scene_director_load_initial(CkSceneDirector *dir,
                                    const char *vfs_path);

/* Convenience: like load_initial(), but resolves the path through the
   storyline quest graph's `start` entry.  Returns false if no graph is
   loaded or the start node has no scene. */
bool ck_scene_director_load_start(CkSceneDirector *dir);

/* Switch the active scene at runtime.  Calls jce_scene_clear() then
   re-invokes the serial loader on the same scene handle, so renderer
   caches (textures / meshes already pulled from PAK) stay warm.
   Also refreshes the trigger set and player.active_quest from the new
   scene's `ck_quest` block.  Returns false if the load failed; in that
   case the scene is left empty but the director / renderer remain
   usable. */
bool ck_scene_director_load(CkSceneDirector *dir, const char *vfs_path);

/* Per-frame update.  Checks whether `player_pos[3]` (XZ disc) is inside
   any of the current scene's trigger zones; if so, advances the player
   state to the next quest and loads the corresponding scene.  Safe to
   call when no triggers / no graph are loaded (no-op). */
void ck_scene_director_tick(CkSceneDirector *dir, float dt_sec,
                            const float player_pos[3]);

/* Accessors used by ck_app and by upcoming systems (fade / HUD / ...). */
JceScene         *ck_scene_director_scene(CkSceneDirector *dir);
JceSceneRenderer *ck_scene_director_renderer(CkSceneDirector *dir);
CkPlayerState    *ck_scene_director_player(CkSceneDirector *dir);

/* Identifier of the most recently loaded scene (vfs path string).
   Returns an empty string if nothing has been loaded yet. */
const char       *ck_scene_director_current_path(const CkSceneDirector *dir);

#ifdef __cplusplus
}
#endif

#endif /* CK_SCENE_DIRECTOR_H */
