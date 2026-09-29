/*
 * jce_rt_input.c  The runtime's per-frame input sample.
 *
 * Its own translation unit for the reason jce_rt_physics.c / jce_rt_script.c /
 * jce_rt_audio.c / jce_rt_streaming.c are: jce_runtime.c is a 5000-line file
 * the size gate freezes, and this arrived as an addition the gate correctly
 * refused to let that file absorb.
 */

#include "jce_rt_internal.h"

#include <string.h>

/* Expire everything in JceRuntimeInput that is a SAMPLE rather than a latched
 * state, immediately after a step consumed it.
 *
 * Pointer deltas, the wheel, buttons and touches are cleared because a
 * hidden or unfocused host view must not leave a button stuck down; movement
 * input deliberately keeps its latched semantics and is not touched here.
 *
 * The keyboard borrow expires for exactly the same reason, and it matters
 * more: a host that stops supplying input -- an unfocused game view -- must
 * stop answering jce.is_key_down too, or a Play script keeps reading the keys
 * the user is typing into some other panel.  Every host that has a keyboard
 * re-borrows it every frame; a host with none supplies NULL and the binding
 * answers false, which is what it did before the field existed.
 *
 * Guarded by tests/application/test_jce_runtime_is_key_down.c, whose third
 * case supplies a keyboard once and steps twice.
 */
void rt_input_expire_frame_sample(JceRuntime *rt)
{
	if (!rt) return;
	rt->input.pointer_dx      = 0.0f;
	rt->input.pointer_dy      = 0.0f;
	rt->input.pointer_wheel   = 0.0f;
	rt->input.pointer_buttons = 0u;
	rt->input.touch_count     = 0;
	rt->input.keyboard        = NULL;
	memset(rt->input.touches, 0, sizeof(rt->input.touches));
}
