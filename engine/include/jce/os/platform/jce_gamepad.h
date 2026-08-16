/*
 * jce_gamepad.h  Platform-independent gamepad button/axis codes.
 *
 * Values match SDL3 enums so the engine can cast directly.
 * Verified at compile time in jce_input_sdl.c, the one input translation
 * unit that speaks SDL and therefore the only one that performs that cast.
 */

#ifndef JCE_GAMEPAD_H
#define JCE_GAMEPAD_H


#include <jce/os/core/jce_defs.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Finger ID for touch input (matches SDL_FingerID = Uint64). */
typedef uint64_t JceFingerID;

/* ----- Gamepad buttons ----------------------------------------------- */

typedef int JceGamepadButton;

#define JCE_GAMEPAD_BUTTON_INVALID         (-1)
#define JCE_GAMEPAD_BUTTON_SOUTH             0
#define JCE_GAMEPAD_BUTTON_EAST              1
#define JCE_GAMEPAD_BUTTON_WEST              2
#define JCE_GAMEPAD_BUTTON_NORTH             3
#define JCE_GAMEPAD_BUTTON_BACK              4
#define JCE_GAMEPAD_BUTTON_GUIDE             5
#define JCE_GAMEPAD_BUTTON_START             6
#define JCE_GAMEPAD_BUTTON_LEFT_STICK        7
#define JCE_GAMEPAD_BUTTON_RIGHT_STICK       8
#define JCE_GAMEPAD_BUTTON_LEFT_SHOULDER     9
#define JCE_GAMEPAD_BUTTON_RIGHT_SHOULDER   10
#define JCE_GAMEPAD_BUTTON_DPAD_UP          11
#define JCE_GAMEPAD_BUTTON_DPAD_DOWN        12
#define JCE_GAMEPAD_BUTTON_DPAD_LEFT        13
#define JCE_GAMEPAD_BUTTON_DPAD_RIGHT       14
#define JCE_GAMEPAD_BUTTON_MISC1            15
#define JCE_GAMEPAD_BUTTON_RIGHT_PADDLE1    16
#define JCE_GAMEPAD_BUTTON_LEFT_PADDLE1     17
#define JCE_GAMEPAD_BUTTON_RIGHT_PADDLE2    18
#define JCE_GAMEPAD_BUTTON_LEFT_PADDLE2     19
#define JCE_GAMEPAD_BUTTON_TOUCHPAD         20
/* The tail of SDL's button space.  These codes have always been inside
 * JCE_GAMEPAD_BUTTON_COUNT -- it has been 26 since the enum was mirrored --
 * but they had no name, so a binding to one could only be authored as a bare
 * integer and nothing in the tree said which integers those were.
 *
 * SDL documents them as "Additional button", vendor-specific extras beyond the
 * share/capture button (MISC1, code 15) and the four Elite/Edge/Joy-Con
 * paddles (codes 16..19); MISC3 and MISC4 are the GameCube left and right
 * trigger clicks.  There is nothing common to print on them, which is why they
 * carry no per-model glyph.
 *
 * MISC2 and MISC6 are JCE_SASSERT'd against SDL in jce_input_sdl.c; the middle
 * three are forced by test_jce_gamepad_tail_buttons proving the 26 named codes
 * fill [0, COUNT) exactly once each. */
#define JCE_GAMEPAD_BUTTON_MISC2            21
#define JCE_GAMEPAD_BUTTON_MISC3            22
#define JCE_GAMEPAD_BUTTON_MISC4            23
#define JCE_GAMEPAD_BUTTON_MISC5            24
#define JCE_GAMEPAD_BUTTON_MISC6            25
#define JCE_GAMEPAD_BUTTON_COUNT            26

/* ----- Gamepad axes -------------------------------------------------- */

typedef int JceGamepadAxis;

#define JCE_GAMEPAD_AXIS_INVALID           (-1)
#define JCE_GAMEPAD_AXIS_LEFTX               0
#define JCE_GAMEPAD_AXIS_LEFTY               1
#define JCE_GAMEPAD_AXIS_RIGHTX              2
#define JCE_GAMEPAD_AXIS_RIGHTY              3
#define JCE_GAMEPAD_AXIS_LEFT_TRIGGER        4
#define JCE_GAMEPAD_AXIS_RIGHT_TRIGGER       5
#define JCE_GAMEPAD_AXIS_COUNT               6

JCE_EXTERN_C_END

#endif /* JCE_GAMEPAD_H */
