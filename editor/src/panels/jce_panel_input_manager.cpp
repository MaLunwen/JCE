/*
 * jce_panel_input_manager.cpp  Input Manager window (Sprint 2 / 0.8.18)
 *
 * Project-wide editor surface for authoring action bindings consumed by
 * jce_input_actions (engine layer). Actions are stored editor-side in
 * .jce/input_actions.json so they survive across sessions and can be
 * loaded into a JceInputActions instance at game start.
 *
 * Each action has a unique name and 0..JCE_ACTION_MAX_BINDS bindings.
 * A binding is (type, code, scale, deadzone) — the same struct the
 * engine consumes via jce_action_bind().
 *
 * Default seed (first run) is copied from the engine's canonical table,
 * jce_actions_bind_fps_defaults() — there is no editor-local copy.
 * Persistence likewise goes through the engine (jce_actions_load_file /
 * jce_actions_save_file), so the panel can never drift from the schema
 * the runtime reads.
 *
 * Consumers: the engine boot-loads this JSON into JceServices.actions
 * (jce_actions_load_file) for shipped games, and the editor's Play view
 * queries the LIVE panel state through jce_editor_input_action_keys()
 * below — so rebinds apply to play-in-editor without even saving.
 */

#include "ui/jce_editor_colors.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include <jce/os/platform/jce_input_actions.h>
}

#include "core/jce_editor_config.h"   /* jce_editor_dotjce_path (~/.jce) */

/* Per-user config dir (~/.jce); see jce_editor_dotjce_path. */
static const char *input_path(void) {
    static char p[1024]; static bool init = false;
    if (!init) { jce_editor_dotjce_path("input_actions.json", p, sizeof(p)); init = true; }
    return p;
}
#define INPUT_PATH input_path()

struct EditBinding {
    int   type;       /* JceBindType enum value */
    int   code;
    float scale;
    float deadzone;
};

struct EditAction {
    std::string              name;
    std::vector<EditBinding> binds;
};

static std::vector<EditAction> s_actions;
static bool                     s_initialized = false;
static int                      s_selected    = -1;
static char                     s_new_name[64] = {0};

/* ── Helpers ────────────────────────────────────────────────────────── */

static const char *bind_type_label(int t)
{
    switch (t) {
    case JCE_BIND_KEY:          return "Key";
    case JCE_BIND_MOUSE_BTN:    return "MouseBtn";
    case JCE_BIND_GAMEPAD_BTN:  return "PadBtn";
    case JCE_BIND_GAMEPAD_AXIS: return "PadAxis";
    default:                    return "?";
    }
}

/* ── Persistence (backed by the engine's jce_input_actions API) ────── */

/* Copy an engine action table into the panel's editing model. */
static void copy_from_engine(const JceInputActions *a)
{
    s_actions.clear();
    const int n = jce_actions_count(a);
    for (int i = 0; i < n; ++i) {
        const char *name = jce_action_name(a, i);
        if (!name || !name[0]) continue;

        EditAction ea;
        ea.name = name;
        const int bn = jce_action_bind_count(a, i);
        for (int b = 0; b < bn; ++b) {
            JceBinding bind;
            if (!jce_action_bind_at(a, i, b, &bind)) continue;
            EditBinding eb{ (int)bind.type, bind.code,
                            bind.scale, bind.deadzone };
            ea.binds.push_back(eb);
        }
        s_actions.push_back(std::move(ea));
    }
}

/* Build an engine action table from the panel's editing model.
 * Caller owns the returned table (jce_actions_destroy). */
static JceInputActions *build_engine_table(void)
{
    JceInputActions *a = jce_actions_create();
    if (!a) return nullptr;
    for (const EditAction &ea : s_actions) {
        int id = jce_action_register(a, ea.name.c_str());
        if (id < 0) continue;   /* duplicate name / table full */
        for (const EditBinding &eb : ea.binds) {
            JceBinding b;
            b.type     = (JceBindType)eb.type;
            b.code     = eb.code;
            b.scale    = eb.scale;
            b.deadzone = eb.deadzone;
            jce_action_bind(a, id, &b);
        }
    }
    return a;
}

static void input_save(void)
{
    JceInputActions *a = build_engine_table();
    if (!a) return;
    jce_actions_save_file(a, INPUT_PATH);
    jce_actions_destroy(a);
}

/* Seed from the engine's canonical default table so the editor and the
 * runtime fallback (jce_actions_bind_fps_defaults) can never diverge. */
static void seed_default_actions(void)
{
    JceInputActions *a = jce_actions_create();
    if (!a) { s_actions.clear(); return; }
    jce_actions_bind_fps_defaults(a);
    copy_from_engine(a);
    jce_actions_destroy(a);
}

static bool input_load(void)
{
    JceInputActions *a = jce_actions_load_file(INPUT_PATH);
    if (!a) return false;
    copy_from_engine(a);
    jce_actions_destroy(a);
    return true;
}

static void ensure_init(void)
{
    if (s_initialized) return;
    s_initialized = true;
    if (!input_load() || s_actions.empty()) {
        seed_default_actions();
        input_save();
    }
}

/* ── Editor-Play action queries ─────────────────────────────────────── */

/* The editor's Play loop reads input through ImGui, so authored KEY
 * bindings (SDL scancodes) are translated via the shared
 * jce_editor_scancode_to_imgui_key() table in jce_editor.cpp;
 * mouse/gamepad bindings apply only in the shipped game, where the
 * engine runtime evaluates them via JceInput. */
extern "C" int jce_editor_input_action_keys(const char *name,
                                            int *out_imgui_keys, int max)
{
    ensure_init();   /* the panel may never have been opened this session */
    if (!name || !out_imgui_keys || max <= 0) return 0;
    for (const EditAction &a : s_actions) {
        if (a.name != name) continue;
        int n = 0;
        for (const EditBinding &b : a.binds) {
            if (b.type != JCE_BIND_KEY) continue;
            ImGuiKey k = (ImGuiKey)jce_editor_scancode_to_imgui_key(b.code);
            if (k != ImGuiKey_None && n < max)
                out_imgui_keys[n++] = (int)k;
        }
        return n;
    }
    return 0;
}

/* ── UI ─────────────────────────────────────────────────────────────── */

extern "C" void jce_editor_panel_input_manager_content(void)
{
    ensure_init();

    /* Toolbar */
    if (ImGui::Button(jce_editor_i18n("inputManager.addAction"))) {
        if (s_actions.size() < JCE_ACTION_MAX) {
            EditAction a;
            char tmp[32];
            std::snprintf(tmp, sizeof(tmp), "action_%u",
                          (unsigned)s_actions.size());
            a.name = tmp;
            s_actions.push_back(std::move(a));
            s_selected = (int)s_actions.size() - 1;
            input_save();
        }
    }
    ImGui::SameLine();
    bool can_remove = (s_selected >= 0 &&
                       s_selected < (int)s_actions.size());
    ImGui::BeginDisabled(!can_remove);
    if (ImGui::Button(jce_editor_i18n("inputManager.removeAction"))) {
        s_actions.erase(s_actions.begin() + s_selected);
        if (s_selected >= (int)s_actions.size()) --s_selected;
        input_save();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("inputManager.resetDefaults"))) {
        seed_default_actions();
        s_selected = 0;
        input_save();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("inputManager.save"))) {
        input_save();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("inputManager.reload"))) {
        s_initialized = false;
        ensure_init();
    }

    ImGui::TextDisabled("%s %s",
        jce_editor_i18n("inputManager.path"), INPUT_PATH);
    ImGui::Separator();

    /* Two-column: left list, right details */
    ImGui::BeginChild("##actions_left", ImVec2(220, 0), true);
    for (int i = 0; i < (int)s_actions.size(); ++i) {
        ImGui::PushID(i);
        bool sel = (s_selected == i);
        if (ImGui::Selectable(s_actions[i].name.c_str(), sel)) {
            s_selected = i;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##actions_right", ImVec2(0, 0), true);
    if (can_remove) {
        EditAction &a = s_actions[s_selected];

        char namebuf[64];
        std::snprintf(namebuf, sizeof(namebuf), "%s", a.name.c_str());
        if (ImGui::InputText(jce_editor_i18n("inputManager.name"),
                             namebuf, sizeof(namebuf))) {
            a.name = namebuf;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) input_save();

        ImGui::Separator();
        ImGui::Text("%s (%d / %d)",
                    jce_editor_i18n("inputManager.bindings"),
                    (int)a.binds.size(), (int)JCE_ACTION_MAX_BINDS);

        for (int bi = 0; bi < (int)a.binds.size(); ++bi) {
            ImGui::PushID(1000 + bi);
            EditBinding &b = a.binds[bi];

            ImGui::SetNextItemWidth(110);
            const char *types[] = { "Key", "MouseBtn", "PadBtn", "PadAxis" };
            int t = b.type; if (t < 0 || t > 3) t = 0;
            if (ImGui::Combo("##type", &t, types, IM_ARRAYSIZE(types))) {
                b.type = t;
                input_save();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            if (ImGui::InputInt("##code", &b.code, 0))
                input_save();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            if (ImGui::DragFloat("##scale", &b.scale, 0.05f, -4.0f, 4.0f, "s%.2f"))
                {}
            if (ImGui::IsItemDeactivatedAfterEdit()) input_save();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            if (ImGui::DragFloat("##dz", &b.deadzone, 0.01f, 0.0f, 1.0f, "dz%.2f"))
                {}
            if (ImGui::IsItemDeactivatedAfterEdit()) input_save();
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) {
                a.binds.erase(a.binds.begin() + bi);
                input_save();
                ImGui::PopID();
                break;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", bind_type_label(b.type));

            ImGui::PopID();
        }

        if (a.binds.size() < JCE_ACTION_MAX_BINDS) {
            if (ImGui::Button(jce_editor_i18n("inputManager.addBinding"))) {
                EditBinding b{ JCE_BIND_KEY, 0, 1.0f, 0.15f };
                a.binds.push_back(b);
                input_save();
            }
        }
    } else {
        ImGui::TextDisabled("%s",
            jce_editor_i18n("inputManager.selectAction"));
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::TextDisabled("%s",
        jce_editor_i18n("inputManager.runtimeNote"));
}

extern "C" void jce_editor_panel_input_manager(void)
{
    /* Reserved. */
}
