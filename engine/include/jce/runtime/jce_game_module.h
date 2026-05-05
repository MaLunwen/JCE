/*
 * jce_game_module.h  Game-module registry (B-phase: real game embedding).
 *
 * A "game module" is just a JceAppDesc that the editor can drive in-process
 * during Play mode.  Modules register themselves at startup and the editor
 * picks one via the Game View toolbar.
 *
 * The editor provides a JceServices struct built from its own subsystems;
 * modules behave the same whether they're driven by the standalone exe
 * (JCE_MAIN -> Run loop) or by the editor (Play -> per-frame lifecycle).
 */

#ifndef JCE_GAME_MODULE_H
#define JCE_GAME_MODULE_H

#include <jce/application/jce_app_interface.h>

JCE_EXTERN_C_BEGIN

typedef JceAppDesc JceGameModule;

/* Built-in default module: pure ECS tick (scripts / animators / physics).
 * Always present, no game-specific logic.  Returned pointer is static. */
JCE_API const JceGameModule *jce_game_module_default(void);

/* Register a game module by name.  Name must be a string literal or a
 * pointer with program-lifetime storage; not copied.  Returns true if
 * accepted, false on overflow / duplicate. */
JCE_API bool jce_game_module_register(const char *name,
                                      const JceGameModule *desc);

/* Lookup by name.  Returns NULL if not found. */
JCE_API const JceGameModule *jce_game_module_find(const char *name);

/* Enumerate registered modules (the default module is always index 0). */
JCE_API int          jce_game_module_count(void);
JCE_API const char  *jce_game_module_name_at(int idx);
JCE_API const JceGameModule *jce_game_module_at(int idx);

JCE_EXTERN_C_END

#endif /* JCE_GAME_MODULE_H */
