/* JCE Game View focus/capture policy (no SDL/ImGui dependencies).
 * Ownership is requested on Play or a clicked image. Alt temporarily unlocks;
 * Escape, Stop, focus loss, or a hidden viewport retire it. Re-lock on Alt-up
 * only if the Game View retained focus. */
#ifndef JCE_GAME_VIEW_FOCUS_POLICY_H
#define JCE_GAME_VIEW_FOCUS_POLICY_H

struct JceGameViewFocusInput {
    bool wanted;
    bool play_started;
    bool play_stopped;
    bool viewport_ready;
    bool app_focused;
    bool view_focused;
    bool was_captured;
    bool image_clicked;
    bool clicked_elsewhere;
    bool alt_held;
    bool escape_pressed;
    bool allow_capture;
    bool alt_just_released; /* a single ownership-preserving edge, NOT a toggle */
};

struct JceGameViewFocusOutput {
    bool wanted;
    bool keyboard_owned;
    bool request_capture;
};

inline JceGameViewFocusOutput jce_game_view_focus_decide(
    const JceGameViewFocusInput &in)
{
    bool wanted = in.wanted;
    if (in.play_stopped) wanted = false;
    if (in.play_started) wanted = true;
    if (!in.viewport_ready || !in.app_focused) wanted = false;
    /* Releasing relative mode on Alt-down may make ImGui report that
     * the Game View is not focused. Keep ownership during the HELD
     * interval and for the single Alt-up reacquire frame. A real
     * focus loss and click on another panel still revoke below. */
    if (wanted && !in.play_started && !in.view_focused && !in.was_captured &&
        !in.alt_held && !in.alt_just_released)
        wanted = false;
    if (in.image_clicked && !in.alt_held && !in.play_stopped &&
        in.viewport_ready && in.app_focused)
        wanted = true;
    /* Clicking another editor panel while the cursor is free revokes
     * input ownership even before docking focus resolves next frame. */
    if (in.clicked_elsewhere && !in.was_captured) wanted = false;
    if (in.escape_pressed) wanted = false;
    const bool keyboard = wanted && !in.alt_held && in.app_focused &&
        in.viewport_ready && (in.view_focused || in.was_captured ||
                              in.play_started || in.image_clicked ||
                              in.alt_just_released);
    JceGameViewFocusOutput out = {wanted, keyboard,
                                     keyboard && in.allow_capture};
    return out;
}

#endif
