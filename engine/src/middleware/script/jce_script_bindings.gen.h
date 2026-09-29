/* jce_script_bindings.gen.h — GENERATED. DO NOT EDIT.
 *   python tools/scriptgen/gen_script_bindings.py --write
 */
#ifndef JCE_SCRIPT_BINDINGS_GEN_H
#define JCE_SCRIPT_BINDINGS_GEN_H

#include "jce_script_internal.h"

#define JCE_SCRIPT_GENERATED_BINDING_COUNT 101

/* Registration order matches script_exposure.json's expose[] order. */
extern const char *const JCE_SCRIPT_GENERATED_BINDING_NAMES[JCE_SCRIPT_GENERATED_BINDING_COUNT];

/* Installs every generated binding into the table on top of s->L's
 * stack, then the manifest's constants. */
void jce_script_install_generated_bindings(JceScript *s);

#endif /* JCE_SCRIPT_BINDINGS_GEN_H */
