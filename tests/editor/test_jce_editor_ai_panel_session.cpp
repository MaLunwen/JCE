#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_ai_panel_session.h"

TEST_CASE("first panel open defaults to dry run")
{
    JceEditorAiPanelSession session;
    CHECK_FALSE(session.send_for_real);
    session.send_for_real = true;
    jce_editor_ai_panel_session_observe(session, true);
    CHECK_FALSE(session.send_for_real);
    CHECK(jce_editor_ai_panel_session_args(session, "local-provider") == "local-provider");
}

TEST_CASE("explicit send survives repeated observations in one visible session")
{
    JceEditorAiPanelSession session;
    jce_editor_ai_panel_session_observe(session, true);
    session.send_for_real = true;
    for (int frame = 0; frame < 8; ++frame)
        jce_editor_ai_panel_session_observe(session, true);
    CHECK(session.send_for_real);
    CHECK(jce_editor_ai_panel_session_args(session, "local-provider") == "local-provider --send");
}

TEST_CASE("closing invalidates send before another draw")
{
    JceEditorAiPanelSession session;
    jce_editor_ai_panel_session_observe(session, true);
    session.send_for_real = true;
    jce_editor_ai_panel_session_observe(session, false);
    CHECK_FALSE(session.visible);
    CHECK_FALSE(session.send_for_real);
    CHECK(jce_editor_ai_panel_session_args(session, "local-provider") == "local-provider");
}

TEST_CASE("reopening a closed panel requires a fresh send opt in")
{
    JceEditorAiPanelSession session;
    for (int cycle = 0; cycle < 3; ++cycle) {
        jce_editor_ai_panel_session_observe(session, true);
        CHECK_FALSE(session.send_for_real);
        session.send_for_real = true;
        CHECK(jce_editor_ai_panel_session_args(session, "local-provider") == "local-provider --send");
        jce_editor_ai_panel_session_observe(session, false);
        jce_editor_ai_panel_session_observe(session, true);
        CHECK(jce_editor_ai_panel_session_args(session, "local-provider") == "local-provider");
    }
}

TEST_CASE("visible inactive or collapsed panels keep the explicit session choice")
{
    JceEditorAiPanelSession session;
    jce_editor_ai_panel_session_observe(session, true);
    session.send_for_real = true;
    // Policy consumes visibility, never ImGui Begin's draw/collapse result.
    jce_editor_ai_panel_session_observe(session, true);
    CHECK(session.send_for_real);
}

TEST_CASE("session flags do not rewrite the configured provider arguments")
{
    const std::string base = "local-provider --look --verify --scene scene.jcescene";
    JceEditorAiPanelSession session;
    jce_editor_ai_panel_session_observe(session, true);
    CHECK(jce_editor_ai_panel_session_args(session, base) == base);
    session.send_for_real = true;
    CHECK(jce_editor_ai_panel_session_args(session, base) == base + " --send");
    jce_editor_ai_panel_session_observe(session, false);
    CHECK(jce_editor_ai_panel_session_args(session, base) == base);
}
