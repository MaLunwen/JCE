/* jce_script_api_internal.h — the handle's layout. NOT installed, NOT exported.
 *
 * `JceScriptApi` is opaque in the public header on purpose: the binding side
 * of this ABI is ctypes / JNA / JNI code that must never know a field offset,
 * because knowing one would make the layout part of the contract and this
 * struct would then be unable to grow.
 */
#ifndef JCE_SCRIPT_API_INTERNAL_H
#define JCE_SCRIPT_API_INTERNAL_H

#include <jce/script_api/jce_script_api.h>

struct JceScriptApi {
    /* A COPY, clamped to min(caller, library) over a zeroed table by
     * jce_script_api_open — never a pointer to the caller's table.  The
     * caller may build the host on its stack, and every forwarder null-checks
     * the member it is about to call precisely because a short caller leaves
     * the tail NULL. */
    JceScriptHost host;
};

#endif /* JCE_SCRIPT_API_INTERNAL_H */
