/* jce_java_diff_host.gen.h -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * The recording mock host, shared by BOTH sides of the cross-language
 * differential: the Lua reference driver links it and so does the JNI helper
 * the Java driver loads. ONE implementation, compiled twice -- so a trace
 * difference between the two processes is a difference in what the BINDINGS
 * did, never a difference in what the mock did.
 */
#ifndef JCE_JAVA_DIFF_HOST_GEN_H
#define JCE_JAVA_DIFF_HOST_GEN_H

#include <jce/middleware/script/jce_script.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Every member mocked; every call traced. */
const JceScriptHost *jce_java_diff_host_full(void);

/* Only `log` and `user`. Every one of the generated entries takes its absent
 * path, which is where absent_value, clamp_min and the host guard live. */
const JceScriptHost *jce_java_diff_host_partial(void);

size_t jce_java_diff_host_size(void);

/* The host-call trace since the last reset, and the Lua probe output. */
const char *jce_java_diff_trace(void);
const char *jce_java_diff_out(void);
void        jce_java_diff_reset(void);
void        jce_java_diff_tracef(const char *fmt, ...);

/* The case table, in ONE order both drivers walk. */
int         jce_java_diff_case_count(void);
const char *jce_java_diff_case_label(int i);

#ifdef __cplusplus
}
#endif

#endif /* JCE_JAVA_DIFF_HOST_GEN_H */
