/* jce_script_api_core.c — the three entry points that are not a manifest
 * entry: the version handshake, and the handle's lifetime.
 *
 * Hand-written, and small on purpose.  Everything that varies with the
 * scripting surface is generated (jce_script_api.gen.c); what is here is the
 * part that does not change when a host member is added.
 *
 * calloc/free rather than the engine's allocator, deliberately: this library
 * links NO engine layer.  It is pure marshalling glue — every call it serves
 * ends in a function pointer the host supplied — and that is what makes "the
 * shared object's exports are the whole scripting surface" structurally true
 * rather than a promise.  jce_alloc lives in jce_core, and linking jce_core to
 * reach it would put the engine inside the boundary this library exists to
 * draw.  (scripts/lint/check_raw_allocator.py scans engine/src and editor/src;
 * this file is outside both, and the reason above is why it should stay so.)
 */

#include "jce_script_api_internal.h"

#include <stdlib.h>
#include <string.h>

uint32_t jce_script_api_version(void)
{
    return JCE_SCRIPT_API_VERSION;
}

JceScriptApi *jce_script_api_open(const JceScriptHost *host,
                                  size_t host_size,
                                  uint32_t script_api_min)
{
    JceScriptApi *api;
    size_t copy;

    if (!host || host_size == 0u)
        return NULL;

    /* Newer binding, older library: refuse.  It cannot degrade — the binding
     * was generated against entries this library does not implement, and the
     * first call to one would be a call through a symbol that is not here.
     * The loader reports both numbers; jce_script_api_version() is callable
     * without a handle for exactly that message. */
    if (script_api_min > JCE_SCRIPT_API_VERSION)
        return NULL;

    api = (JceScriptApi *)calloc(1u, sizeof *api);
    if (!api)
        return NULL;

    /* min(caller, library) over a ZEROED table — the same contract as
     * jce_script_create_sized.  JceScriptHost is allocated by the CALLER and
     * gains members over releases, so copying at this library's own sizeof
     * would read past the end of a host built against an older header and
     * file whatever followed it under the newest member — which is a FUNCTION
     * POINTER the next call would jump through.  Sizes come from sizeof, never
     * from summing members. */
    copy = (host_size < sizeof api->host) ? host_size : sizeof api->host;
    memcpy(&api->host, host, copy);
    return api;
}

void jce_script_api_close(JceScriptApi *api)
{
    free(api);
}
