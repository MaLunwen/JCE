/* AI panel session policy; provider/process work remains in the LLM owner. */
#ifndef JCE_EDITOR_AI_PANEL_SESSION_H
#define JCE_EDITOR_AI_PANEL_SESSION_H

#include <string>

struct JceEditorAiPanelSession {
    bool visible = false;
    bool send_for_real = false;
};

inline void jce_editor_ai_panel_session_observe(
    JceEditorAiPanelSession &session, bool visible)
{
    if (!visible || !session.visible)
        session.send_for_real = false;
    session.visible = visible;
}

inline std::string jce_editor_ai_panel_session_args(
    const JceEditorAiPanelSession &session, const std::string &base)
{
    return session.send_for_real ? base + " --send" : base;
}

#endif
