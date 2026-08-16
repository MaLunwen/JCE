/*
 * jce_input_bind_eval.c  The binding evaluator.
 *
 * Pure: no SDL, no globals, no hardware, no allocation.  Everything it knows
 * about the world arrives through `const JceInput *`.
 *
 * ONE DEAD REGION FOR THE DEVICE LAYER AND THIS EVALUATOR -- not yet for the
 * engine, and the difference is the whole of Task 11.  shape_travel() below is
 * the only magnitude-to-0..1-travel arithmetic on THIS path;
 * jce_input_devices.c's jce_input_device_stick() and _trigger() reach it
 * through the two jce_input_shape_* helpers.  A SECOND copy is still live in
 * jce_input_actions.c: evaluate_binding() computes `(fabsf(v) - dz) / (1.0f -
 * dz)` at line 392 and jce_actions_update() still runs it, so it is that copy,
 * not this one, that every action a player touches goes through today.  Plan B
 * Task 11 switches jce_actions_update() onto jce_input_bind_eval() and deletes
 * it; until then "the only place in the engine" would be a comment nothing
 * enforces, which is why it does not say that.
 *
 * THE NUMBERS ARE CHOSEN, NOT MEASURED.  The dead-region defaults this file
 * resolves against (stick_inner 0.15, trigger_inner 0.02) come from
 * deadzone_defaults() in jce_input_devices.c and nobody has measured a pad of
 * this owner's.  0.15 is continuous with what evaluate_binding() already
 * applies, which is why moving onto these changes the dead region's SHAPE and
 * not its size.
 */

#include "jce_input_bind_eval.h"

#include <jce/os/platform/jce_gamepad.h>
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_device.h>
#include <jce/os/platform/jce_keys.h>

#include <math.h>
#include <string.h>

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Magnitude -> 0..1 travel.  The ONE place a dead region is applied. */
static float shape_travel(float mag, float inner, float outer, float curve)
{
    float t;
    /* A CROSSED zone inverts the axis; it does NOT produce a NaN, and saying so
     * mattered enough to get it wrong once.  The early-out below returns before
     * the division whenever mag <= inner, so the numerator is strictly positive
     * by the time it is divided and 0/0 is unreachable:
     *   outer == inner -- +inf, which clampf() pins to 1.0.  The guard changes
     *     nothing observable here, which is why an inner == outer test cannot
     *     tell whether this line exists.
     *   outer <  inner -- the denominator is NEGATIVE, so t is negative and the
     *     shaped value would point the OPPOSITE WAY.  clampf() currently pins
     *     that to 0, so the symptom is a binding that is silently always dead
     *     rather than one that is inverted -- but the clamp is what is holding
     *     it, and a clamp is not a reason.
     * So this line is the reason, and the crossed case is the only input that
     * reaches it: test_an_inverted_zone_does_not_invert_the_axis authors inner
     * 0.5 / outer 0.2 at 0.6 and reddens if this line is removed.  A degenerate
     * zone therefore collapses to a near-binary switch, the same answer
     * jce_input_device_set_deadzone() gives a crossed PROFILE. */
    if (outer <= inner) outer = inner + 1.0e-3f;
    if (mag <= inner)   return 0.0f;
    t = (mag - inner) / (outer - inner);
    t = clampf(t, 0.0f, 1.0f);
    if (curve > 0.0f && curve != 1.0f) t = powf(t, curve);
    return t;
}

void jce_input_shape_stick(float x, float y, float inner, float outer,
                           float curve, float *out_x, float *out_y)
{
    float mag = sqrtf(x * x + y * y);
    float t;
    if (out_x) *out_x = 0.0f;
    if (out_y) *out_y = 0.0f;
    if (!(mag > 1.0e-8f)) return;      /* also refuses a NaN magnitude */
    t = shape_travel(mag, inner, outer, curve);
    if (out_x) *out_x = (x / mag) * t;
    if (out_y) *out_y = (y / mag) * t;
}

float jce_input_shape_unipolar(float v, float inner, float outer, float curve)
{
    if (!(v > 0.0f)) return 0.0f;      /* also refuses a NaN reading */
    return shape_travel(v, inner, outer, curve);
}

/* Which HALF of a bipolar axis feeds this binding.  A NEG binding is BLIND to
 * the positive half of its own axis, which is the entire fix for "pull the
 * stick back and walk forward". */
static float apply_side(float signed_v, int side)
{
    switch (side) {
    case JCE_AXIS_SIDE_POS:      return signed_v > 0.0f ?  signed_v : 0.0f;
    case JCE_AXIS_SIDE_NEG:      return signed_v < 0.0f ? -signed_v : 0.0f;
    case JCE_AXIS_SIDE_UNIPOLAR: return signed_v > 0.0f ?  signed_v : 0.0f;
    case JCE_AXIS_SIDE_FULL:
    default:                     return signed_v;
    }
}

/* Does this binding address its device by ORDINAL rather than by semantic name?
 *
 * A COMPOSITE has no axis or button of its own; it addresses whatever its
 * SUB-SOURCES address.  Deciding from the top-level type alone made the
 * predicate false for every composite, so a composite always resolved the
 * player's GAMEPAD -- while eval_sub() below has handled JCE_SRC_JOY_BUTTON and
 * JCE_SRC_JOY_AXIS since this file landed.  On a machine whose only device is a
 * stick, a HOTAS or a wheel there is no gamepad to resolve, so those two arms
 * read a silent zero: a seam that was built and dead at the same time. */
static int binding_is_ordinal(const JceBinding *b)
{
    int i;
    if (b->type == JCE_SRC_JOY_BUTTON || b->type == JCE_SRC_JOY_AXIS ||
        b->type == JCE_SRC_JOY_HAT)
        return 1;
    if (b->type == JCE_SRC_COMPOSITE) {
        for (i = 0; i < 4; ++i)
            if (b->comp[i].type == JCE_SRC_JOY_BUTTON ||
                b->comp[i].type == JCE_SRC_JOY_AXIS)
                return 1;
    }
    return 0;
}

/* The device this binding addresses: the Nth device of the matching class in
 * the binding's player's set.  player == JCE_INPUT_PLAYER_NONE means "the
 * querying player", which is what makes a schema-1 map -- which carries no
 * player field at all -- behave exactly as it does today for one player. */
static JceDeviceId bind_device(const JceBinding *b, const JceInput *in, int player)
{
    int p = (b->player == (int8_t)JCE_INPUT_PLAYER_NONE) ? player : (int)b->player;
    int ordinal = binding_is_ordinal(b);
    JceDeviceId id;

    if (ordinal) {
        /* Ordinals are valid on ANY device, including gamepads -- that is how
         * a pad's unmapped MISC buttons are reached.  Prefer a raw joystick
         * (that is what an ordinal binding usually means) and fall back to the
         * player's gamepad rather than resolving to nothing. */
        id = jce_input_player_device_of_class(in, p, JCE_DEVCLASS_JOYSTICK,
                                              (int)b->device_slot);
        if (id != JCE_DEVICE_ID_NONE) return id;
        return jce_input_player_device_of_class(in, p, JCE_DEVCLASS_GAMEPAD,
                                                (int)b->device_slot);
    }
    return jce_input_player_device_of_class(in, p, JCE_DEVCLASS_GAMEPAD,
                                            (int)b->device_slot);
}

/* Resolve the dead region.  A NEGATIVE authored value means "use the device
 * profile".  ZERO is a real value and is never rewritten: that conflation is
 * why `deadzone: 0` could not be authored. */
static void resolve_zone(const JceBinding *b, const JceInput *in, JceDeviceId dev,
                         int is_trigger, float *inner, float *outer)
{
    JceInputDeadzone dz;
    memset(&dz, 0, sizeof dz);
    jce_input_device_get_deadzone(in, dev, &dz);
    *inner = (b->deadzone_inner < 0.0f)
           ? (is_trigger ? dz.trigger_inner : dz.stick_inner)
           : b->deadzone_inner;
    *outer = (b->deadzone_outer < 0.0f)
           ? (is_trigger ? dz.trigger_outer : dz.stick_outer)
           : b->deadzone_outer;
}

/* Shape one axis, radially when a companion axis is authored. */
static void eval_axis(const JceBinding *b, const JceInput *in, JceDeviceId dev,
                      float raw, float pair_raw, int has_pair, int is_trigger,
                      JceBindEval *out)
{
    float inner, outer, mag, t, shaped;

    if (b->flags & JCE_BINDF_RAW) {
        shaped = apply_side(raw, (int)b->side);
        out->activation = fabsf(shaped);
        out->value      = shaped * b->scale;
        out->analog     = true;
        return;
    }

    resolve_zone(b, in, dev, is_trigger, &inner, &outer);

    /* RADIAL when paired: the dead region belongs to the stick, not to one
     * axis.  Raw devices stay per-axis unless a pair is authored -- for a
     * wheel, axis 0 is steering and axis 1 is usually a pedal. */
    mag = has_pair ? sqrtf(raw * raw + pair_raw * pair_raw) : fabsf(raw);
    if (!(mag > 1.0e-8f)) return;

    t      = shape_travel(mag, inner, outer, b->curve);
    /* raw/mag is the DIRECTION cosine, so (0.9, 0.1) keeps its 6 degrees off
     * cardinal instead of being snapped square. */
    shaped = apply_side((raw / mag) * t, (int)b->side);

    out->activation = fabsf(shaped);
    out->value      = shaped * b->scale;
    out->analog     = true;
}

/* An ANALOG composite sub-source gets the same dead region as a top-level axis.
 *
 * It used to return the raw axis, which contradicted this file's headline twice
 * over: the dead region is supposed to live HERE and nowhere else, and a
 * composite built from two stick axes fed drift straight into x/y -- the exact
 * "a stick at 0.02 of drift firing an action" the digital/analog split exists to
 * prevent.  `analog` is raised only when a sub-source SURVIVES its dead region,
 * matching eval_axis(), which leaves it false for an axis at rest. */
static float shape_sub_axis(const JceBinding *b, const JceInput *in,
                            JceDeviceId dev, float signed_v, int *analog)
{
    float inner, outer, t, mag = fabsf(signed_v);

    if (b->flags & JCE_BINDF_RAW) {
        if (mag > 0.0f) *analog = 1;
        return signed_v;
    }
    if (!(mag > 1.0e-8f)) return 0.0f;
    resolve_zone(b, in, dev, 0, &inner, &outer);
    t = shape_travel(mag, inner, outer, b->curve);
    if (!(t > 0.0f)) return 0.0f;
    *analog = 1;
    return signed_v < 0.0f ? -t : t;
}

/* One composite sub-source -> 0..1. */
static float eval_sub(const JceBinding *b, const JceInputSource *s,
                      const JceInput *in, JceDeviceId dev,
                      int *digital, int *analog)
{
    /* JCE_SRC_NONE is the ONLY absence test.  A bare int of 0 used to mean
     * "absent", which made pad button 0 (SOUTH) unauthorable in a composite. */
    switch (s->type) {
    case JCE_SRC_KEY:
        if (jce_input_key_down(in, (JceKey)s->code)) { *digital = 1; return 1.0f; }
        return 0.0f;
    case JCE_SRC_MOUSE_BUTTON:
        if (jce_input_mouse_button(in, s->code)) { *digital = 1; return 1.0f; }
        return 0.0f;
    case JCE_SRC_PAD_BUTTON:
        if (jce_input_device_button(in, dev, (JceGamepadButton)s->code)) {
            *digital = 1; return 1.0f;
        }
        return 0.0f;
    case JCE_SRC_JOY_BUTTON:
        if (jce_input_device_ordinal_button(in, dev, s->code)) {
            *digital = 1; return 1.0f;
        }
        return 0.0f;
    case JCE_SRC_PAD_AXIS: {
        float v = jce_input_device_axis_raw(in, dev, (JceGamepadAxis)s->code);
        return shape_sub_axis(b, in, dev, apply_side(v, (int)s->side), analog);
    }
    case JCE_SRC_JOY_AXIS: {
        float v = jce_input_device_ordinal_axis(in, dev, s->code);
        return shape_sub_axis(b, in, dev, apply_side(v, (int)s->side), analog);
    }
    case JCE_SRC_NONE:
    default:
        return 0.0f;
    }
}

void jce_input_bind_eval(const JceBinding *b, const JceInput *in, int player,
                         JceBindEval *out)
{
    JceDeviceId dev;

    if (!out) return;
    memset(out, 0, sizeof *out);
    out->channel = JCE_CHAN_SCALAR;
    if (!b || !in) return;

    /* An authored scale of EXACTLY zero silences the binding on BOTH channels.
     * This is the documented "mute this binding" switch (it has its own test
     * since the composite work), and it is why the press contract's
     * scale-independence is about MAGNITUDE and SIGN, not about zero. */
    if (b->scale == 0.0f) return;

    dev = bind_device(b, in, player);

    switch (b->type) {
    case JCE_SRC_KEY:
        out->digital = jce_input_key_down(in, (JceKey)b->code);
        out->value   = out->digital ? b->scale : 0.0f;
        break;

    case JCE_SRC_MOUSE_BUTTON:
        out->digital = jce_input_mouse_button(in, b->code);
        out->value   = out->digital ? b->scale : 0.0f;
        break;

    case JCE_SRC_MOUSE_AXIS: {
        float dx = 0.0f, dy = 0.0f, v;
        jce_input_mouse_delta(in, &dx, &dy);
        v = (b->code == 0) ? dx
          : (b->code == 1) ? dy
                           : jce_input_mouse_wheel(in);
        out->value      = v * b->scale;
        /* PRE-scale, like every other path.  It is NOT 0..1 here and cannot be:
         * a mouse delta is px/frame and has no full-scale to normalise against,
         * which is the one documented exception to the field's range. */
        out->activation = fabsf(v);
        out->analog     = true;
        break;
    }

    case JCE_SRC_PAD_BUTTON:
        out->digital = jce_input_device_button(in, dev, (JceGamepadButton)b->code);
        out->value   = out->digital ? b->scale : 0.0f;
        break;

    case JCE_SRC_PAD_AXIS: {
        int   is_trigger = (b->code == JCE_GAMEPAD_AXIS_LEFT_TRIGGER ||
                            b->code == JCE_GAMEPAD_AXIS_RIGHT_TRIGGER);
        int   has_pair   = (b->pair_axis != JCE_BIND_PAIR_NONE);
        float raw        = jce_input_device_axis_raw(in, dev, (JceGamepadAxis)b->code);
        float pair_raw   = has_pair
                         ? jce_input_device_axis_raw(in, dev, (JceGamepadAxis)b->pair_axis)
                         : 0.0f;
        eval_axis(b, in, dev, raw, pair_raw, has_pair, is_trigger, out);
        break;
    }

    case JCE_SRC_PAD_STICK: {
        /* A 2D source: `code` is the JceStick, `channel` picks which half of
         * the shaped pair this binding reports. */
        int   ax = (b->code == JCE_STICK_RIGHT) ? JCE_GAMEPAD_AXIS_RIGHTX
                                                : JCE_GAMEPAD_AXIS_LEFTX;
        int   ay = (b->code == JCE_STICK_RIGHT) ? JCE_GAMEPAD_AXIS_RIGHTY
                                                : JCE_GAMEPAD_AXIS_LEFTY;
        float rx = jce_input_device_axis_raw(in, dev, (JceGamepadAxis)ax);
        float ry = jce_input_device_axis_raw(in, dev, (JceGamepadAxis)ay);
        float inner, outer, sx = rx, sy = ry;
        if (!(b->flags & JCE_BINDF_RAW)) {
            resolve_zone(b, in, dev, 0, &inner, &outer);
            jce_input_shape_stick(rx, ry, inner, outer, b->curve, &sx, &sy);
        }
        out->x          = sx * b->scale;
        out->y          = sy * b->scale;
        /* PRE-scale: sx/sy are already 0..1 travel along the stick's direction,
         * so this is the pushed fraction and not the scaled contribution. */
        out->activation = clampf(sqrtf(sx * sx + sy * sy), 0.0f, 1.0f);
        out->analog     = true;
        out->xy         = true;
        out->vector2d   = true;
        out->channel    = (int)b->channel;
        return;                       /* channel routing below does not apply */
    }

    case JCE_SRC_JOY_BUTTON:
        out->digital = jce_input_device_ordinal_button(in, dev, b->code);
        out->value   = out->digital ? b->scale : 0.0f;
        break;

    case JCE_SRC_JOY_AXIS: {
        int   has_pair = (b->pair_axis != JCE_BIND_PAIR_NONE);
        float raw      = jce_input_device_ordinal_axis(in, dev, b->code);
        float pair_raw = has_pair
                       ? jce_input_device_ordinal_axis(in, dev, (int)b->pair_axis)
                       : 0.0f;
        eval_axis(b, in, dev, raw, pair_raw, has_pair, 0, out);
        break;
    }

    case JCE_SRC_JOY_HAT: {
        uint8_t mask = jce_input_device_hat(in, dev, b->code);
        out->digital = (b->hat_dir != JCE_HAT_CENTERED) &&
                       ((mask & b->hat_dir) != 0u);
        out->value   = out->digital ? b->scale : 0.0f;
        break;
    }

    case JCE_SRC_COMPOSITE: {
        int   digital = 0, analog = 0;
        float px = eval_sub(b, &b->comp[JCE_COMP_POS],  in, dev, &digital, &analog);
        float nx = eval_sub(b, &b->comp[JCE_COMP_NEG],  in, dev, &digital, &analog);
        float uy = eval_sub(b, &b->comp[JCE_COMP_UP],   in, dev, &digital, &analog);
        float dy = eval_sub(b, &b->comp[JCE_COMP_DOWN], in, dev, &digital, &analog);
        /* PRE-scale, because activation is what the analog latch compares
         * against a threshold: computing it from the scaled components made the
         * same physical push clear the latch at scale 2 and miss it at scale
         * 0.5, on a field the header documents as a 0..1 magnitude. */
        float x  = px - nx;
        float y  = uy - dy;

        out->x        = x * b->scale;
        out->y        = y * b->scale;
        out->digital  = digital != 0;
        /* Without this an analog composite reported activation > 0 with
         * analog == false, and `down = digital_down || analog_latch` could
         * never be satisfied -- the binding was unpressable by construction. */
        out->analog   = analog != 0;
        out->xy       = true;
        out->vector2d = (b->code == JCE_COMPOSITE_VECTOR_2D);
        out->activation = clampf(out->vector2d ? sqrtf(x * x + y * y) : fabsf(x),
                                 0.0f, 1.0f);
        return;                       /* composites write x/y directly */
    }

    case JCE_SRC_NONE:
    case JCE_SRC_COUNT:
    default:
        return;
    }

    /* DELTA routes the contribution to `delta` and takes it OUT of `value`.
     * Mouse look is px/frame (do NOT multiply by dt); stick look is -1..1 in
     * value() (DO multiply by dt).  One place, one rule. */
    if (b->flags & JCE_BINDF_DELTA) {
        out->delta      = out->value;
        out->value      = 0.0f;
        out->activation = 0.0f;
        out->analog     = false;
        out->digital    = false;
        return;
    }

    /* Channel routing: a binding that declares X or Y writes the vector, not
     * the scalar, so the action layer folds it exactly like a composite and
     * the contribution is never counted twice. */
    out->channel = (int)b->channel;
    if (b->channel == JCE_CHAN_X)      { out->x = out->value; out->value = 0.0f; out->xy = true; }
    else if (b->channel == JCE_CHAN_Y) { out->y = out->value; out->value = 0.0f; out->xy = true; }
}
