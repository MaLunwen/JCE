/* jce_script_py_mock.h — the recording mock host's exported surface.
 *
 * HAND-WRITTEN.  The mock's 70 member bodies are generated into
 * jce_script_py_mock.gen.c from the same manifest the bindings come from; this
 * header is the handful of entry points its two consumers call, and it is not
 * generated because nothing in the manifest describes it.
 *
 * Two consumers, one object.  That is the whole point of the mock being a
 * SHARED library rather than a static one:
 *
 *   lua_case_runner.c   installs jce_mock_host() into jce_script_create_sized
 *                       and drives the Lua bindings;
 *   differential.py     passes the SAME table to jce_script_api_open through
 *                       ctypes and drives the Python bindings.
 *
 * Two mocks written to agree would be a second two-sided contract, and this
 * repository's standing failure is exactly that shape: two sides that agree
 * with each other and are both wrong.
 *
 * No __declspec here on purpose.  The .gen.c defines these with
 * dllexport/visibility("default"); the runner links the import library and
 * needs only a declaration, and marking this one dllimport would make the
 * header unusable from the translation unit that defines them.
 */
#ifndef JCE_SCRIPT_PY_MOCK_H
#define JCE_SCRIPT_PY_MOCK_H

#include <jce/middleware/script/jce_script.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Kept in sync with differential.py's MODES by that file's check_mock_modes,
 * which reads this header and that dict and fails naming the one that moved. */
#define JCE_MOCK_MODE_OK        0
#define JCE_MOCK_MODE_MISS      1
#define JCE_MOCK_MODE_ABSENT    2
#define JCE_MOCK_MODE_NORELEASE 3

/* The host table for `mode`.  Rewritten in place on every call, so a caller
 * that wants two modes at once needs two processes — which is what the
 * differential does anyway, one run per mode. */
const JceScriptHost *jce_mock_host(int mode);

/* The CALLER's sizeof(JceScriptHost), for jce_script_api_open / _create_sized.
 * Exported rather than computed on the Python side because Python has no
 * business knowing this struct's layout: script-api.json deliberately
 * publishes none, and a summed size would be the exact bug
 * jce_script_create_sized exists to prevent. */
size_t jce_mock_host_size(void);

/* sizeof(JceScriptRaycastHit), so the ctypes mirror can be checked against the
 * C struct rather than against a sentence claiming the layouts match. */
size_t jce_mock_sizeof_raycast_hit(void);

/* Clear the trace, select a mode, and zero the allocation ledger. */
void jce_mock_reset(int mode);

/* Append one line to the trace verbatim.  This is the Python side's wire; the
 * Lua side writes the same lines through jce.log, whose mock body calls the
 * same appender, so the two streams are directly comparable. */
void jce_mock_note(const char *text);

/* The whole recorded stream: CASE / RESULT lines interleaved with CALL lines
 * in the order they happened. */
const char *jce_mock_trace_text(void);

/* Owned strings produced and not yet released.  Zero after a well-behaved
 * owned_string_release call — this is how the release is SEEN rather than
 * assumed, on both sides. */
int jce_mock_live_strings(void);

/* Make owned strings longer than the Python binding's initial copy-out buffer,
 * to reach the C ABI's retry path. */
void jce_mock_set_long_strings(int on);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SCRIPT_PY_MOCK_H */
