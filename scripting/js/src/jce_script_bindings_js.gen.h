/* jce_script_bindings_js.gen.h - GENERATED. DO NOT EDIT.
 *   python tools/scriptgen/gen_script_bindings.py --write
 */
#ifndef JCE_SCRIPT_BINDINGS_JS_GEN_H
#define JCE_SCRIPT_BINDINGS_JS_GEN_H

#include "jce_script_js_internal.h"

#define JCE_SCRIPT_JS_BINDING_COUNT 101

/* Registration order matches script_exposure.json's expose[] order. */
extern const char *const JCE_SCRIPT_JS_BINDING_NAMES[JCE_SCRIPT_JS_BINDING_COUNT];

/* Installs the global `jce` object into s->ctx, with every generated
 * binding and then the manifest's constants.  False only when the
 * context could not allocate the object. */
bool jce_script_js_install_bindings(JceJsScript *s);

#endif /* JCE_SCRIPT_BINDINGS_JS_GEN_H */
