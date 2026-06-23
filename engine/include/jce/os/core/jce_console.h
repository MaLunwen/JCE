/*
 * jce_console.h -- Console variables (cvars) + command registry.
 *
 * A Quake/Unreal-style developer console core: typed, named variables that
 * any subsystem can register and that a console UI (or config file, or remote
 * tool) can list and mutate by name at runtime, plus named commands that run a
 * callback with parsed arguments.  This header is the pure registry + dispatch
 * core — it does no rendering and has no dependencies beyond jce_core, so it is
 * fully unit-testable.  A runtime/editor overlay binds to it via the output
 * sink and jce_console_exec().
 *
 * Process-global, single-thread (drive from the main/console thread).
 *
 * Layer: OS / Core (Layer 1) — foundational; subsystems register cvars early.
 */

#ifndef JCE_CONSOLE_H
#define JCE_CONSOLE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_CVAR_BOOL = 0,
    JCE_CVAR_INT,
    JCE_CVAR_FLOAT,
    JCE_CVAR_STRING
} JceCvarType;

/* Optional cvar flags. */
enum {
    JCE_CVAR_FLAG_NONE     = 0,
    JCE_CVAR_FLAG_READONLY = 1 << 0,  /* jce_cvar_set_from_string refuses it   */
    JCE_CVAR_FLAG_CHEAT    = 1 << 1   /* informational; gameplay may gate on it */
};

typedef struct JceCvar JceCvar;   /* opaque handle, stable for process life */

/* ── Registration ──────────────────────────────────────────────────────────
 * Idempotent: registering an existing name returns the existing cvar (the
 * default/help/flags of the first registration win).  Returns NULL on OOM or a
 * type clash (same name, different type). */
JCE_API JceCvar *jce_cvar_register_bool  (const char *name, bool   def,        uint32_t flags, const char *help);
JCE_API JceCvar *jce_cvar_register_int   (const char *name, int    def,        uint32_t flags, const char *help);
JCE_API JceCvar *jce_cvar_register_float (const char *name, float  def,        uint32_t flags, const char *help);
JCE_API JceCvar *jce_cvar_register_string(const char *name, const char *def,   uint32_t flags, const char *help);

JCE_API JceCvar    *jce_cvar_find(const char *name);
JCE_API JceCvarType jce_cvar_type(const JceCvar *cv);
JCE_API const char *jce_cvar_name(const JceCvar *cv);
JCE_API const char *jce_cvar_help(const JceCvar *cv);
JCE_API uint32_t    jce_cvar_flags(const JceCvar *cv);

/* ── Typed access (a get of the wrong type coerces sensibly). ─────────────── */
JCE_API bool        jce_cvar_get_bool  (const JceCvar *cv);
JCE_API int         jce_cvar_get_int   (const JceCvar *cv);
JCE_API float       jce_cvar_get_float (const JceCvar *cv);
JCE_API const char *jce_cvar_get_string(const JceCvar *cv);

JCE_API void jce_cvar_set_bool  (JceCvar *cv, bool   v);
JCE_API void jce_cvar_set_int   (JceCvar *cv, int    v);
JCE_API void jce_cvar_set_float (JceCvar *cv, float  v);
JCE_API void jce_cvar_set_string(JceCvar *cv, const char *v);

/* Parse `value` by the cvar's type and assign.  Returns false on parse error,
 * unknown name, or a READONLY cvar. */
JCE_API bool jce_cvar_set_from_string(const char *name, const char *value);

/* Format the cvar's current value into `out` (NUL-terminated, truncated to
 * cap).  Returns the number of chars written (excluding NUL). */
JCE_API int jce_cvar_format_value(const JceCvar *cv, char *out, int cap);

/* Enumeration (registration order) for listing / autocomplete. */
JCE_API int      jce_cvar_count(void);
JCE_API JceCvar *jce_cvar_at(int index);

/* ── Commands ──────────────────────────────────────────────────────────────
 * argv[0] is the command name; argc counts it.  `user` is the value passed to
 * jce_console_register_cmd. */
typedef void (*JceConsoleCmdFn)(int argc, const char **argv, void *user);

JCE_API bool jce_console_register_cmd(const char *name, JceConsoleCmdFn fn,
                                      void *user, const char *help);

/* ── Output sink + execution ──────────────────────────────────────────────
 * Console output (command results, value echoes, errors) is routed to the
 * registered sink; with none set it goes to the engine log.  `printf` callers
 * should pre-format. */
typedef void (*JceConsoleOutputFn)(const char *text, void *user);
JCE_API void jce_console_set_output(JceConsoleOutputFn fn, void *user);
JCE_API void jce_console_print(const char *text);

/* Execute one console line:
 *   "name"            -> if a command, run with no args; if a cvar, echo value
 *   "name value ..."  -> if a command, run with args; if a cvar, set from value
 * Tokens split on whitespace; a "double quoted" token may contain spaces.
 * Returns true if the line resolved to a known command or cvar. */
JCE_API bool jce_console_exec(const char *line);

/* Drop all registered cvars/commands (tests / engine teardown). */
JCE_API void jce_console_shutdown(void);

JCE_EXTERN_C_END

#endif /* JCE_CONSOLE_H */
