/*
 * jce_console_session.h -- one console surface's editing state.
 *
 * jce_console.h is the registry and the parser.  This is the part between a
 * keyboard and jce_console_exec(): the line being typed, the history behind
 * it, and Tab completion over the names that are actually registered right
 * now.  It renders nothing and owns no window, so it is unit-testable and the
 * SAME code serves the editor's console panel and a shipped game's overlay --
 * which is the point.  Before it, the editor panel was a bare text field with
 * no history and no completion, and a shipped game had no console at all.
 *
 * WHY THIS EXISTS AT ALL: jce_console.h's opening says a console UI "can list
 * and mutate by name at runtime".  Mutating had a reader.  LISTING did not --
 * jce_cvar_at, jce_cvar_help and jce_cvar_flags had no caller anywhere in the
 * repository, and jce_cvar_count / jce_cvar_format_value / jce_console_cmd_*
 * were reached only by their own unit test.  The registry has carried a full
 * introspection surface since it shipped and nothing has ever enumerated it.
 * This is that reader.
 *
 * A session does NOT touch jce_console_set_output().  That sink is
 * single-slot ("routed to THE registered sink"), the editor's panel already
 * holds it, and there is no getter to save and restore it -- so a session
 * that grabbed it would silently stop whichever surface had it first.  Output
 * stays the surface's business; this owns input.
 *
 * Process-global registry, so a session is single-thread like the registry it
 * reads (drive from the main/console thread).  Several sessions may coexist;
 * they share the registry and nothing else, including history.
 *
 * Layer: OS / Core (Layer 1) -- same layer as the registry it reads.
 */

#ifndef JCE_CONSOLE_SESSION_H
#define JCE_CONSOLE_SESSION_H

#include <jce/os/core/jce_console.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* The most recent commands a session remembers, and the most completion
 * matches it will hand back.  Both are storage limits, not truth limits:
 * jce_console_session_complete() reports how many names actually matched,
 * which can exceed what jce_console_session_match_at() can return. */
#define JCE_CONSOLE_SESSION_HISTORY   64
#define JCE_CONSOLE_SESSION_MATCHES   64
#define JCE_CONSOLE_SESSION_LINE_MAX  256

typedef struct JceConsoleSession JceConsoleSession;

JCE_API JceConsoleSession *jce_console_session_create(void);
JCE_API void               jce_console_session_destroy(JceConsoleSession *s);

/* -- The line being edited ------------------------------------------------
 * The surface owns the caret and the keystrokes; the session owns the text.
 * `line` is never NULL for a live session -- an empty session reads "". */
JCE_API const char *jce_console_session_line(const JceConsoleSession *s);
/* Replace the line.  NULL or "" clears it.  Longer than
 * JCE_CONSOLE_SESSION_LINE_MAX-1 is truncated, not refused: a surface pasting
 * a long line should get the front of it rather than nothing. */
JCE_API void        jce_console_session_set_line(JceConsoleSession *s,
                                                 const char *text);

/* -- Submit ---------------------------------------------------------------
 * Runs the line through jce_console_exec(), records it in history, and
 * clears the line.  Returns what jce_console_exec() returned -- false for an
 * unknown name or a refused value -- or false without running anything when
 * the line is blank.
 *
 * A REJECTED LINE IS STILL RECORDED.  A typo is exactly the line you want Up
 * to bring back, and a history that only keeps commands that worked is a
 * history that deletes the ones you need to fix.  Consecutive duplicates
 * collapse, so holding Enter does not fill it. */
JCE_API bool jce_console_session_submit(JceConsoleSession *s);

/* -- History (Up / Down) --------------------------------------------------
 * `prev` walks toward older entries, `next` toward newer.  Each returns true
 * when the line changed.
 *
 * Stepping off the newest entry restores WHAT WAS BEING TYPED when the walk
 * started, not an empty line: a half-written command is not something the
 * Down key should destroy.  Any edit through set_line() ends the walk, so the
 * next Up starts again from the newest entry. */
JCE_API bool jce_console_session_history_prev(JceConsoleSession *s);
JCE_API bool jce_console_session_history_next(JceConsoleSession *s);

/* -- Completion (Tab) -----------------------------------------------------
 * Completes the NAME at the start of the line against every registered cvar
 * and command.  Returns the TOTAL number of matches:
 *
 *   0  nothing matched; the line is untouched.
 *   1  unique; the line becomes that name followed by a space, ready for a
 *      value.
 *   n  ambiguous; the line is extended to the longest prefix common to all n
 *      matches -- which may be no extension at all -- and the names are
 *      retrievable with match_at() below.
 *
 * The common prefix is computed over ALL matches even when more matched than
 * match_at() can return, so a truncated list can never over-extend the line
 * into a name that does not exist.
 *
 * A line that already contains a space completes nothing and returns 0: the
 * cvar's VALUE is being typed, and the registry knows a cvar's type but not
 * its legal values, so there is nothing honest to offer there. */
JCE_API uint32_t    jce_console_session_complete(JceConsoleSession *s);
/* How many of the last complete()'s matches can be read back -- at most
 * JCE_CONSOLE_SESSION_MATCHES, and less than complete()'s return value when
 * more matched than that.  Zero until complete() runs. */
JCE_API uint32_t    jce_console_session_match_count(const JceConsoleSession *s);
/* Name of the i-th retrievable match, or NULL when i is out of range.  The
 * pointer is owned by the registry and is stable for process life. */
JCE_API const char *jce_console_session_match_at(const JceConsoleSession *s,
                                                 uint32_t i);

JCE_EXTERN_C_END

#endif /* JCE_CONSOLE_SESSION_H */
