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
 * Default seed (first run, similar to jce_actions_bind_fps_defaults):
 *     move_forward, move_back, move_left, move_right,
 *     jump, sprint, look_x, look_y
 *
 * No engine wiring: the editor doesn't own a JceInputActions today,
 * and binding it into play-mode requires the game side to opt in. The
 * panel exposes a "Save" button and surfaces the JSON path so the game
 * loader (or a future editor hook) can pick it up.
 */

#include "ui/jce_editor_colors.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "io/jce_editor_file_util.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <jce/os/platform/jce_input_actions.h>
}

#define INPUT_PATH ".jce/input_actions.json"

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

/* ── Persistence ────────────────────────────────────────────────────── */

static void input_save(void)
{
    size_t cap = 256;
    for (auto &a : s_actions) cap += a.name.size() + 64 + a.binds.size() * 80;
    char *buf = (char *)ED_MALLOC(cap);
    if (!buf) return;
    size_t off = 0;
    int    w   = std::snprintf(buf + off, cap - off, "{\n  \"actions\": [\n");
    if (w < 0) { ED_FREE(buf); return; }
    off += (size_t)w;
    for (size_t i = 0; i < s_actions.size(); ++i) {
        const EditAction &a = s_actions[i];
        w = std::snprintf(buf + off, cap - off,
            "    { \"name\": \"%s\", \"binds\": [", a.name.c_str());
        if (w < 0 || (size_t)w >= cap - off) { ED_FREE(buf); return; }
        off += (size_t)w;
        for (size_t j = 0; j < a.binds.size(); ++j) {
            const EditBinding &b = a.binds[j];
            w = std::snprintf(buf + off, cap - off,
                "%s{\"type\":%d,\"code\":%d,\"scale\":%.4f,\"deadzone\":%.4f}",
                (j == 0 ? "" : ","),
                b.type, b.code, (double)b.scale, (double)b.deadzone);
            if (w < 0 || (size_t)w >= cap - off) { ED_FREE(buf); return; }
            off += (size_t)w;
        }
        w = std::snprintf(buf + off, cap - off, "] }%s\n",
                          (i + 1 < s_actions.size()) ? "," : "");
        if (w < 0 || (size_t)w >= cap - off) { ED_FREE(buf); return; }
        off += (size_t)w;
    }
    w = std::snprintf(buf + off, cap - off, "  ]\n}\n");
    if (w < 0) { ED_FREE(buf); return; }
    off += (size_t)w;
    ed_write_file(INPUT_PATH, buf, off);
    ED_FREE(buf);
}

static void seed_default_actions(void)
{
    s_actions.clear();
    static const struct { const char *name; int key; float scale; } K[] = {
        { "move_forward", 26 /*W*/,  1.0f },  /* SDL scancodes */
        { "move_back",    22 /*S*/,  1.0f },
        { "move_left",     4 /*A*/,  1.0f },
        { "move_right",    7 /*D*/,  1.0f },
        { "jump",         44 /*Spc*/,1.0f },
        { "sprint",      225 /*LShift*/,1.0f },
        { "look_x",        0,        1.0f },  /* mapped via mouse axis */
        { "look_y",        0,        1.0f },
    };
    for (auto &k : K) {
        EditAction a;
        a.name = k.name;
        if (k.key != 0) {
            EditBinding b{ JCE_BIND_KEY, k.key, k.scale, 0.15f };
            a.binds.push_back(b);
        }
        s_actions.push_back(std::move(a));
    }
}

static bool input_load(void)
{
    size_t len = 0;
    char  *raw = (char *)ed_read_file(INPUT_PATH, &len);
    if (!raw) return false;
    if (len > (1 << 20)) { ED_FREE(raw); return false; }

    s_actions.clear();

    const char *p = raw;
    while (p && *p) {
        const char *name_key = std::strstr(p, "\"name\"");
        if (!name_key) break;
        const char *q1 = std::strchr(name_key + 6, '"');
        const char *q2 = q1 ? std::strchr(q1 + 1, '"') : nullptr;
        if (!q1 || !q2) break;

        EditAction a;
        a.name.assign(q1 + 1, q2 - q1 - 1);

        const char *binds_key  = std::strstr(q2, "\"binds\"");
        const char *next_action = std::strstr(q2 + 1, "\"name\"");
        if (binds_key && (!next_action || binds_key < next_action)) {
            const char *bp = binds_key;
            const char *end = next_action ? next_action : (raw + len);
            while (bp && bp < end) {
                const char *t = std::strstr(bp, "\"type\"");
                if (!t || t >= end) break;
                int   ty = 0, cd = 0;
                float sc = 1.0f, dz = 0.15f;
                std::sscanf(t, "\"type\":%d", &ty);
                const char *cc = std::strstr(t, "\"code\"");
                if (cc && cc < end) std::sscanf(cc, "\"code\":%d", &cd);
                const char *sk = std::strstr(t, "\"scale\"");
                if (sk && sk < end) std::sscanf(sk, "\"scale\":%f", &sc);
                const char *dk = std::strstr(t, "\"deadzone\"");
                if (dk && dk < end) std::sscanf(dk, "\"deadzone\":%f", &dz);
                EditBinding b{ ty, cd, sc, dz };
                a.binds.push_back(b);
                if (a.binds.size() >= JCE_ACTION_MAX_BINDS) break;
                const char *adv = dk ? dk : (sk ? sk : (cc ? cc : t));
                bp = adv + 1;
            }
        }
        s_actions.push_back(std::move(a));
        p = (binds_key && next_action) ? next_action : (q2 + 1);
        if (s_actions.size() >= JCE_ACTION_MAX) break;
    }

    ED_FREE(raw);
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
