/*
 * jce_script_exports.h  What a SCRIPT FILE declares it wants from an author.
 *
 * JceScriptComponent already carries authored parameters, and the Inspector
 * already draws them -- but the author declares them there, by typing a name
 * and picking a kind.  The script itself has no say, which is the one reason
 * both scripting.inspector.exposed-fields and
 * editor.inspector.script-exposed-fields are behind: Unity's
 * `[SerializeField] float speed;`, Godot's `@export var speed: float` and
 * UE's `UPROPERTY(EditAnywhere)` are all declarations made IN THE SCRIPT that
 * the editor then discovers.  Here the editor had nothing to discover.
 *
 * THE DECLARATION IS A COMMENT, and that is a decision rather than a
 * shortcut.  This engine runs SEVEN script languages (lua, c, cpp, csharp,
 * java, js, python).  A native form -- an attribute, an annotation, a
 * decorator -- means seven parsers that can disagree, and for the C and C++
 * backends there is no reflection to hang one on at all.  Every one of the
 * seven has line comments, so one marker inside a comment is the only form
 * that is identical in all of them, and it is the form that cannot drift
 * between backends because there is only one reader:
 *
 *     -- @export number  speed  = 5.0        (lua)
 *     // @export bool    alive  = true       (c / cpp / csharp / java / js)
 *     #  @export text    label  = patrol     (python)
 *     // @export entity  target               (no default: entity 0)
 *
 * The scanner does not care which comment syntax surrounds it; it looks for
 * the marker anywhere on a line.  That means a `@export` inside a STRING
 * would also be read -- accepted deliberately, because the alternative is
 * seven lexers, and a false positive shows up immediately as a parameter the
 * author did not expect rather than as a value that is silently wrong.
 *
 * WHAT THIS IS NOT.  It is not type-checked by the language's own compiler,
 * so renaming the variable does not rename the declaration.  That gap is real
 * and is named in the ledger rows rather than papered over here.
 */

#ifndef JCE_SCRIPT_EXPORTS_H
#define JCE_SCRIPT_EXPORTS_H

#include <jce/middleware/scene/jce_scene.h>

#include <stddef.h>

JCE_EXTERN_C_BEGIN

/*
 * Scan script SOURCE TEXT for `@export` declarations, newest-to-oldest order
 * preserved, and fill `out` with at most `max_out` of them.
 *
 * Returns the number written, or -1 when `source` or `out` is NULL or
 * `max_out` is not positive.  `len` is the source length in bytes; the text
 * does not have to be NUL-terminated.
 *
 * THE RESULT IS JceScriptParam, not a parallel struct.  A declaration and an
 * authored value are the same four kinds with the same four value slots, and
 * a second type would mean a second mapping between them -- the exact shape
 * this tree keeps finding as "two answers for one thing".  A declaration
 * fills `name`, `kind`, and whichever value slot its default belongs in; a
 * declaration with no default leaves that slot zeroed, which is the same
 * zero an unauthored parameter has.
 *
 * SKIPPED, never guessed: a line whose kind word is not one of
 * number/bool/text/entity, whose name is empty, longer than 31 bytes, or not
 * made of [A-Za-z0-9_] starting with a letter or underscore.  A malformed
 * declaration produces NO parameter rather than a parameter with an invented
 * name, so an author sees their typo as a missing row.
 *
 * FIRST DECLARATION WINS on a duplicate name, matching the array the result
 * feeds: JceScriptComponent keys parameters by name and two rows with one
 * name would make `jce.get_param` depend on scan order.
 */
JCE_API int jce_script_exports_scan(const char *source, size_t len,
                                    JceScriptParam *out, int max_out);

/*
 * Merge declarations into an authored parameter array, the way an editor
 * wants them: every declared parameter ends up present, and every AUTHORED
 * VALUE that is still declared is KEPT.
 *
 * `params` / `count` are the component's array, updated in place; `cap` is
 * JCE_SCRIPT_PARAM_MAX.  Returns the new count, or -1 on bad arguments.
 *
 * THREE RULES, and the second is the one that matters:
 *   declared, not authored  -> appended with the declared default
 *   declared AND authored   -> the AUTHORED VALUE SURVIVES; only the kind is
 *                              taken from the declaration, because the script
 *                              is what decides what the value means
 *   authored, not declared  -> LEFT ALONE, not deleted
 *
 * That last one is why this is a merge and not an overwrite.  Deleting an
 * author's tuned value because a script was mid-edit -- renamed a field, or
 * simply not saved yet -- would destroy work that cannot be recovered from
 * the script file, and it would do it during a routine redraw.  A stale
 * parameter is visible and removable; a deleted one is gone.
 */
JCE_API int jce_script_exports_merge(const JceScriptParam *decls, int decl_count,
                                     JceScriptParam *params, int count, int cap);

/*
 * True when `name` appears in `decls`.  The editor uses it to mark a row the
 * script no longer declares, which is the only way an author can tell a
 * parameter they are still using from one left behind by a rename.
 */
JCE_API bool jce_script_exports_declares(const JceScriptParam *decls,
                                         int decl_count, const char *name);

JCE_EXTERN_C_END

#endif /* JCE_SCRIPT_EXPORTS_H */
