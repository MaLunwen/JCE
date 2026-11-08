#ifndef JCE_EDITOR_KEY_EVENT_H
#define JCE_EDITOR_KEY_EVENT_H
#include <jce/tools/jce_imgui.hpp>
#include <jce/os/platform/jce_window_event.h>

/* The existing event bridge's submission owner, shared with actual-context
 * controls. The editor's existing scancode table supplies the mapped key. */
inline void jce_editor_key_modifiers(ImGuiIO &io, uint16_t mods)
{
    io.AddKeyEvent(ImGuiMod_Ctrl,  (mods & JCE_KMOD_CTRL)  != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (mods & JCE_KMOD_SHIFT) != 0);
    io.AddKeyEvent(ImGuiMod_Alt,   (mods & JCE_KMOD_ALT)   != 0);
    io.AddKeyEvent(ImGuiMod_Super, (mods & JCE_KMOD_GUI)   != 0);
}
inline void jce_editor_submit_key_event(const JceEvent &event, ImGuiKey mapped)
{
    ImGuiIO &io = ImGui::GetIO();
    jce_editor_key_modifiers(io, event.key.mod);
    if (mapped != ImGuiKey_None)
        io.AddKeyEvent(mapped, event.type == JCE_EVENT_KEY_DOWN);
}
#endif
