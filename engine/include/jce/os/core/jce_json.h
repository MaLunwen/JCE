/*
 * jce_json.h  Thin facade over cJSON.
 *
 * Goals:
 *   1. Public engine/editor code does not include <cjson/cJSON.h>.
 *   2. Common access patterns (typed-default get, vec3/vec4, "any-of-keys")
 *      collapse to one helper instead of being re-implemented per TU.
 *   3. File I/O (parse a path, print to a path) is one call.
 *
 * `JceJson` is an opaque handle.  It is a forward-declared alias of
 * cJSON, the underlying parser library.  Public consumers cannot access
 * any field on it without separately including <cjson/cJSON.h> and
 * linking the cjson library — which is forbidden by Phase A of the ABI
 * firewall.  Internally, engine bridge TUs that already include cjson
 * may treat JceJson and cJSON as the same struct.
 */

#ifndef JCE_JSON_H
#define JCE_JSON_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>

JCE_EXTERN_C_BEGIN

/* Opaque alias: layout is cJSON.  Do NOT introspect from public code. */
typedef struct cJSON JceJson;

/* ── Lifecycle ─────────────────────────────────────────────────────── */

/* Parse text (NUL-terminated) or buffer of `len` bytes.  Returns NULL on
 * failure.  When `len == 0`, treats `text` as NUL-terminated. */
JCE_API JceJson *jce_json_parse(const char *text, size_t len);

/* Parse exactly one JSON value.  Unlike jce_json_parse(), trailing non-space
 * bytes are rejected.  `len == 0` treats text as NUL-terminated.  This is the
 * right entry point for trust-boundary data such as project/script assets;
 * the lenient function remains for backward-compatible document readers. */
JCE_API JceJson *jce_json_parse_strict(const char *text, size_t len);

/* Read a file from disk and parse it.  Returns NULL on I/O or parse fail;
 * jce_json_last_error() tells those two apart. */
JCE_API JceJson *jce_json_parse_file(const char *path);

/* ── Parse-failure diagnostics ─────────────────────────────────────── */

/* Why the most recent jce_json_parse / jce_json_parse_file call on THIS
 * thread returned NULL, e.g.
 *   malformed JSON: line 42, column 7 (byte 1180), near ,"tint": [1,
 *   cannot open file: No such file or directory
 * Never NULL; "" when that call succeeded, or when this thread has not
 * parsed anything yet.
 *
 * Threading: the record is thread-local, so an asset-loader thread cannot
 * clobber the message the main thread is about to read.  The flip side is
 * that it must be read on the SAME thread that received the NULL — reading
 * it after a job-system hop yields the other thread's (probably empty)
 * record.  The string is owned by the facade and is overwritten by the next
 * parse on this thread; copy it if you keep it.
 *
 * cJSON's own cJSON_GetErrorPtr() is deliberately not surfaced: it reads a
 * plain global inside cJSON, which is already wrong the moment two threads
 * parse at once — and the engine does parse JSON off the main thread. */
JCE_API const char *jce_json_last_error(void);

/* Byte offset into the parsed text at which the parser stopped, for that
 * same record.  (size_t)-1 when the failure was not a parse failure (absent
 * or unreadable file, NULL input) or when there was no failure — which also
 * makes this the "could not read it" vs "read it but it is broken" test. */
JCE_API size_t JCE_CALL jce_json_last_error_offset(void);

/* Construct empty container nodes. */
JCE_API JceJson *jce_json_object(void);
JCE_API JceJson *jce_json_array(void);
JCE_API JceJson *jce_json_number(double v);
JCE_API JceJson *jce_json_string(const char *v);
JCE_API JceJson *jce_json_bool(bool v);

/* Serialise a tree.  Returned string must be freed with
 * jce_json_free_string().  When `pretty` is false, output is compact. */
JCE_API char    *jce_json_print(const JceJson *root, bool pretty);

/* Serialise to a file.  When `take_ownership` is true the tree is freed
 * regardless of success.  Returns true on success. */
JCE_API bool JCE_CALL jce_json_write_file(const char *path, JceJson *root,
                             bool pretty, bool take_ownership);

JCE_API void JCE_CALL jce_json_free(JceJson *root);
JCE_API void JCE_CALL jce_json_free_string(char *s);

/* ── Type tests / navigation ───────────────────────────────────────── */

JCE_API bool JCE_CALL jce_json_is_object(const JceJson *j);
JCE_API bool JCE_CALL jce_json_is_array(const JceJson *j);
JCE_API bool JCE_CALL jce_json_is_number(const JceJson *j);
JCE_API bool JCE_CALL jce_json_is_string(const JceJson *j);
JCE_API bool JCE_CALL jce_json_is_bool(const JceJson *j);
JCE_API bool JCE_CALL jce_json_is_null(const JceJson *j);

JCE_API JceJson *jce_json_get(const JceJson *obj, const char *key);   /* case-sensitive */
JCE_API bool JCE_CALL jce_json_has(const JceJson *obj, const char *key);

/* Remove (and free) a member by key.  No-op when the key is absent.  Use
 * before a jce_json_set_* call to replace a key without leaving a duplicate
 * (the set helpers append rather than overwrite). */
JCE_API void JCE_CALL jce_json_remove(JceJson *obj, const char *key);

/* Detach `child` from `parent` WITHOUT freeing it: ownership transfers to the
 * caller, which must either re-attach it (jce_json_array_push /
 * jce_json_set_child) or free it (jce_json_free).
 *
 * This is the move primitive — the difference from jce_json_remove is that
 * remove destroys the subtree.  Needed to relocate a node between documents
 * without a print/re-parse round trip; the world partitioner moves entity
 * nodes from the master scene into per-cell fragments this way.  Without it
 * such callers had to reach past the facade for cJSON_DetachItemViaPointer.
 *
 * No-op if either argument is NULL or `child` is not a child of `parent`. */
JCE_API void JCE_CALL jce_json_detach(JceJson *parent, JceJson *child);

JCE_API int JCE_CALL jce_json_array_size(const JceJson *arr);
JCE_API JceJson *jce_json_array_at(const JceJson *arr, int index);

/* Object iteration.  Use as:
 *   for (JceJson *it = jce_json_first_child(obj); it;
 *        it = jce_json_next_sibling(it)) {
 *       const char *key = jce_json_member_key(it);
 *       ...
 *   }
 */
JCE_API JceJson    *jce_json_first_child  (const JceJson *obj);
JCE_API JceJson    *jce_json_next_sibling (const JceJson *node);
JCE_API const char *jce_json_member_key   (const JceJson *node);  /* may be NULL for array items */
JCE_API const char *jce_json_string_value (const JceJson *node, const char *def);
JCE_API double JCE_CALL jce_json_number_value(const JceJson *node, double def);
JCE_API bool JCE_CALL jce_json_bool_value(const JceJson *node, bool def);

/* ── Typed accessors with defaults ─────────────────────────────────── */

JCE_API double JCE_CALL jce_json_get_number(const JceJson *obj, const char *key, double def);
JCE_API int JCE_CALL jce_json_get_int(const JceJson *obj, const char *key, int def);
JCE_API bool JCE_CALL jce_json_get_bool(const JceJson *obj, const char *key, bool def);
JCE_API const char *jce_json_get_string(const JceJson *obj, const char *key,
                                const char *def);

/* "Any-of" variants: try each key in order; returns first hit or default. */
JCE_API double JCE_CALL jce_json_get_number_any(const JceJson *obj,
                                    const char *const *keys, int n, double def);
JCE_API const char *jce_json_get_string_any(const JceJson *obj,
                                    const char *const *keys, int n,
                                    const char *def);

/* Read N consecutive floats from an array member.  Missing or
 * non-numeric elements fall back to `def[i]` (or 0.0f when def==NULL). */
JCE_API void JCE_CALL jce_json_get_floats(const JceJson *obj, const char *key,
                             float *out, int n, const float *def);

/* Read "<prefix>X","<prefix>Y","<prefix>Z" pattern (e.g. posX/posY/posZ).
 * Falls back to def[i] component-wise. */
JCE_API void JCE_CALL jce_json_get_xyz(const JceJson *obj, const char *prefix,
                          float out[3], const float def[3]);
JCE_API void JCE_CALL jce_json_get_xyzw(const JceJson *obj, const char *prefix,
                           float out[4], const float def[4]);

/* ── Builders (also collapse the cJSON_AddX one-liner family) ─────── */

JCE_API void JCE_CALL jce_json_set_number(JceJson *obj, const char *key, double v);
JCE_API void JCE_CALL jce_json_set_int(JceJson *obj, const char *key, int v);
JCE_API void JCE_CALL jce_json_set_bool(JceJson *obj, const char *key, bool v);
JCE_API void JCE_CALL jce_json_set_string(JceJson *obj, const char *key, const char *v);
/* Attach a child object/array; ownership transfers to parent. */
JCE_API void JCE_CALL jce_json_set_child(JceJson *obj, const char *key, JceJson *child);
JCE_API void JCE_CALL jce_json_array_push(JceJson *arr, JceJson *item);
JCE_API void JCE_CALL jce_json_array_push_number(JceJson *arr, double v);
JCE_API void JCE_CALL jce_json_array_push_string(JceJson *arr, const char *v);

/* Symmetric helpers for the xyz / xyzw write side. */
JCE_API void JCE_CALL jce_json_set_xyz(JceJson *obj, const char *prefix, const float v[3]);
JCE_API void JCE_CALL jce_json_set_xyzw(JceJson *obj, const char *prefix, const float v[4]);
JCE_API void JCE_CALL jce_json_set_float_array(JceJson *obj, const char *key,
                                  const float *v, int n);

JCE_EXTERN_C_END

#endif /* JCE_JSON_H */
