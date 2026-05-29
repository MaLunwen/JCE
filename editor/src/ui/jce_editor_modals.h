/* jce_editor_modals.h — Reusable ImGui modal-dialog helpers.
 *
 * Several panels (assets, inspector, …) historically open a centered
 * "Confirm delete?" popup with a red destructive button and a cancel.
 * The frame setup (SetNextWindowPos / Size / Viewport / BeginPopupModal
 * / right-aligned button row / red color push) was ~50 LOC each, and
 * the two existing copies had already drifted (different widths,
 * different i18n keys, one using ImGuiKey_Escape, the other not).
 *
 * Use jce_modal::confirm_delete() to drop a single one-call modal.  The
 * caller draws the prompt body inside a lambda; the helper handles all
 * window/button chrome and tells you whether the user confirmed.
 */
#pragma once

#include <jce/tools/jce_imgui.hpp>
#include <functional>

namespace jce_modal {

enum Result {
    NONE    =  0, /* modal still open or not yet shown */
    CONFIRM =  1, /* user clicked the destructive button */
    CANCEL  = -1, /* user clicked cancel, pressed Esc, or X */
};

/* Centered destructive-confirm modal.
 *
 *   p_open       Caller-owned flag.  Set to true to request open; the
 *                helper opens the popup, renders chrome, runs draw_body(),
 *                draws Confirm + Cancel buttons, and resets *p_open to
 *                false when the user finishes.  Subsequent calls with
 *                *p_open == false are cheap no-ops.
 *   popup_id     ImGui popup id, e.g. "###AssetDeleteConfirm".  Use the
 *                "###" form so the visible title can be i18n'd freely.
 *   confirm_key  i18n key for the destructive button label.
 *   cancel_key   i18n key for the cancel button label.
 *   width        Dialog width in px; <=0 uses 400.0f.
 *   draw_body    Lambda that draws the prompt text / item list inside
 *                the modal (above the button row).
 *
 * Returns the user's choice this frame: NONE, CONFIRM, or CANCEL.
 */
Result confirm_delete(bool *p_open,
                      const char *popup_id,
                      const char *confirm_key,
                      const char *cancel_key,
                      float width,
                      std::function<void()> draw_body);

} /* namespace jce_modal */
