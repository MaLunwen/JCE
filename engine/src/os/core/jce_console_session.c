/*
 * jce_console_session.c -- line, history and Tab completion for one console
 * surface.  See jce_console_session.h for why this is separate from both the
 * registry and any UI.
 */

#include <jce/os/core/jce_console_session.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_str.h>

#include <string.h>

struct JceConsoleSession {
    char     line[JCE_CONSOLE_SESSION_LINE_MAX];

    /* Ring of submitted lines, oldest overwritten.  `count` saturates at
     * JCE_CONSOLE_SESSION_HISTORY; `head` is where the NEXT entry goes. */
    char     hist[JCE_CONSOLE_SESSION_HISTORY][JCE_CONSOLE_SESSION_LINE_MAX];
    uint32_t hist_count;
    uint32_t hist_head;

    /* Walk state.  `cursor` counts BACK from the newest: 0 = not walking,
     * 1 = newest entry, hist_count = oldest.  `pending` holds what was being
     * typed when the walk started, so stepping back off the newest restores
     * it instead of blanking a half-written command. */
    uint32_t cursor;
    char     pending[JCE_CONSOLE_SESSION_LINE_MAX];

    const char *matches[JCE_CONSOLE_SESSION_MATCHES];
    uint32_t    match_count;
};

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* jce_strlcpy does the bounded, always-terminated copy; this adds only the
 * NULL-means-empty convention the callers below rely on.  The first draft
 * hand-rolled it and the dedup detector caught it the moment the file became
 * visible to `git ls-files`: token-identical to safe_copy() in
 * jce_pbr_material.c, which is what a fifth private strlcpy looks like from
 * the outside. */
static void line_set(char *dst, const char *src)
{
    jce_strlcpy(dst, src ? src : "", JCE_CONSOLE_SESSION_LINE_MAX);
}

/* The i-th most recent entry (i = 1 is newest).  NULL when out of range. */
static const char *hist_recent(const JceConsoleSession *s, uint32_t i)
{
    if (i == 0 || i > s->hist_count) return NULL;
    /* head is one past the newest, modulo the ring. */
    uint32_t idx = (s->hist_head + JCE_CONSOLE_SESSION_HISTORY - i)
                 % JCE_CONSOLE_SESSION_HISTORY;
    return s->hist[idx];
}

static void hist_push(JceConsoleSession *s, const char *text)
{
    const char *newest = hist_recent(s, 1);
    if (newest && strcmp(newest, text) == 0) return;   /* collapse repeats */

    line_set(s->hist[s->hist_head], text);
    s->hist_head = (s->hist_head + 1) % JCE_CONSOLE_SESSION_HISTORY;
    if (s->hist_count < JCE_CONSOLE_SESSION_HISTORY) s->hist_count++;
}

/* Length of the leading name on the line, or -1 when the line already has a
 * space in it (a value is being typed, and completing that is not something
 * the registry can do honestly). */
static int name_len(const char *line)
{
    size_t n = strlen(line);
    for (size_t i = 0; i < n; ++i)
        if (line[i] == ' ' || line[i] == '\t') return -1;
    return (int)n;
}

/* Every registered name, cvars then commands, by index.  Keeps the two
 * registries' enumeration in one place so a caller cannot walk one and forget
 * the other -- which is what a Tab key that knows half the namespace is. */
static const char *registered_name(int i)
{
    int ncv = jce_cvar_count();
    if (i < ncv) {
        JceCvar *cv = jce_cvar_at(i);
        return cv ? jce_cvar_name(cv) : NULL;
    }
    const char *nm = NULL;
    if (!jce_console_cmd_at(i - ncv, &nm, NULL)) return NULL;
    return nm;
}

static int registered_count(void)
{
    return jce_cvar_count() + jce_console_cmd_count();
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

JceConsoleSession *jce_console_session_create(void)
{
    JceConsoleSession *s = (JceConsoleSession *)jce_malloc(sizeof(*s));
    if (s) memset(s, 0, sizeof(*s));   /* empty line, empty history, no walk */
    return s;
}

void jce_console_session_destroy(JceConsoleSession *s)
{
    jce_free(s);
}

/* ── The line ────────────────────────────────────────────────────────── */

const char *jce_console_session_line(const JceConsoleSession *s)
{
    return s ? s->line : "";
}

void jce_console_session_set_line(JceConsoleSession *s, const char *text)
{
    if (!s) return;
    line_set(s->line, text);
    /* An edit ends the history walk: the next Up starts from the newest
     * entry again rather than continuing from wherever the last walk left
     * off, which would step through history the user has since left. */
    s->cursor = 0;
}

/* ── Submit ──────────────────────────────────────────────────────────── */

bool jce_console_session_submit(JceConsoleSession *s)
{
    if (!s || !s->line[0]) return false;

    char sent[JCE_CONSOLE_SESSION_LINE_MAX];
    line_set(sent, s->line);

    /* Record BEFORE executing.  A command that fails -- or that quits the
     * application -- is still one the user typed, and history is how they get
     * it back to fix it. */
    hist_push(s, sent);
    s->line[0]   = '\0';
    s->cursor    = 0;
    s->pending[0] = '\0';

    return jce_console_exec(sent);
}

/* ── History ─────────────────────────────────────────────────────────── */

bool jce_console_session_history_prev(JceConsoleSession *s)
{
    if (!s || s->hist_count == 0) return false;
    if (s->cursor >= s->hist_count) return false;   /* already at the oldest */

    if (s->cursor == 0)
        line_set(s->pending, s->line);   /* starting a walk: stash the draft */

    s->cursor++;
    const char *e = hist_recent(s, s->cursor);
    if (!e) { s->cursor--; return false; }
    line_set(s->line, e);
    return true;
}

bool jce_console_session_history_next(JceConsoleSession *s)
{
    if (!s || s->cursor == 0) return false;   /* not walking */

    s->cursor--;
    if (s->cursor == 0) {
        /* Stepped off the newest entry: give back the draft, not a blank. */
        line_set(s->line, s->pending);
        s->pending[0] = '\0';
        return true;
    }
    const char *e = hist_recent(s, s->cursor);
    if (!e) return false;
    line_set(s->line, e);
    return true;
}

/* ── Completion ──────────────────────────────────────────────────────── */

uint32_t jce_console_session_complete(JceConsoleSession *s)
{
    if (!s) return 0;
    s->match_count = 0;

    const int nlen = name_len(s->line);
    if (nlen < 0) return 0;              /* typing a value, not a name */

    const int total_names = registered_count();
    const size_t plen = (size_t)nlen;

    /* Pass 1: count matches and fold the longest common prefix over ALL of
     * them.  The fold has to see every match, not just the ones that fit in
     * the array below -- a prefix folded over a truncated list can be longer
     * than the true common prefix, and extending the line by it would write a
     * name that does not exist. */
    uint32_t    total    = 0;
    const char *lcp      = NULL;   /* first match; the running prefix */
    size_t      lcp_len  = 0;

    for (int i = 0; i < total_names; ++i) {
        const char *nm = registered_name(i);
        if (!nm || strncmp(nm, s->line, plen) != 0) continue;

        if (total == 0) {
            lcp     = nm;
            lcp_len = strlen(nm);
        } else {
            size_t k = 0;
            while (k < lcp_len && nm[k] && lcp[k] == nm[k]) ++k;
            lcp_len = k;
        }
        total++;
        if (s->match_count < JCE_CONSOLE_SESSION_MATCHES)
            s->matches[s->match_count++] = nm;
    }

    if (total == 0) return 0;

    if (total == 1) {
        /* Unique: complete it and leave a space, so the next keystroke is the
         * value rather than a Tab that now matches nothing. */
        char done[JCE_CONSOLE_SESSION_LINE_MAX];
        line_set(done, lcp);
        size_t n = strlen(done);
        if (n + 1 < JCE_CONSOLE_SESSION_LINE_MAX) { done[n] = ' '; done[n + 1] = '\0'; }
        line_set(s->line, done);
        s->cursor = 0;
        return 1;
    }

    /* Ambiguous: extend to the common prefix, if that is an extension. */
    if (lcp_len > plen) {
        char done[JCE_CONSOLE_SESSION_LINE_MAX];
        size_t n = lcp_len;
        if (n >= JCE_CONSOLE_SESSION_LINE_MAX) n = JCE_CONSOLE_SESSION_LINE_MAX - 1;
        memcpy(done, lcp, n);
        done[n] = '\0';
        line_set(s->line, done);
        s->cursor = 0;
    }
    return total;
}

uint32_t jce_console_session_match_count(const JceConsoleSession *s)
{
    return s ? s->match_count : 0u;
}

const char *jce_console_session_match_at(const JceConsoleSession *s, uint32_t i)
{
    if (!s || i >= s->match_count) return NULL;
    return s->matches[i];
}
