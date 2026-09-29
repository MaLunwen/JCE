/* test_jce_gamepad_tail_buttons.c
 *
 * jce_gamepad.h named buttons 0..20 and then declared COUNT == 26, so five
 * real buttons on modern pads had no name at all: a binding to one could only
 * be authored as a bare integer, and nothing in the tree said which integers
 * those were.  This target pins that the NAMED set and the SEMANTIC set are
 * now the same set -- every code in [0, COUNT) is named by exactly one
 * JCE_GAMEPAD_BUTTON_* macro, no macro shares a code with another, and none
 * falls outside the range.
 *
 * The numbering itself is checked where SDL is legal to name: jce_input_sdl.c
 * carries JCE_SASSERT for MISC2 and MISC6 alongside the pre-existing one for
 * COUNT.  Those endpoints plus the no-gap / no-overlap proof below force
 * MISC3..MISC5 -- five distinct codes filling exactly [21, 25] leave the
 * middle three no freedom.  That is why this file needs no SDL token.
 *
 * SCOPE NOTE, so the remaining half is explicit rather than forgotten.  Plan C
 * Task 1 also specifies stable ASCII ids ("misc2".."misc6") and hardware
 * labels for these codes.  Those live in jce_input_names.c, which Plan B
 * Task 17 creates and which does NOT exist in this tree; see
 * .superpowers/sdd/2026-08-05-input-C-features-and-channels/task-1-report.md.
 * The id and label assertions belong in this file and are deliberately absent
 * rather than written against a stub, which would only show a writer and a
 * reader agreeing with each other.
 */

#include <jce/os/platform/jce_gamepad.h>

#include "unity.h"

#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

/* Every JCE_GAMEPAD_BUTTON_* macro that names a real button, spelled out by
 * hand.  Hand-written on purpose: a loop over 0..COUNT-1 would be satisfied by
 * the integers alone and could never tell a named code from an unnamed one --
 * which is the exact defect this file exists to keep from coming back. */
static const struct { int code; const char *macro; } kNamedButtons[] = {
    { JCE_GAMEPAD_BUTTON_SOUTH,          "SOUTH"          },
    { JCE_GAMEPAD_BUTTON_EAST,           "EAST"           },
    { JCE_GAMEPAD_BUTTON_WEST,           "WEST"           },
    { JCE_GAMEPAD_BUTTON_NORTH,          "NORTH"          },
    { JCE_GAMEPAD_BUTTON_BACK,           "BACK"           },
    { JCE_GAMEPAD_BUTTON_GUIDE,          "GUIDE"          },
    { JCE_GAMEPAD_BUTTON_START,          "START"          },
    { JCE_GAMEPAD_BUTTON_LEFT_STICK,     "LEFT_STICK"     },
    { JCE_GAMEPAD_BUTTON_RIGHT_STICK,    "RIGHT_STICK"    },
    { JCE_GAMEPAD_BUTTON_LEFT_SHOULDER,  "LEFT_SHOULDER"  },
    { JCE_GAMEPAD_BUTTON_RIGHT_SHOULDER, "RIGHT_SHOULDER" },
    { JCE_GAMEPAD_BUTTON_DPAD_UP,        "DPAD_UP"        },
    { JCE_GAMEPAD_BUTTON_DPAD_DOWN,      "DPAD_DOWN"      },
    { JCE_GAMEPAD_BUTTON_DPAD_LEFT,      "DPAD_LEFT"      },
    { JCE_GAMEPAD_BUTTON_DPAD_RIGHT,     "DPAD_RIGHT"     },
    { JCE_GAMEPAD_BUTTON_MISC1,          "MISC1"          },
    { JCE_GAMEPAD_BUTTON_RIGHT_PADDLE1,  "RIGHT_PADDLE1"  },
    { JCE_GAMEPAD_BUTTON_LEFT_PADDLE1,   "LEFT_PADDLE1"   },
    { JCE_GAMEPAD_BUTTON_RIGHT_PADDLE2,  "RIGHT_PADDLE2"  },
    { JCE_GAMEPAD_BUTTON_LEFT_PADDLE2,   "LEFT_PADDLE2"   },
    { JCE_GAMEPAD_BUTTON_TOUCHPAD,       "TOUCHPAD"       },
    { JCE_GAMEPAD_BUTTON_MISC2,          "MISC2"          },
    { JCE_GAMEPAD_BUTTON_MISC3,          "MISC3"          },
    { JCE_GAMEPAD_BUTTON_MISC4,          "MISC4"          },
    { JCE_GAMEPAD_BUTTON_MISC5,          "MISC5"          },
    { JCE_GAMEPAD_BUTTON_MISC6,          "MISC6"          }
};

#define NAMED_COUNT ((int)(sizeof kNamedButtons / sizeof kNamedButtons[0]))

/* The whole point: naming is total over the semantic space.  A code with no
 * macro is a button that can only be authored as a bare integer; a code with
 * two macros is two names for one button, which is worse. */
static void test_every_semantic_code_is_named_exactly_once(void)
{
    int seen[JCE_GAMEPAD_BUTTON_COUNT];
    char msg[96];
    int i;

    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_GAMEPAD_BUTTON_COUNT, NAMED_COUNT,
        "JCE_GAMEPAD_BUTTON_COUNT and the number of named buttons disagree");

    for (i = 0; i < JCE_GAMEPAD_BUTTON_COUNT; ++i) seen[i] = 0;

    for (i = 0; i < NAMED_COUNT; ++i) {
        sprintf(msg, "JCE_GAMEPAD_BUTTON_%s is outside [0, COUNT)",
                kNamedButtons[i].macro);
        TEST_ASSERT_TRUE_MESSAGE(kNamedButtons[i].code >= 0 &&
                                 kNamedButtons[i].code < JCE_GAMEPAD_BUTTON_COUNT,
                                 msg);
        seen[kNamedButtons[i].code]++;
    }

    for (i = 0; i < JCE_GAMEPAD_BUTTON_COUNT; ++i) {
        sprintf(msg, "button code %d is named %d times, must be exactly 1",
                i, seen[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, seen[i], msg);
    }
}

/* The five codes this task added, at the values SDL3 assigns them. */
static void test_tail_button_codes_are_21_through_25(void)
{
    TEST_ASSERT_EQUAL_INT(21, JCE_GAMEPAD_BUTTON_MISC2);
    TEST_ASSERT_EQUAL_INT(22, JCE_GAMEPAD_BUTTON_MISC3);
    TEST_ASSERT_EQUAL_INT(23, JCE_GAMEPAD_BUTTON_MISC4);
    TEST_ASSERT_EQUAL_INT(24, JCE_GAMEPAD_BUTTON_MISC5);
    TEST_ASSERT_EQUAL_INT(25, JCE_GAMEPAD_BUTTON_MISC6);
}

/* They occupy exactly the stretch that used to be nameless: strictly above the
 * last previously-named code, and running right up to COUNT. */
static void test_the_tail_closes_the_gap_below_count(void)
{
    TEST_ASSERT_TRUE(JCE_GAMEPAD_BUTTON_MISC2 > JCE_GAMEPAD_BUTTON_TOUCHPAD);
    TEST_ASSERT_EQUAL_INT(JCE_GAMEPAD_BUTTON_TOUCHPAD + 1,
                          JCE_GAMEPAD_BUTTON_MISC2);
    TEST_ASSERT_EQUAL_INT(JCE_GAMEPAD_BUTTON_COUNT - 1,
                          JCE_GAMEPAD_BUTTON_MISC6);
}

/* INVALID is not a member of the space it is the absence of. */
static void test_invalid_is_outside_the_semantic_space(void)
{
    TEST_ASSERT_EQUAL_INT(-1, JCE_GAMEPAD_BUTTON_INVALID);
    TEST_ASSERT_TRUE(JCE_GAMEPAD_BUTTON_INVALID < 0);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_semantic_code_is_named_exactly_once);
    RUN_TEST(test_tail_button_codes_are_21_through_25);
    RUN_TEST(test_the_tail_closes_the_gap_below_count);
    RUN_TEST(test_invalid_is_outside_the_semantic_space);
    return UNITY_END();
}
