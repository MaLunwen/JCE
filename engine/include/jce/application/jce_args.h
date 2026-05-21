/*
 * jce_args.h  Process-argv accessor for engine consumers.
 *
 * The engine entry point (jce_main_sdl.c → jce_engine_create) hands the
 * platform argv to jce_args_stash() once at boot.  Anything in the
 * engine, middleware, or game layer that needs to inspect launch
 * options reads through these accessors instead of touching argv
 * directly (the editor never sees argv at all otherwise — `JCE_MAIN`
 * hides it).
 *
 * Today we expose only the two switches the runtime actually needs.
 * Add new accessors here (do NOT expose raw argv) when a feature
 * needs another flag.
 *
 * Layer: L1 application (depends on jce_defs only).
 */

#ifndef JCE_ARGS_H
#define JCE_ARGS_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>

JCE_EXTERN_C_BEGIN

/* Called once by the engine entry point before any subsystem init.
 * Safe to call again — last call wins.  Implementation copies into
 * a fixed-size internal table; overflow argv past the cap is
 * silently dropped (logged at info level). */
JCE_API void JCE_CALL jce_args_stash(int argc, char *argv[]);

/* True iff --dev was passed (with or without a value). */
JCE_API bool JCE_CALL jce_args_has_dev(void);

/* Returns the resolved dev-assets source directory, in priority
 * order:
 *   1. `--dev <dir>` or `--dev=<dir>` on the command line
 *   2. JCE_DEV_ASSETS environment variable
 *
 * Copies into `out` (NUL-terminated, truncated to `cap-1` chars) and
 * returns true on success.  Returns false (and leaves `out` untouched)
 * when neither source is set or `out`/`cap` are invalid.
 *
 * The returned path is not normalised; callers that need a
 * canonical form should run it through jce_path_*. */
JCE_API bool JCE_CALL jce_args_get_dev_assets(char *out, size_t cap);

JCE_EXTERN_C_END

#endif /* JCE_ARGS_H */
