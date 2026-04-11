/*
 * jce_keys.h  Platform-independent keyboard key codes.
 *
 * Values match SDL3 scancodes (USB HID codes) so the engine
 * can cast directly — no runtime translation needed.
 * Verified at compile time with _Static_assert in jce_input.c.
 *
 * Game/application code includes this header instead of SDL.
 */

#ifndef JCE_KEYS_H
#define JCE_KEYS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int JceKey;

/* Key count (matches SDL_SCANCODE_COUNT). */
#define JCE_KEY_COUNT       512

/* Letters */
#define JCE_KEY_A           4
#define JCE_KEY_B           5
#define JCE_KEY_C           6
#define JCE_KEY_D           7
#define JCE_KEY_E           8
#define JCE_KEY_F           9
#define JCE_KEY_G           10
#define JCE_KEY_H           11
#define JCE_KEY_I           12
#define JCE_KEY_J           13
#define JCE_KEY_K           14
#define JCE_KEY_L           15
#define JCE_KEY_M           16
#define JCE_KEY_N           17
#define JCE_KEY_O           18
#define JCE_KEY_P           19
#define JCE_KEY_Q           20
#define JCE_KEY_R           21
#define JCE_KEY_S           22
#define JCE_KEY_T           23
#define JCE_KEY_U           24
#define JCE_KEY_V           25
#define JCE_KEY_W           26
#define JCE_KEY_X           27
#define JCE_KEY_Y           28
#define JCE_KEY_Z           29

/* Top-row digits */
#define JCE_KEY_1           30
#define JCE_KEY_2           31
#define JCE_KEY_3           32
#define JCE_KEY_4           33
#define JCE_KEY_5           34
#define JCE_KEY_6           35
#define JCE_KEY_7           36
#define JCE_KEY_8           37
#define JCE_KEY_9           38
#define JCE_KEY_0           39

/* Common keys */
#define JCE_KEY_RETURN      40
#define JCE_KEY_ESCAPE      41
#define JCE_KEY_BACKSPACE   42
#define JCE_KEY_TAB         43
#define JCE_KEY_SPACE       44

/* Punctuation / symbols */
#define JCE_KEY_MINUS       45
#define JCE_KEY_EQUALS      46
#define JCE_KEY_LEFTBRACKET  47
#define JCE_KEY_RIGHTBRACKET 48
#define JCE_KEY_BACKSLASH   49
#define JCE_KEY_SEMICOLON   51
#define JCE_KEY_APOSTROPHE  52
#define JCE_KEY_GRAVE       53
#define JCE_KEY_COMMA       54
#define JCE_KEY_PERIOD      55
#define JCE_KEY_SLASH       56

#define JCE_KEY_CAPSLOCK    57

/* Function keys */
#define JCE_KEY_F1          58
#define JCE_KEY_F2          59
#define JCE_KEY_F3          60
#define JCE_KEY_F4          61
#define JCE_KEY_F5          62
#define JCE_KEY_F6          63
#define JCE_KEY_F7          64
#define JCE_KEY_F8          65
#define JCE_KEY_F9          66
#define JCE_KEY_F10         67
#define JCE_KEY_F11         68
#define JCE_KEY_F12         69

#define JCE_KEY_PRINTSCREEN 70
#define JCE_KEY_SCROLLLOCK  71
#define JCE_KEY_PAUSE       72
#define JCE_KEY_INSERT      73
#define JCE_KEY_HOME        74
#define JCE_KEY_PAGEUP      75
#define JCE_KEY_DELETE      76
#define JCE_KEY_END         77
#define JCE_KEY_PAGEDOWN    78

/* Arrow keys */
#define JCE_KEY_RIGHT       79
#define JCE_KEY_LEFT        80
#define JCE_KEY_DOWN        81
#define JCE_KEY_UP          82

#define JCE_KEY_NUMLOCKCLEAR 83

/* Keypad */
#define JCE_KEY_KP_DIVIDE   84
#define JCE_KEY_KP_MULTIPLY 85
#define JCE_KEY_KP_MINUS    86
#define JCE_KEY_KP_PLUS     87
#define JCE_KEY_KP_ENTER    88
#define JCE_KEY_KP_1        89
#define JCE_KEY_KP_2        90
#define JCE_KEY_KP_3        91
#define JCE_KEY_KP_4        92
#define JCE_KEY_KP_5        93
#define JCE_KEY_KP_6        94
#define JCE_KEY_KP_7        95
#define JCE_KEY_KP_8        96
#define JCE_KEY_KP_9        97
#define JCE_KEY_KP_0        98
#define JCE_KEY_KP_PERIOD   99
#define JCE_KEY_KP_EQUALS   103

/* Modifier keys */
#define JCE_KEY_LCTRL       224
#define JCE_KEY_LSHIFT      225
#define JCE_KEY_LALT        226
#define JCE_KEY_LGUI        227
#define JCE_KEY_RCTRL       228
#define JCE_KEY_RSHIFT      229
#define JCE_KEY_RALT        230
#define JCE_KEY_RGUI        231

/* Mobile / special */
#define JCE_KEY_AC_BACK     282

#ifdef __cplusplus
}
#endif

#endif /* JCE_KEYS_H */
