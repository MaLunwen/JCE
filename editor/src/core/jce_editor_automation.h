/*
 * jce_editor_automation.h — the editor calling the Automation API.
 *
 * REQ-ARCH-02 asks that the editor and the CLI call the SAME Automation API,
 * rather than growing two implementations that happen to agree.  This is the
 * editor's end of that: it spawns private/tools/automation/automation_cli.py and reads
 * back the same JSON envelope an external agent reads.  One implementation,
 * two front ends.
 *
 * A PUMP, NOT A THREAD, and not a blocking call.  jce_process.h requires one
 * thread, and an editor that stalls its frame loop for a subprocess has
 * stopped being an editor for the length of the call -- physics.probe runs a
 * real solver for seconds.  jce_editor_automation_poll() drains the pipe and
 * polls for exit once per frame, the same shape jce_llm.h uses for exactly
 * the same reason.
 *
 * ONE CALL IN FLIGHT.  Not a simplification: the Automation layer's own
 * process takes a single-instance lock for anything that reaches the engine,
 * and two concurrent calls through it is the failure that cost this tree a
 * day -- contended calls exit without writing a result and both call sites
 * translate that into INVALID_ARGS, which reads as "your arguments are wrong"
 * about arguments that were never wrong.
 *
 * WHAT IT MAY CALL, AND THE RULE ON WRITES.  A tool that WRITES the project
 * (contracts/automation-tools.json lists twelve with writes_project) must be
 * called from inside an editor edit scope -- jce_state_begin_batch_edit or
 * begin_entity_edit -- so the change is one Ctrl+Z like every other editor
 * edit.  That is the ownership REQ-ARCH-02 settles: the editor's undo history
 * owns the in-memory scene between saves, the changeset owns bytes on disk
 * between commits, and the seam is the save.  Unity's Undo.RecordObject rule
 * and Unreal's FScopedTransaction rule, unchanged.
 * tools/lint/check_editor_automation_undo.py fails the build if a writing
 * call appears outside a scope.
 */
#ifndef JCE_EDITOR_AUTOMATION_H
#define JCE_EDITOR_AUTOMATION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/* Start a call.  `json_args` is the tool's argument object as JSON text, or
 * NULL for "{}".  Returns false when a call is already in flight or the
 * interpreter could not be spawned; in both cases the reason is logged and
 * shown as a toast, because a button that does nothing and says nothing is
 * indistinguishable from a broken one. */
bool jce_editor_automation_call(const char *tool, const char *json_args);

/* Start a call to a tool that WRITES the project.  Three CLI invocations, not
 * one: api.call does `if spec.writes: require_changeset()`, so none of the
 * twelve project-writing tools can run outside a changeset and the CLI has no
 * single-shot form.  The door runs
 *
 *     changeset.begin -> the tool -> changeset.validate ->
 *     changeset.commit,  and on ANY failure: changeset.rollback
 *
 * VALIDATE IS NOT OPTIONAL AND IT IS NOT FAST.  changeset.commit
 * refuses an unvalidated changeset (REQ-TXN-03), so without that step
 * the COMMON path would be write -> refuse -> roll back: every write
 * appearing to work, undoing itself, and reporting a refusal about a
 * step the caller never asked for.  And validate runs a real LINT, so
 * a writing call holds the one call slot for MINUTES.  The caller is
 * responsible for saying which step is in flight -- see
 * jce_editor_automation_current_tool(), which names it.
 *
 * `title` becomes both the changeset title and the commit message.
 *
 * THE UNDO FOR THIS IS changeset.rollback, NOT Ctrl+Z, and that is the
 * ownership REQ-ARCH-02 settles rather than a limitation: the editor's undo
 * history owns the in-memory scene between saves, the changeset owns bytes on
 * disk between commits, and a tool that writes a file is acting on the second
 * layer.  Wrapping this in jce_state_begin_batch_edit would push an undo
 * record for an in-memory change that is not happening.
 */
bool jce_editor_automation_write(const char *tool, const char *json_args,
                                 const char *title);

/* Drive the in-flight call.  Call once per editor frame. */
void jce_editor_automation_poll(void);

bool jce_editor_automation_is_running(void);

/* The tool currently in flight, or "" -- for the status line. */
const char *jce_editor_automation_current_tool(void);

/* The changeset a write left OPEN, awaiting a human decision, or "".
 *
 * A write stops here on purpose.  It is the loop REQ-SCN-04 describes -- the
 * human previews in the editor and approves, and the approval is the commit --
 * and it is also the only honest way out of a real constraint: commit refuses
 * an unvalidated changeset (REQ-TXN-03) and validate runs a REAL LINT, minutes
 * on this tree.  Committing automatically would freeze the one call slot
 * behind a menu item that looks instant; validating with skip_build/skip_tests
 * would give an editor-made changeset a WEAKER gate than a CLI-made one, which
 * is two standards for one operation and the thing REQ-ARCH-02 exists to
 * prevent.  So the slow step is opt-in, and the menu says what it costs.
 */
const char *jce_editor_automation_open_change(void);

/* Validate then commit the open change.  SLOW: validate runs a real lint. */
bool jce_editor_automation_commit_open(void);

/* Roll the open change back.  Fast, and restores the tree byte for byte. */
bool jce_editor_automation_rollback_open(void);

/* Pin the probe thresholds into jce_project.json so they travel with the
 * project (REQ-PHY-02's "saved with the project" half).  WRITES: it goes
 * through jce_editor_automation_write, so its undo is changeset.rollback and
 * not Ctrl+Z -- see that function. */
bool jce_editor_automation_pin_thresholds(void);

/* Run physics.probe on the scene that is open, and report what it measured to
 * the Console.  READ-ONLY: it measures, it does not author, so it needs no
 * edit scope and cannot be undone because there is nothing to undo. */
bool jce_editor_automation_probe_current_scene(void);

/* Measure the ONE selected entity's model and propose its colliders, masses
 * and centre of mass, WITH THE REASON for every choice (REQ-PHY-03).
 * READ-ONLY: it writes nothing, and the reasons go to the Console so a person
 * can read them before deciding.  The plan is kept for _author_apply. */
bool jce_editor_automation_author_plan(void);

/* Write the plan kept by _author_plan into the open scene.  WRITES, so it goes
 * through the changeset and stops with the change OPEN for review; the undo is
 * changeset.rollback, not Ctrl+Z.
 *
 * Refuses when there is no kept plan, and refuses when the selection is a
 * DIFFERENT model from the one planned -- authoring A's colliders onto B would
 * produce a scene that looks entirely plausible and is wrong. */
bool jce_editor_automation_author_apply(void);

/* The ragdoll pair, same shape and same reason as the collider pair: plan is
 * READ-ONLY and reports the capsule radius and height scale WITH the bone pair
 * that bounds them; apply WRITES through the changeset.
 *
 * Apply authors the SkeletalAnimator as well as the Ragdoll, and that is not a
 * convenience: the runtime spawn is gated on the animator's skeleton loading a
 * skinned model, so a scene with a correct Ragdoll and no animator is a scene
 * whose ragdoll never exists -- and nothing in the scene, the editor or the
 * log says so.  Doing it through the tool is what makes the pair the unit. */
bool jce_editor_automation_ragdoll_plan(void);
bool jce_editor_automation_ragdoll_apply(void);

/* The AI-scene chain's two ends.  compile_recipe asks for a recipe file and
 * runs scene.compile -- THE ENGINE'S OWN compiler, so what the editor shows
 * and what a headless run produces are one plan with one hash.  READ-ONLY;
 * the pick is asynchronous and finishes inside _poll.
 *
 * materialise WRITES the compiled plan into the open scene, through the
 * changeset.  It refuses a plan from any other planner: every applier takes an
 * `object`, so a mismatched plan is a type-correct call that writes nonsense. */
bool jce_editor_automation_compile_recipe(void);
bool jce_editor_automation_materialise(void);

/* Create a gameplay script through script.write: a real, runnable template in
 * the project's scripts directory, under the changeset.  WRITES.
 *
 * The editor could not do this before.  The Assets browser's "New Script"
 * writes `NewScript.c` containing a seventeen-byte comment and nothing else --
 * not a valid script in any of the seven languages, at a fixed name that the
 * second use replaces, with no transaction.
 *
 * TWO LANGUAGES, deliberately, though script.write accepts seven: this tree
 * contains a working example of exactly two, and a template for the others
 * would be derived from a header comment rather than from anything observed
 * to run.  Enumerate them rather than hard-coding a list at the call site. */
int         jce_editor_automation_script_language_count(void);
const char *jce_editor_automation_script_language(int index);
bool        jce_editor_automation_script_new(const char *lang);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_AUTOMATION_H */
