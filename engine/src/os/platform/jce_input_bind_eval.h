/*
 * jce_input_bind_eval.h  The binding evaluator (engine-internal).
 *
 * ONE pure function of (binding, input, player).  Deadzone, saturation, curve,
 * axis side, radial pairing, hat direction and delta-vs-absolute live here for
 * the DEVICE layer and for this evaluator -- jce_input_device_stick()/_trigger()
 * call the shaping helpers below rather than carrying a second copy of the
 * maths.  NOT yet for the whole engine: jce_input_actions.c's
 * evaluate_binding() still computes its own per-axis remap and is still the one
 * jce_actions_update() runs.  Task 11 deletes that copy; saying "nowhere else"
 * before it lands would be a contract nothing enforces.  Two implementations of
 * a deadzone is how a stick ends up feeling different depending on which API
 * you asked, which is why the count is stated rather than assumed.
 *
 * WHO READS THIS TODAY, stated as the tree stands rather than as a promise:
 * jce_input_devices.c (the two shaping helpers) and
 * tests/os/platform/test_jce_input_bind_eval.c.  jce_actions_update() still
 * runs jce_input_actions.c's own evaluate_binding(); moving it onto this
 * function is Plan B Task 11, and until it lands NOTHING a player touches goes
 * through jce_input_bind_eval().
 *
 * NOT under <jce/...>: this is a seam between two engine TUs, not public API.
 *
 * Layer: OS Abstraction (Layer 1).
 */

#ifndef JCE_INPUT_BIND_EVAL_H
#define JCE_INPUT_BIND_EVAL_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/platform/jce_input_actions.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

/* jce_input_actions.h has already supplied this through jce_input_device.h.
 * C99 forbids a repeated typedef, so the shared guard is what makes a third
 * spelling of it harmless rather than a diagnostic on a strict compiler. */
#ifndef JCE_INPUT_TYPEDEF_DEFINED
#define JCE_INPUT_TYPEDEF_DEFINED
typedef struct JceInput JceInput;
#endif

/* What one binding contributed this frame.
 *
 * `digital` and `analog` are separate on purpose: the press contract is
 * `down = digital_down || analog_latch`, and collapsing them is exactly the
 * `down == (value != 0)` bug -- a stick at 0.02 of drift firing an action. */
typedef struct JceBindEval {
    float value;       /* signed scalar contribution, scale applied          */
    float x, y;        /* 2D contribution, scale applied                     */
    float delta;       /* JCE_BINDF_DELTA contribution, UNCLAMPED, not in
                          `value` -- px/frame for a mouse, do NOT multiply
                          by dt                                              */
    float activation;  /* 0..1 pre-sign, PRE-SCALE magnitude: what the analog
                          latch sees.  Pre-scale on every path on purpose --
                          computed after `scale` it would make the same physical
                          push clear a threshold at scale 2 and miss it at 0.5.
                          ONE exception, and it is not normalisable: a
                          JCE_SRC_MOUSE_AXIS binding reports px/frame, which has
                          no full-scale to divide by                     */
    int   channel;     /* JceBindChannel this binding reported               */
    bool  digital;     /* a digital source of this binding is physically down */
    bool  analog;      /* this binding has a live analog source              */
    bool  xy;          /* the contribution landed in x/y, not in value       */
    bool  vector2d;    /* the contribution is a declared 2D vector           */
} JceBindEval;

/* Evaluate one binding for one player slot.  `out` is fully overwritten; a
 * NULL binding or input yields a zeroed result, never UB. */
void jce_input_bind_eval(const JceBinding *b, const JceInput *in, int player,
                         JceBindEval *out);

/* Radial stick shaping: the dead region is a property of the STICK, so the
 * test is on the MAGNITUDE.  Testing each axis separately carves a SQUARE dead
 * region out of a round stick, which is what produces cardinal snapping. */
void jce_input_shape_stick(float x, float y, float inner, float outer,
                           float curve, float *out_x, float *out_y);

/* Unipolar (trigger) shaping: 0..1 in, 0..1 out. */
float jce_input_shape_unipolar(float v, float inner, float outer, float curve);

JCE_EXTERN_C_END

#endif /* JCE_INPUT_BIND_EVAL_H */
