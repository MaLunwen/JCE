/*
 * jce_script_exports.c  Parse `@export` declarations out of script source.
 *
 * See jce_script_exports.h for why the declaration is a comment marker and
 * not a native attribute in each of the seven languages.
 *
 * The scanner is deliberately a line scanner with no lexer state: it never
 * needs to know which language it is reading, which is the whole point.
 */

#include <jce/middleware/scene/jce_script_exports.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MARKER     "@export"
#define MARKER_LEN 7

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

static bool is_name_start(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool is_name_char(char c)
{
    return is_name_start(c) || (c >= '0' && c <= '9');
}

/* Advance past spaces; returns the new cursor. */
static const char *skip_space(const char *p, const char *end)
{
    while (p < end && is_space(*p)) ++p;
    return p;
}

/* Copy a run of non-space characters into `dst` (NUL-terminated).  Returns
 * the cursor past the word, or NULL when the word does not fit -- the caller
 * treats that as a malformed declaration rather than truncating, because a
 * truncated NAME would silently bind to a different parameter. */
static const char *take_word(const char *p, const char *end,
                             char *dst, size_t dst_size)
{
    size_t n = 0;
    while (p < end && !is_space(*p)) {
        if (n + 1 >= dst_size) return NULL;
        dst[n++] = *p++;
    }
    dst[n] = '\0';
    return n ? p : NULL;
}

static bool kind_from_word(const char *w, uint32_t *out_kind)
{
    if (strcmp(w, "number") == 0) { *out_kind = JCE_SCRIPT_PARAM_NUMBER; return true; }
    if (strcmp(w, "bool")   == 0) { *out_kind = JCE_SCRIPT_PARAM_BOOL;   return true; }
    if (strcmp(w, "text")   == 0) { *out_kind = JCE_SCRIPT_PARAM_TEXT;   return true; }
    if (strcmp(w, "entity") == 0) { *out_kind = JCE_SCRIPT_PARAM_ENTITY; return true; }
    return false;
}

static bool name_is_valid(const char *n)
{
    if (!n[0] || !is_name_start(n[0])) return false;
    for (const char *p = n + 1; *p; ++p)
        if (!is_name_char(*p)) return false;
    return true;
}

/* Parse the default text after '=' into whichever value slot `kind` names.
 * An unparseable default leaves the slot ZEROED rather than rejecting the
 * declaration: the author's intent to expose the parameter is clear, and a
 * missing row is a worse answer to a typo'd default than a zero. */
static void apply_default(JceScriptParam *p, const char *val, size_t len)
{
    while (len && is_space(val[len - 1])) --len;     /* trailing space */

    switch (p->kind) {
    case JCE_SCRIPT_PARAM_NUMBER:
    case JCE_SCRIPT_PARAM_ENTITY: {
        char buf[64];
        if (len >= sizeof buf) return;
        memcpy(buf, val, len);
        buf[len] = '\0';
        char *endp = NULL;
        const double d = strtod(buf, &endp);
        if (endp == buf) return;                     /* not a number at all */
        if (p->kind == JCE_SCRIPT_PARAM_NUMBER) p->number = (float)d;
        else if (d >= 0.0)                      p->entity = (uint64_t)d;
        break;
    }
    case JCE_SCRIPT_PARAM_BOOL:
        /* `true` and `1` both, because seven languages spell it both ways and
         * an author should not have to know which one this scanner prefers. */
        if ((len == 4 && strncmp(val, "true", 4) == 0) ||
            (len == 1 && val[0] == '1'))
            p->number = 1.0f;
        break;
    case JCE_SCRIPT_PARAM_TEXT:
        if (len >= sizeof p->text) len = sizeof p->text - 1;
        memcpy(p->text, val, len);
        p->text[len] = '\0';
        break;
    default:
        break;
    }
}

int jce_script_exports_scan(const char *source, size_t len,
                            JceScriptParam *out, int max_out)
{
    if (!source || !out || max_out <= 0) return -1;

    int         found = 0;
    const char *p     = source;
    const char *end   = source + len;

    while (p < end && found < max_out) {
        /* One line at a time: a declaration never spans lines, so the line
         * end is also the end of any default value. */
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;

        const char *m = NULL;
        for (const char *q = p; q + MARKER_LEN <= le; ++q) {
            if (memcmp(q, MARKER, MARKER_LEN) == 0) { m = q + MARKER_LEN; break; }
        }
        if (!m) { p = nl ? nl + 1 : end; continue; }

        char kind_word[16];
        char name[32];
        const char *c = skip_space(m, le);
        c = take_word(c, le, kind_word, sizeof kind_word);

        uint32_t kind = 0;
        if (!c || !kind_from_word(kind_word, &kind)) {
            p = nl ? nl + 1 : end;
            continue;                               /* unknown kind: skipped */
        }

        c = skip_space(c, le);
        /* The NAME stops at '=' as well as at whitespace, so `speed=5` parses
         * the same as `speed = 5`.  Without this the name would be "speed=5",
         * fail validation, and the declaration would vanish for a reason the
         * author cannot see. */
        {
            size_t n = 0;
            while (c < le && !is_space(*c) && *c != '=') {
                if (n + 1 >= sizeof name) { n = 0; break; }
                name[n++] = *c++;
            }
            name[n] = '\0';
        }
        if (!name_is_valid(name)) { p = nl ? nl + 1 : end; continue; }

        /* FIRST DECLARATION WINS -- see the header. */
        bool dup = false;
        for (int i = 0; i < found; ++i)
            if (strcmp(out[i].name, name) == 0) { dup = true; break; }
        if (dup) { p = nl ? nl + 1 : end; continue; }

        JceScriptParam *d = &out[found];
        memset(d, 0, sizeof *d);
        snprintf(d->name, sizeof d->name, "%s", name);
        d->kind = kind;

        c = skip_space(c, le);
        if (c < le && *c == '=') {
            c = skip_space(c + 1, le);
            if (c < le) apply_default(d, c, (size_t)(le - c));
        }

        ++found;
        p = nl ? nl + 1 : end;
    }

    return found;
}

bool jce_script_exports_declares(const JceScriptParam *decls, int decl_count,
                                 const char *name)
{
    if (!decls || decl_count <= 0 || !name || !name[0]) return false;
    for (int i = 0; i < decl_count; ++i)
        if (strcmp(decls[i].name, name) == 0) return true;
    return false;
}

int jce_script_exports_merge(const JceScriptParam *decls, int decl_count,
                             JceScriptParam *params, int count, int cap)
{
    if (!params || count < 0 || cap <= 0 || count > cap) return -1;
    if (!decls || decl_count <= 0) return count;

    for (int i = 0; i < decl_count; ++i) {
        const JceScriptParam *d = &decls[i];
        if (!d->name[0]) continue;

        int at = -1;
        for (int j = 0; j < count; ++j)
            if (strcmp(params[j].name, d->name) == 0) { at = j; break; }

        if (at >= 0) {
            /* ALREADY AUTHORED: take only the KIND.  The script decides what
             * the value means; the author decides what it is.  Overwriting
             * the value here would silently discard tuning every time the
             * editor redrew a component. */
            params[at].kind = d->kind;
            continue;
        }
        if (count >= cap) break;        /* full: the rest simply do not fit */
        params[count++] = *d;
    }
    return count;
}
