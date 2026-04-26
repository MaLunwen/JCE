/*
 * jce_gamepad.h  Platform-independent gamepad button/axis codes.
 *
 * Values match SDL3 enums so the engine can cast directly.
 * Verified at compile time with _Static_assert in jce_input.c.
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
