/*
 * jce_postfx_apply.h  PostFX stack → render-graph pass list.
 *
 * Consumes JcePostFxStackComponent (B10.2 data layer) and emits a
 * declarative pass-order list the render graph executes in turn.
 * Each emitted JcePostFxPassDesc carries:
 *   - kind (which effect)
 *   - normalised parameter struct (already gain-baked + smooth-curved)
 *   - input + output RT handle slot
 *
 * The render-graph node table in B17 wire-up routes these descs to
 * the corresponding bgfx submission pass.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_POSTFX_APPLY_H
#define JCE_POSTFX_APPLY_H

#include <jce/middleware/scene/jce_postfx_stack.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_POSTFX_MAX_PASSES 8

typedef enum {
    JCE_POSTFX_PASS_NONE     = 0,
    JCE_POSTFX_PASS_BLOOM    = 1,
    JCE_POSTFX_PASS_CA       = 2,
    JCE_POSTFX_PASS_VIGNETTE = 3,
    JCE_POSTFX_PASS_GRAIN    = 4,
    JCE_POSTFX_PASS_COLORADJ = 5,
} JcePostFxPassKind;

typedef struct {
    JcePostFxPassKind kind;
    /* Effect-specific tuned parameters as four floats (kind decodes). */
    float params0[4];
    float params1[4];
    /* RT slot indices into the render graph's transient pool. */
    uint8_t input_slot;
    uint8_t output_slot;
} JcePostFxPassDesc;

typedef struct {
    JcePostFxPassDesc passes[JCE_POSTFX_MAX_PASSES];
    uint8_t           pass_count;
    /* Effective exposure gain (color_adjust.exposure_ev linearised). */
    float             exposure_gain;
} JcePostFxPipeline;

/* Build the pass list from a stack.  `out` is cleared first.  Pass
 * ordering follows HDRP convention: exposure → bloom → CA → vignette
 * → grain → color adjust. */
JCE_API void jce_postfx_apply_build(const JcePostFxStackComponent *stack,
                                      JcePostFxPipeline             *out);

/* Convenience — short label for the pass kind (UI / debug output). */
JCE_API const char *jce_postfx_pass_kind_name(JcePostFxPassKind k);

JCE_EXTERN_C_END

#endif /* JCE_POSTFX_APPLY_H */
