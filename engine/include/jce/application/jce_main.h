/*
 * jce_main.h  Public, SDL-free entry-point glue for JCE applications.
 *
 * Usage (in exactly one .c TU per application):
 *
 *     #include <jce/application/jce_main.h>
 *     #include "my_app.h"
 *
 *     JCE_MAIN(my_app_get_desc)
 *
 * where `my_app_get_desc` returns a JceAppDesc.
 *
 * This header does NOT include SDL.  The SDL3 callback backend lives
 * in engine/src/application/jce_main_sdl.c (compiled into jce_application), which
 * calls back into the application-defined factory `jce_app_get_desc`.
 *
 * Phase A ABI firewall — see THIRD_PARTY_LICENSES.md and the engine
 * cross-platform contract.  Treat this header as the only public
 * touch-point for application bootstrap.
 */

#ifndef JCE_MAIN_H
#define JCE_MAIN_H

#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_engine.h>
#include <jce/os/core/jce_defs.h>

JCE_EXTERN_C_BEGIN

/*
 * The application must provide exactly one definition of this symbol.
 * The JCE_MAIN() macro below generates it by forwarding to a
 * user-named factory.  Defining it manually is also legal.
 */
JCE_API JceAppDesc jce_app_get_desc(void);

JCE_EXTERN_C_END

/*
 * JCE_MAIN(get_desc_fn)
 *
 * Emits a `jce_app_get_desc` definition that forwards to the
 * user-named factory.  Place this in exactly one application TU.
 */
#define JCE_MAIN(get_desc_fn)                                  \
    JceAppDesc get_desc_fn(void);                              \
    JceAppDesc jce_app_get_desc(void)                          \
    {                                                          \
        return get_desc_fn();                                  \
    }

#endif /* JCE_MAIN_H */
