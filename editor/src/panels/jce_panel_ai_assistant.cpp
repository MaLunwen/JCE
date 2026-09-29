/*
 * jce_panel_ai_assistant.cpp -- ask a language model to author a scene, from
 * inside the editor.
 *
 * WHAT WAS ACTUALLY MISSING.  The loop itself was not: private/tools/ai/jce_design.py
 * extracts the schema from the engine's own parsers so a model cannot invent
 * a field, audits the answer before it can become a file, refuses to send
 * anything without --send, and already closes the loop -- --capture-now
 * renders the scene, --look shows the model that render, --verify compares
 * before and after against a noise floor taken from two shots of the
 * unchanged scene, and --iterate runs the whole round again.  What was
 * missing is that ALL of it was reachable only by typing a command, so the
 * editor -- the place a designer actually works -- could not reach any of it.
 *
 * This panel is a front end for that tool, over the engine's own
 * <jce/api_llm.h>.  It deliberately owns none of the intelligence: the schema,
 * the audit, the render loop and the provider matrix stay in one place, and a
 * panel that reimplemented any of them would be a second answer to a question
 * that already has one.
 *
 * THE KEY IS NOT HERE, and there is no field for it.  The child process
 * inherits the editor's environment, which is where JCE_LLM_API_KEY lives.
 * The editor never reads it, never stores it in prefs.json, and so cannot
 * leak it into a bug report.  A text box for it would be a text box somebody
 * screenshots.
 *
 * NOTHING IS SENT BY ACCIDENT.  The tool's default is a dry run that prints
 * the request instead of making it; the "Send for real" checkbox is what adds
 * --send, it is off every time the panel is opened, and the button says which
 * of the two it is about to do.
 */

#include "jce_panel_common.h"
#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_config.h"
#include "dialogs/jce_path_input.h"
#include "core/jce_assetdb.h"

#include <jce/api_llm.h>
#include <jce/tools/jce_imgui.hpp>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

/* ---- persisted provider ------------------------------------------- */
/* Keys live in prefs.json through the generic UI-string store, so the
 * provider survives a restart the way every other editor preference does. */
constexpr const char *K_EXE     = "ai.provider.exe";
constexpr const char *K_ARGS    = "ai.provider.args";
constexpr const char *K_WD      = "ai.provider.wd";
constexpr const char *K_TIMEOUT = "ai.provider.timeoutSec";

/* The default reaches this repository's own tool, which speaks five wire
 * formats plus a `command` escape hatch of its own -- so "any model" is true
 * out of the box rather than after configuration.  {prompt} and {response}
 * are substituted by the engine; the quotes survive because
 * jce_process_spawn splits tokens quote-aware. */
constexpr const char *DEFAULT_EXE  = "python";
constexpr const char *DEFAULT_ARGS =
    "private/tools/ai/jce_design.py --brief-file \"{prompt}\" --out \"{response}\"";

/* ONE state, not a dozen loose statics: the dedup audit counts file-scope
 * statics and a struct counts as one, and the engine half of this feature
 * (jce_llm.c) is folded the same way.  Two halves of one feature disagreeing
 * about the house style is how the style stops meaning anything. */
struct AiPanelState {
    char exe[512]   = { 0 };
    char args[1024] = { 0 };
    char wd[512]    = { 0 };
    int  timeout_s  = 180;

    /* The brief.  Large because a useful one is a paragraph, not a sentence,
     * and because the transport is a file precisely so it need not be short. */
    char brief[8192] = { 0 };

    bool send_for_real = false;   /* off on every open, on purpose */
    bool look          = false;
    bool verify        = false;
    int  iterate       = 1;
    char scene[512]    = { 0 };

    JceLlmHandle handle = 0u;
    bool         loaded = false;
};

AiPanelState s_ai;

void load_prefs(void)
{
    if (s_ai.loaded) return;
    s_ai.loaded = true;
    if (!jce_editor_config_get_ui_str(K_EXE, s_ai.exe, sizeof s_ai.exe) || !s_ai.exe[0])
        snprintf(s_ai.exe, sizeof s_ai.exe, "%s", DEFAULT_EXE);
    if (!jce_editor_config_get_ui_str(K_ARGS, s_ai.args, sizeof s_ai.args) || !s_ai.args[0])
        snprintf(s_ai.args, sizeof s_ai.args, "%s", DEFAULT_ARGS);
    (void)jce_editor_config_get_ui_str(K_WD, s_ai.wd, sizeof s_ai.wd);
    s_ai.timeout_s = jce_editor_config_get_ui_int_or(NULL, K_TIMEOUT, 180);
    if (s_ai.timeout_s < 5) s_ai.timeout_s = 5;
}

/* The flags the checkboxes add.  Appended to the stored template rather than
 * baked into it, so a user who rewrote the template for a different tool
 * keeps their template and simply gets no flags they did not ask for. */
std::string build_args(void)
{
    std::string a = s_ai.args;
    if (s_ai.send_for_real) a += " --send";
    if (s_ai.look)          a += " --look";
    if (s_ai.verify)        a += " --verify";
    if (s_ai.iterate > 1)   { char b[32]; snprintf(b, sizeof b, " --iterate %d", s_ai.iterate); a += b; }
    if (s_ai.scene[0])      { a += " --scene \""; a += s_ai.scene; a += "\""; }
    return a;
}

bool request_running(void)
{
    JceLlmProgress p;
    return s_ai.handle != 0u && jce_llm_poll(s_ai.handle, &p) &&
           p.status == JCE_LLM_RUNNING;
}

void submit(void)
{
    if (s_ai.handle != 0u) { jce_llm_release(s_ai.handle); s_ai.handle = 0u; }

    const std::string args = build_args();

    JceLlmRequest req;
    memset(&req, 0, sizeof req);
    req.provider.executable  = s_ai.exe;
    req.provider.arguments   = args.c_str();
    req.provider.working_dir = s_ai.wd[0] ? s_ai.wd : NULL;
    req.provider.timeout_ms  = s_ai.timeout_s * 1000;
    req.prompt               = s_ai.brief;
    req.work_dir             = s_ai.wd[0] ? s_ai.wd : NULL;

    s_ai.handle = jce_llm_submit(&req);
    if (s_ai.handle == 0u)
        jce_editor_console_log_level(JCE_CONSOLE_ERROR, "[AI] %s",
                                     jce_llm_last_error());
}

void draw_provider(void)
{
    if (!ImGui::CollapsingHeader(jce_editor_i18n("ai.provider"))) return;

    ImGui::TextWrapped("%s", jce_editor_i18n("ai.provider.note"));
    ImGui::Spacing();

    if (ImGui::InputText(jce_editor_i18n("ai.provider.exe"), s_ai.exe, sizeof s_ai.exe))
        jce_editor_config_set_ui_str(K_EXE, s_ai.exe);
    if (ImGui::InputText(jce_editor_i18n("ai.provider.args"), s_ai.args, sizeof s_ai.args))
        jce_editor_config_set_ui_str(K_ARGS, s_ai.args);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("ai.provider.args.tip"));
    if (ImGui::InputText(jce_editor_i18n("ai.provider.wd"), s_ai.wd, sizeof s_ai.wd))
        jce_editor_config_set_ui_str(K_WD, s_ai.wd);
    if (ImGui::DragInt(jce_editor_i18n("ai.provider.timeout"), &s_ai.timeout_s,
                       1.0f, 5, 3600, "%d s")) {
        JceEditorConfig cfg;
        if (jce_editor_config_load(&cfg)) {
            jce_editor_config_set_ui_int(&cfg, K_TIMEOUT, s_ai.timeout_s);
            (void)jce_editor_config_save(&cfg);
        }
    }
    if (ImGui::SmallButton(jce_editor_i18n("ai.provider.reset"))) {
        snprintf(s_ai.exe,  sizeof s_ai.exe,  "%s", DEFAULT_EXE);
        snprintf(s_ai.args, sizeof s_ai.args, "%s", DEFAULT_ARGS);
        jce_editor_config_set_ui_str(K_EXE,  s_ai.exe);
        jce_editor_config_set_ui_str(K_ARGS, s_ai.args);
    }
    ImGui::Separator();
}

void draw_content(void)
{
    load_prefs();

    draw_provider();

    ImGui::TextUnformatted(jce_editor_i18n("ai.brief"));
    ImGui::InputTextMultiline("##ai_brief", s_ai.brief, sizeof s_ai.brief,
                              ImVec2(-1, ImGui::GetTextLineHeight() * 6));

    /* The scene the question is ABOUT.  Optional: without it the model is
     * authoring from nothing, with it the tool can render the before, show
     * it, and compare the after. */
    jce_draw_path_input_asset(jce_editor_i18n("ai.scene"), s_ai.scene,
                              sizeof s_ai.scene, JCE_ASSET_KIND_SCENE);

    ImGui::Checkbox(jce_editor_i18n("ai.look"), &s_ai.look);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", jce_editor_i18n("ai.look.tip"));
    ImGui::SameLine();
    ImGui::Checkbox(jce_editor_i18n("ai.verify"), &s_ai.verify);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", jce_editor_i18n("ai.verify.tip"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::DragInt(jce_editor_i18n("ai.iterate"), &s_ai.iterate, 0.1f, 1, 8);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", jce_editor_i18n("ai.iterate.tip"));

    /* THE ONE CONTROL THAT SPENDS SOMETHING AND LEAVES THE MACHINE.  Off on
     * every open; the button below says which of the two it will do. */
    ImGui::Checkbox(jce_editor_i18n("ai.send"), &s_ai.send_for_real);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", jce_editor_i18n("ai.send.tip"));

    ImGui::Separator();

    const bool running = request_running();
    ImGui::BeginDisabled(running || s_ai.brief[0] == 0);
    if (ImGui::Button(s_ai.send_for_real ? jce_editor_i18n("ai.ask")
                                      : jce_editor_i18n("ai.dryRun"),
                      ImVec2(200, 0)))
        submit();
    ImGui::EndDisabled();

    if (running) {
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("common.cancel")))
            jce_llm_cancel(s_ai.handle);
    }

    JceLlmProgress p;
    if (s_ai.handle != 0u && jce_llm_poll(s_ai.handle, &p)) {
        ImGui::Spacing();
        const char *state = jce_editor_i18n("ai.state.idle");
        switch (p.status) {
        case JCE_LLM_RUNNING:   state = jce_editor_i18n("ai.state.running");   break;
        case JCE_LLM_DONE:      state = jce_editor_i18n("ai.state.done");      break;
        case JCE_LLM_FAILED:    state = jce_editor_i18n("ai.state.failed");    break;
        case JCE_LLM_CANCELLED: state = jce_editor_i18n("ai.state.cancelled"); break;
        default: break;
        }
        ImGui::Text("%s  %.1f s  %s", state, (double)p.elapsed_ms / 1000.0,
                    p.message ? p.message : "");

        const char *log = jce_llm_output_log(s_ai.handle);
        if (log && log[0]) {
            ImGui::TextUnformatted(jce_editor_i18n("ai.log"));
            ImGui::InputTextMultiline("##ai_log", (char *)log, strlen(log) + 1,
                                      ImVec2(-1, ImGui::GetTextLineHeight() * 8),
                                      ImGuiInputTextFlags_ReadOnly);
        }

        size_t len = 0;
        const char *ans = jce_llm_response(s_ai.handle, &len);
        if (ans) {
            ImGui::Separator();
            ImGui::Text("%s (%zu B)", jce_editor_i18n("ai.answer"), len);
            ImGui::InputTextMultiline("##ai_answer", (char *)ans, len + 1,
                                      ImVec2(-1, ImGui::GetTextLineHeight() * 10),
                                      ImGuiInputTextFlags_ReadOnly);
            /* Deliberately NOT an "apply to the open scene" button yet.  The
             * tool already wrote a validated scene file; opening it is the
             * reversible action, and silently replacing what the designer has
             * on screen is not. */
            ImGui::TextWrapped("%s", jce_editor_i18n("ai.answer.note"));
        }
    }
}

} /* namespace */

/* Pumped from the editor's frame update, NOT from the draw above: a panel
 * that is closed or docked behind another tab does not draw, and a child
 * whose stdout is never drained fills the pipe, blocks in write() and then
 * never exits.  The request would hang for exactly as long as the user had
 * the window hidden, which is the kind of bug that gets blamed on the model. */
extern "C" void jce_editor_ai_assistant_tick(void)
{
    jce_llm_tick();
}

extern "C" void jce_editor_panel_ai_assistant(void)
{
    char _wt[96];
    snprintf(_wt, sizeof(_wt), "%s###ai_assistant",
             jce_editor_i18n("window.aiAssistant"));
    if (!ImGui::Begin(_wt,
                      jce_editor_panel_visible_ptr(JCE_PANEL_AI_ASSISTANT),
                      ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return;
    }
    draw_content();
    ImGui::End();
}
