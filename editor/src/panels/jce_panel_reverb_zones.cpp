/*
 * jce_panel_reverb_zones.cpp  Shim (P6-A.1).
 *
 * The Reverb Zones authoring UI has been merged into the Audio Mixer
 * panel as a "Reverb" tab (see jce_panel_audio_mixer.cpp). This file
 * preserves the public `_content` symbol so existing menu / hotkey /
 * layout callers still link — invoking it focuses the Audio Mixer panel
 * with the Reverb tab pre-selected and hides the legacy window.
 */

#include "ui/jce_editor_panels.h"

#include <jce/tools/jce_imgui.hpp>

extern "C" void jce_editor_audio_mixer_focus_reverb_tab(void);

extern "C" void jce_editor_panel_reverb_zones_content(void)
{
    bool *self = jce_editor_panel_visible_ptr(JCE_PANEL_REVERB_ZONES);
    bool *host = jce_editor_panel_visible_ptr(JCE_PANEL_AUDIO_MIXER);
    if (host) *host = true;
    if (self) *self = false;
    jce_editor_audio_mixer_focus_reverb_tab();
    ImGui::SetWindowFocus("###audio_mixer");
}
