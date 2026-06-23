/*
 * jce_save_migration.h  Central save-migration registry.
 *
 * The snapshot framework (jce_snapshot.h) hands every section's read_fn the
 * version the bytes were written at (loaded_version).  Without a shared
 * upgrade path each provider has to DIY every old layout it ever shipped,
 * and most simply refuse anything older than current — which silently
 * orphans real player saves.  This registry centralizes that work.
 *
 * A migration is an ordered chain of single-step transforms over a section's
 * parsed JSON document:
 *
 *     v1 --step--> v2 --step--> v3 ... --step--> current
 *
 * Each step is registered for one section id and one exact from_version; it
 * rewrites the JSON in place (rename / rescale / restructure fields) and the
 * registry records the to_version the data now conforms to.  On load the
 * provider parses its payload to JSON and calls jce_save_migrate(); the
 * registry walks the chain from the loaded version up to the provider's
 * current version, applying each step in turn, *before* the provider's
 * read_fn consumes the (now current-version) JSON.
 *
 * Guarantees:
 *   - Current-version loads are a strict no-op (from == to): zero lookups,
 *     zero overhead, JSON untouched.
 *   - A missing step (no transform registered for some intermediate
 *     from_version on the way to `to`) is REPORTED as a failure, never
 *     silently skipped — a half-migrated document is corruption.
 *   - from > to (a future save) is reported as a failure (downgrade is not
 *     supported); the caller should refuse the load.
 *   - Steps must strictly advance the version (to_version > from_version) and
 *     a from_version may have only one step (deterministic chain).
 *
 * The registry owns nothing about the JSON it transforms; the provider keeps
 * ownership of the JceJson document across the call.  A step transforms in
 * place and returns true on success / false to abort the whole migration.
 *
 * Thread-safety: matches jce_snapshot — register all steps before any load.
 *
 * Layer: middleware/save (L4).  Deps: jce_core (json) only.
 */
#ifndef JCE_SAVE_MIGRATION_H
#define JCE_SAVE_MIGRATION_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_json.h>   /* JceJson (typedef of cJSON) */
#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Migration registry                                                  */
/* ================================================================== */
typedef struct JceSaveMigrationRegistry JceSaveMigrationRegistry;

/*
 * A single migration step.  `json` is the section's parsed document at
 * `from_version`; transform it in place so it conforms to the step's
 * to_version.  Return false to abort the entire migration (the caller
 * should then refuse the load).  `user` is the pointer passed at
 * registration.
 */
typedef bool (*JceSaveMigrateFn)(JceJson *json, void *user);

JCE_API JceSaveMigrationRegistry *jce_save_migration_registry_create(void);
JCE_API void jce_save_migration_registry_destroy(JceSaveMigrationRegistry *r);

/*
 * Register one step: for section `id`, JSON written at `from_version` is
 * upgraded by `fn` to `to_version`.  Requires to_version > from_version and
 * a unique (id, from_version) pair (a second registration for the same pair
 * is rejected, leaving the first intact, to keep the chain deterministic).
 *
 * Returns true when the step was registered.  No-op / false on NULL args,
 * to_version <= from_version, or a duplicate (id, from_version).
 */
JCE_API bool jce_save_migration_register(JceSaveMigrationRegistry *r,
                                         const char       *id,
                                         uint32_t          from_version,
                                         uint32_t          to_version,
                                         JceSaveMigrateFn  fn,
                                         void             *user);

/*
 * Apply the chain of registered steps for section `id` to bring `json` from
 * `from_version` up to `to_version`, transforming `json` in place.
 *
 *   - from == to : strict no-op, returns true, `json` untouched (zero cost).
 *   - from  < to : walk steps from_version -> ... -> to_version.  Every
 *                  intermediate from_version on the path MUST have exactly
 *                  one registered step or the call fails (missing step is
 *                  reported, not silently skipped).  A step that returns
 *                  false aborts and the call fails.  A step that overshoots
 *                  `to` (to_version > requested to) fails.
 *   - from  > to : fails (downgrade unsupported).
 *
 * Returns true only when `json` has been advanced exactly to `to_version`.
 * On failure `json` may be partially transformed — the caller must treat
 * the load as failed and not consume it.
 */
JCE_API bool jce_save_migrate(JceSaveMigrationRegistry *r,
                              const char *id,
                              uint32_t    from_version,
                              uint32_t    to_version,
                              JceJson    *json);

JCE_EXTERN_C_END
#endif /* JCE_SAVE_MIGRATION_H */
