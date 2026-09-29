/*
 * jce_input_sdl.h  SEAM A — internal interface of the one SDL-speaking input
 *                  translation unit.
 *
 * NOT a public header: it lives under engine/src/, never under <jce/...>, and
 * nothing outside engine/src/os/platform/ includes it.
 *
 * It contains NO SDL token of its own.  That is deliberate and it is checked:
 * tools/lint/check_input_seam.py requires SDL to appear in exactly one input
 * translation unit, jce_input_sdl.c, and this header is inside the scanned
 * set.  The platform event arrives as `const void *` for the same reason the
 * public jce_input_handle_event takes one.
 */

#ifndef JCE_INPUT_SDL_INTERNAL_H
#define JCE_INPUT_SDL_INTERNAL_H

#include <jce/os/platform/jce_input_event.h>

/* The backend vtable is PUBLIC (jce/os/platform/jce_input_device.h): a game
 * installs a null backend on a dedicated server and a test installs a
 * recording fake, so the type cannot live behind engine/src/.  It was
 * declared here only until JceInputDeviceInfo existed. */
#include <jce/os/platform/jce_input_device.h>

#include <stdbool.h>
#include <stdint.h>

/* THE TWO MACROS THAT USED TO STAND HERE ARE GONE, and this note is their
 * receipt rather than a hole in the file.
 *
 * They were JCE_INPUT_SDL_CLS_GAMEPAD 3u and JCE_INPUT_SDL_LAYOUT_GAMEPAD 1u,
 * described as "the exact values JCE_DEVCLASS_GAMEPAD and
 * JCE_INPUT_LAYOUT_GAMEPAD WILL carry when jce_input_device.h lands ... so
 * replacing these two macros with those enumerators later is a rename with no
 * value change".  jce_input_device.h has landed -- this header includes it
 * twelve lines above -- so the sentence was written in the future tense about
 * a past event, and it named its own fix.  The translator now spells the
 * enumerators.
 *
 * VALUE IDENTITY WAS CHECKED, not assumed, and it is pinned by a test rather
 * than by this paragraph: test_class_ordinals_match_what_the_translator_
 * already_emits (tests/os/platform/test_jce_input_devices.c) asserts
 * JCE_DEVCLASS_GAMEPAD == 3 and JCE_INPUT_LAYOUT_GAMEPAD == 1 against the
 * literals, so a renumbering of either enum is a red test and not a silently
 * mis-classed attach. */

/* SEAM A.  Translate ONE platform event into 0..max JceInputEvents; returns
 * the number written.
 *
 * PURE: a function of *platform_event and nothing else.  It calls no SDL
 * function, reads no global, opens no device and touches no hardware, which
 * is precisely what lets a test hand it a struct literal with nothing plugged
 * in.  Anything that needs to probe a device belongs on the far side of
 * open_device() below -- or in jce_input_sdl_translate_live() just past it,
 * which is the one place a probe was actually needed.
 *
 * `platform_event` is a `const SDL_Event *`.  Unknown event types, and codes
 * outside the JCE range, write nothing and return 0 — they are DROPPED, never
 * clamped into a neighbouring code. */
int jce_input_sdl_translate(const void *platform_event,
                            JceInputEvent *out, int max);

/* SEAM A's IMPURE HALF, and the reason it exists is that SDL DELIVERS EVERY
 * MAPPED PAD TWICE.
 *
 * A device SDL's mapping database recognises produces the SDL_EVENT_GAMEPAD_*
 * family AND the SDL_EVENT_JOYSTICK_* family for the same SDL_JoystickID:
 * GAMEPAD_ADDED beside JOYSTICK_ADDED, GAMEPAD_AXIS_MOTION beside
 * JOYSTICK_AXIS_MOTION, and so on.  The translator above handles both families
 * because a wheel has only the second one -- so without a gate one pad would
 * arrive as two devices AND as two differently-spelled state streams.
 *
 * THE SECOND HALF IS THE DANGEROUS ONE and it is why this gate is at the event
 * door rather than inside open_device().  A record's ordinal and semantic
 * spellings are ONE STORE (jce_input_devices.c: d->axes[n] is both ordinal n
 * and semantic n; the ingest bounds at capacity and consults no layout), so a
 * mapped pad's raw echo does not merely duplicate -- it OVERWRITES.  An Xbox
 * pad's left trigger is joystick axis 4 reporting -32768 at rest and gamepad
 * axis LEFT_TRIGGER (also 4) reporting 0 at rest; the raw echo would park the
 * semantic trigger at -1.0 forever.  A gate in open_device() would refuse the
 * second IDENTITY and let that stream through, so it would fix the visible
 * half and leave the silent one.
 *
 * WHAT IT DOES: for an SDL_EVENT_JOYSTICK_* event whose instance
 * SDL_IsGamepad() recognises, it writes nothing and returns 0.  Everything
 * else -- every other event type, and the whole joystick family on a device
 * with no mapping -- is handed to jce_input_sdl_translate() unchanged.
 *
 * WHY NOT IN THE TRANSLATOR: SDL_IsGamepad() is an SDL call against the live
 * device list, and the translator's purity is what lets a test hand it a
 * struct literal with nothing plugged in (34 cases do exactly that).  Folding
 * the probe in would have bought one call site and spent the only property
 * that makes SEAM A testable.
 *
 * jce_input.c's jce_input_handle_event() calls THIS, not the pure one. */
int jce_input_sdl_translate_live(const void *platform_event,
                                 JceInputEvent *out, int max);

/* SEAM C, first half.  Hardware handle lifetime.
 *
 * The state machine calls JceInputBackend's slots when it sees DEVICE_ADDED /
 * DEVICE_REMOVED, which is what keeps the translator pure.  A test installs
 * its own struct to observe the calls without opening anything.
 *
 * The SDL implementation.  Never NULL, and ALL SEVEN of its slots are filled:
 * open_device, close_device, and the four effectors rumble / rumble_triggers /
 * set_led / power.  Dispatch is still caps-gated -- a device whose `caps` lacks
 * the bit is refused before the slot is reached -- so the visibly degraded path
 * has not gone away; it moved one gate earlier, to where SDL itself reports the
 * capability CLEAR (which is what a macOS build without hidapi and haptic
 * does, conanfile.py:110-115).
 *
 * A build that wants NO effectors installs the null backend, which is what
 * jce_input_create() leaves in place.
 *
 * jce_input_create() installs NOTHING; jce_engine_create_windowed_input()
 * (engine/src/application/jce_engine_windowed_input.h) installs this
 * explicitly, so headless, a dedicated server, replay and this suite all run
 * on the null backend without SDL ever being asked for a handle.  That
 * function exists so the install can be asserted with no window;
 * tests/application/test_jce_engine_input_backend.c is where it is. */
const JceInputBackend *jce_input_sdl_backend(void);

/* SEAM C's BOUNDARY CONVERSION, exposed for one reason: so the rounding rule
 * can be PINNED BY A TEST instead of asserted in a comment.  The engine's
 * rumble API takes 0..1 floats and SDL's takes 0..0xFFFF Uint16s, and this is
 * the only place in the tree where one becomes the other.
 *
 *   [0.0f, 1.0f] -> [0, 65535], ROUNDED TO NEAREST (ties up).  0.0f is 0 and
 *   1.0f is 65535, both exactly.
 *
 *   Everything the boundary cannot interpret becomes 0 -- SILENCE, never full
 *   power: NaN, -INF and every negative.  Anything above 1, +INF included,
 *   clamps to 65535.  The float-to-integer cast is undefined behaviour for an
 *   out-of-range value (C99 6.3.1.4), so those guards are not decoration.
 *
 * NOT JCE_API and not a public entry point: it is internal to the SDL seam.
 * It is declared here because tests/os/platform/test_jce_input_sdl_translate.c
 * is the one target that can see this header, and that is where the rule above
 * is run rather than believed. */
uint16_t jce_input_sdl_rumble_magnitude(float v);

#endif /* JCE_INPUT_SDL_INTERNAL_H */
