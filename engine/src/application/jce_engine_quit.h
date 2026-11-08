/* Programmatic quit owner; shared terminal result, no public ABI here. */
#ifndef JCE_ENGINE_QUIT_H
#define JCE_ENGINE_QUIT_H
#include <jce/application/jce_engine.h>
/* Keep CONTINUE nonterminal; a requested FAILURE wins over terminal success. */
JceAppResult jce_engine_quit_resolve(JceAppResult result);
/* Only the completed engine teardown releases a request session. */
void jce_engine_quit_end_session(void);
#endif
