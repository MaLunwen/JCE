/*
 * jce_script_js_internal.h — what the js backend's translation units share.
 *
 * PRIVATE TO scripting/js/src/.  It is not installed, not reachable from
 * <jce/...>, and no consumer may include it: the JceJsScript layout below is
 * a detail that changes whenever this backend changes, and the public promise
 * is jce_script_vm_js.h alone.
 *
 * It exists because the generated bindings need the struct.  Before the
 * generator produced them there was one translation unit and the struct lived
 * in it; jce_script_bindings_js.gen.c is the second, and a struct copied into
 * two files is a struct that will eventually differ between them.  Lua's
 * jce_script_internal.h exists for the same reason and says the same thing.
 */

#ifndef JCE_SCRIPT_JS_INTERNAL_H
#define JCE_SCRIPT_JS_INTERNAL_H

#include <jce/middleware/script/jce_script.h>
#include <jce/middleware/script/jce_script_vm.h>

#include "quickjs.h"

#include <stdbool.h>

/* ── The handle ──────────────────────────────────────────────────────────
 *
 * JceScriptVMHeader FIRST.  jce_script_vm_create() refuses a handle whose
 * first word is not the table it dispatched through, because every forwarder
 * reads the vtable from offset 0. */
typedef struct JceJsScript {
    JceScriptVMHeader hdr;
    JceScriptHost     host;
    bool              have_host;

    JSRuntime        *rt;
    JSContext        *ctx;

    /* Instances.  A handle is (index + 1) so 0 stays "no instance", which is
     * what every caller tests.  Freed slots are JS_UNDEFINED and reused, so a
     * scene that spawns and despawns does not grow this forever. */
    JSValue          *inst;
    int               inst_cap;
    int               inst_live;
} JceJsScript;

#endif /* JCE_SCRIPT_JS_INTERNAL_H */
