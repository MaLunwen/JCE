/*
 * jce_panel_input_manager.cpp  Input Manager window (Sprint 2 / 0.8.18)
 *
 * Project-wide editor surface for authoring action bindings consumed by
 * jce_input_actions (engine layer). Actions are PROJECT-scoped: stored in
 * the project-relative .jce/input_actions.json so they travel with the
 * project (version control) and the shipped game loads them at start.
 * Older builds wrote a per-user ~/.jce copy; it is migrated forward on
 * first run when the project has no copy yet.
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
#include <jce/os/platform/jce_input.h>   /* live ImGui->engine action synthesis (Top 6) */
#include <jce/os/platform/jce_keys.h>    /* JCE_KEY_COUNT */
#include <jce/os/core/jce_filesystem.h>
}

#include "core/jce_editor_config.h"   /* jce_editor_dotjce_path (legacy ~/.jce) */

/* Action bindings are PROJECT-scoped: they travel with the project and the
 * shipped game reads them, so they persist to the project-relative
 * `.jce/input_actions.json` — the same convention as
 * jce_project_settings.cpp, and the location the engine's
 * jce_select_input_actions_path() prefers (CWD-relative copy first). The
 * editor runs with its CWD at the project root, so this resolves into the
 * open project's `.jce` directory. */
#define INPUT_PATH ".jce/input_actions.json"
#define INPUT_DIR  ".jce"

/* Legacy per-user location (~/.jce/input_actions.json) — read once by
 * migrate_legacy_actions() to carry older authored bindings forward. */
static const char *legacy_input_path(void) {
    static char p[1024]; static bool init = false;
    if (!init) { jce_editor_dotjce_path("input_actions.json", p, sizeof(p)); init = true; }
    return p;
}

struct EditBinding {
    int   type;       /* JceBindType enum value */
    int   code;       /* scancode / button / axis; for COMPOSITE: kind */
    float scale;
    float deadzone;
    /* Composite sub-keys (JCE_BIND_COMPOSITE only). Carried through the
     * engine<->edit round-trip so composites are not silently flattened to
     * scalar on save. Zero for non-composite binds. */
    int   comp_pos;
    int   comp_neg;
    int   comp_up;
    int   comp_down;
    /* Control-scheme device tag (JceInputDeviceGroup).  0 == JCE_DEVICE_NONE
     * keeps the binding device-agnostic (active in every scheme). */
    int   device_group;
};

struct EditAction {
    std::string              name;
    std::vector<EditBinding> binds;
};

/* Mirror of an engine SchemeEntry (name + device-group bitmask). */
struct EditScheme {
    std::string name;
    unsigned    mask;   /* OR of JCE_DEVICE_BIT(JceInputDeviceGroup) */
};

static std::vector<EditAction> s_actions;

/* Live engine action map + synthesized input for editor Play (Top 6): rebuilt
 * from s_actions each frame and updated from ImGui key state, so editor-Play
 * scripts read the SAME data-driven actions a shipped game reads.  Owned here;
 * freed at process exit. */
static JceInputActions *s_live_actions = nullptr;
static JceInput        *s_live_input   = nullptr;
static std::vector<EditScheme> s_schemes;       /* named control schemes      */
static int                     s_active_scheme = -1; /* -1 == none defined    */
static bool                    s_auto_switch   = true; /* last-used auto-switch */
static bool                     s_initialized = false;
static int                      s_selected    = -1;
static char                     s_new_name[64] = {0};

/* ── Helpers ────────────────────────────────────────────────────────── */

static const char *bind_type_label(int t)
{
    switch (t) {
    case JCE_BIND_KEY:          return jce_editor_i18n("inputManager.bindType.key");
    case JCE_BIND_MOUSE_BTN:    return jce_editor_i18n("inputManager.bindType.mouseBtn");
    case JCE_BIND_GAMEPAD_BTN:  return jce_editor_i18n("inputManager.bindType.padBtn");
    case JCE_BIND_GAMEPAD_AXIS: return jce_editor_i18n("inputManager.bindType.padAxis");
    case JCE_BIND_COMPOSITE:    return jce_editor_i18n("inputManager.bindType.composite");
    default:                    return "?";
    }
}

/* Device-group combo labels, indexed by JceInputDeviceGroup
 * (NONE / KBM / GAMEPAD / TOUCH). */
static const char **device_group_labels(void)
{
    static const char *labels[JCE_DEVICE_GROUP_COUNT];
    labels[JCE_DEVICE_NONE]    = jce_editor_i18n("inputManager.device.none");
    labels[JCE_DEVICE_KBM]     = jce_editor_i18n("inputManager.device.kbm");
    labels[JCE_DEVICE_GAMEPAD] = jce_editor_i18n("inputManager.device.gamepad");
    labels[JCE_DEVICE_TOUCH]   = jce_editor_i18n("inputManager.device.touch");
    return labels;
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
                            bind.scale, bind.deadzone,
                            bind.comp_pos, bind.comp_neg,
                            bind.comp_up,  bind.comp_down,
                            bind.device_group };
            ea.binds.push_back(eb);
        }
        s_actions.push_back(std::move(ea));
    }

    /* Control schemes (name + device-group mask) and the active selection. */
    s_schemes.clear();
    const int sn = jce_action_scheme_count(a);
    for (int s = 0; s < sn; ++s) {
        const char *sname = jce_action_scheme_name(a, s);
        EditScheme es;
        es.name = (sname && sname[0]) ? sname : "scheme";
        es.mask = jce_action_scheme_mask(a, s);
        s_schemes.push_back(std::move(es));
    }
    s_active_scheme = jce_action_scheme_active(a);
    s_auto_switch   = jce_action_scheme_auto(a);
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
            JceBinding b{};
            b.type      = (JceBindType)eb.type;
            b.code      = eb.code;
            b.scale     = eb.scale;
            b.deadzone  = eb.deadzone;
            /* Carry composite sub-keys through so a composite binding survives
             * load/edit/save instead of being flattened to a bare scalar. */
            b.comp_pos  = eb.comp_pos;
            b.comp_neg  = eb.comp_neg;
            b.comp_up   = eb.comp_up;
            b.comp_down = eb.comp_down;
            b.device_group = eb.device_group;
            jce_action_bind(a, id, &b);
        }
    }

    /* Re-register named control schemes (skipped silently when none).  The
     * FIRST scheme registered becomes active inside the engine table; honor
     * the authored active-scheme selection and the auto-switch toggle on top so
     * the saved JSON (schemes / active_scheme) round-trips faithfully. */
    for (const EditScheme &es : s_schemes) {
        if (es.name.empty()) continue;
        jce_action_scheme_register(a, es.name.c_str(), es.mask);
    }
    if (jce_action_scheme_count(a) > 0) {
        if (s_active_scheme >= 0 &&
            s_active_scheme < jce_action_scheme_count(a)) {
            /* set_active() also pins (auto OFF); restore the authored auto
             * state afterward so an auto-switch=true map persists correctly. */
            jce_action_scheme_set_active(a, s_active_scheme);
        }
        jce_action_scheme_set_auto(a, s_auto_switch);
    }
    return a;
}

static void input_save(void)
{
    JceInputActions *a = build_engine_table();
    if (!a) return;
    jce_fs_host_create_directory(INPUT_DIR);
    jce_actions_save_file(a, INPUT_PATH);
    jce_actions_destroy(a);
}

/* One-time forward-migration: older editors authored the per-user
 * ~/.jce/input_actions.json.  If the open project has no copy yet but a
 * legacy one exists, load it and write it into the project so the
 * bindings travel with the project (and the runtime finds them). */
static void migrate_legacy_actions(void)
{
    if (jce_fs_host_exists_file(INPUT_PATH)) return;        /* project copy wins */
    const char *legacy = legacy_input_path();
    if (!legacy[0] || !jce_fs_host_exists_file(legacy)) return;
    JceInputActions *a = jce_actions_load_file(legacy);
    if (!a) return;
    jce_fs_host_create_directory(INPUT_DIR);
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
    migrate_legacy_actions();
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

/* Synthesize the authored action map into a LIVE engine JceInputActions from the
 * current ImGui keyboard state (Top 6).  Lets editor-Play scripts read the SAME
 * data-driven actions (jce.is_action_down / is_action_pressed / get_axis) a
 * shipped game reads — composites / axes / scale / deadzone / schemes all
 * resolve through the engine's own jce_actions_update, byte-identical to the
 * shipped path.  Rebuilt every call so unsaved rebinds apply instantly.
 * Keyboard + key-composite binds only (mouse/gamepad resolve only in the
 * shipped game, matching today's editor input limitation). */
extern "C" const JceInputActions *jce_editor_input_actions_live(void)
{
    ensure_init();
    if (!s_live_input) s_live_input = jce_input_create();
    if (!s_live_input) return nullptr;

    if (s_live_actions) jce_actions_destroy(s_live_actions);
    s_live_actions = build_engine_table();   /* <= JCE_ACTION_MAX, cheap */
    if (!s_live_actions) return nullptr;

    jce_input_update(s_live_input);   /* roll cur->prev so pressed/released edges work */

    JceInputFrame fr;
    memset(&fr, 0, sizeof fr);
    fr.version   = JCE_INPUT_FRAME_VERSION;
    fr.key_count = JCE_KEY_COUNT;
    /* JCE_KEY_* == SDL scancode == EditBinding.code, so bits index directly. */
    auto set_key = [&](int sc) {
        if (sc <= 0 || sc >= JCE_KEY_COUNT) return;
        ImGuiKey k = (ImGuiKey)jce_editor_scancode_to_imgui_key(sc);
        if (k != ImGuiKey_None && ImGui::IsKeyDown(k))
            fr.keys_bits[sc >> 6] |= (uint64_t)1 << (sc & 63);
    };
    for (const EditAction &a : s_actions) {
        for (const EditBinding &b : a.binds) {
            if (b.type == JCE_BIND_KEY) {
                set_key(b.code);
            } else if (b.type == JCE_BIND_COMPOSITE) {
                set_key(b.comp_pos); set_key(b.comp_neg);
                set_key(b.comp_up);  set_key(b.comp_down);
            }
        }
    }
    jce_input_apply(s_live_input, &fr);
    jce_actions_update(s_live_actions, s_live_input);
    return s_live_actions;
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
            const char *types[] = { jce_editor_i18n("inputManager.bindType.key"), jce_editor_i18n("inputManager.bindType.mouseBtn"), jce_editor_i18n("inputManager.bindType.padBtn"), jce_editor_i18n("inputManager.bindType.padAxis"), jce_editor_i18n("inputManager.bindType.composite") };
            int t = b.type; if (t < 0 || t >= (int)IM_ARRAYSIZE(types)) t = 0;
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

            /* Per-binding device group (control-scheme tag).  NONE keeps the
             * binding active in every scheme; KBM/Gamepad/Touch route it to the
             * matching scheme.  Authored even with no schemes defined so it is
             * ready when the designer adds them. */
            ImGui::Indent();
            ImGui::SetNextItemWidth(160);
            int dg = b.device_group;
            if (dg < 0 || dg >= JCE_DEVICE_GROUP_COUNT) dg = JCE_DEVICE_NONE;
            if (ImGui::Combo(jce_editor_i18n("inputManager.deviceGroup"), &dg,
                             device_group_labels(), JCE_DEVICE_GROUP_COUNT)) {
                b.device_group = dg;
                input_save();
            }

            /* Composite sub-key editor (only for JCE_BIND_COMPOSITE).  The
             * binding's `code` field carries the JceCompositeKind. */
            if (b.type == JCE_BIND_COMPOSITE) {
                ImGui::SetNextItemWidth(160);
                const char *kinds[] = {
                    jce_editor_i18n("inputManager.composite.axis1d"),
                    jce_editor_i18n("inputManager.composite.vector2d") };
                int kind = b.code;
                if (kind != JCE_COMPOSITE_AXIS_1D &&
                    kind != JCE_COMPOSITE_VECTOR_2D)
                    kind = JCE_COMPOSITE_AXIS_1D;
                if (ImGui::Combo(jce_editor_i18n("inputManager.composite.kind"),
                                 &kind, kinds, IM_ARRAYSIZE(kinds))) {
                    b.code = kind;
                    /* 1D axis ignores up/down: zero them so the saved JSON is
                     * unambiguous and value2 stays (vx,0). */
                    if (kind == JCE_COMPOSITE_AXIS_1D) {
                        b.comp_up = 0; b.comp_down = 0;
                    }
                    input_save();
                }

                if (kind == JCE_COMPOSITE_VECTOR_2D) {
                    /* 2D vector: +x (right), -x (left), +y (up), -y (down). */
                    ImGui::SetNextItemWidth(70);
                    if (ImGui::InputInt(jce_editor_i18n("inputManager.composite.right"),
                                        &b.comp_pos, 0)) input_save();
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(70);
                    if (ImGui::InputInt(jce_editor_i18n("inputManager.composite.left"),
                                        &b.comp_neg, 0)) input_save();
                    ImGui::SetNextItemWidth(70);
                    if (ImGui::InputInt(jce_editor_i18n("inputManager.composite.up"),
                                        &b.comp_up, 0)) input_save();
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(70);
                    if (ImGui::InputInt(jce_editor_i18n("inputManager.composite.down"),
                                        &b.comp_down, 0)) input_save();
                } else {
                    /* 1D axis: pos / neg keys. */
                    ImGui::SetNextItemWidth(70);
                    if (ImGui::InputInt(jce_editor_i18n("inputManager.composite.pos"),
                                        &b.comp_pos, 0)) input_save();
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(70);
                    if (ImGui::InputInt(jce_editor_i18n("inputManager.composite.neg"),
                                        &b.comp_neg, 0)) input_save();
                }
            }
            ImGui::Unindent();

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

    /* ── Control schemes (named device-group sets + active picker) ─────── */
    ImGui::Separator();
    if (ImGui::CollapsingHeader(jce_editor_i18n("inputManager.schemes"))) {
        /* New-scheme row: name field + Add button.  A scheme is created with an
         * empty device mask; the designer ticks the device groups below. */
        static char s_scheme_name[JCE_SCHEME_NAME_MAX] = {0};
        ImGui::SetNextItemWidth(180);
        ImGui::InputText("##scheme_new", s_scheme_name, sizeof(s_scheme_name));
        ImGui::SameLine();
        bool can_add = (s_scheme_name[0] != 0 &&
                        (int)s_schemes.size() < JCE_SCHEME_MAX);
        ImGui::BeginDisabled(!can_add);
        if (ImGui::Button(jce_editor_i18n("inputManager.scheme.add"))) {
            /* Reject duplicate names (mirrors the engine's guard). */
            bool dup = false;
            for (const EditScheme &es : s_schemes)
                if (es.name == s_scheme_name) { dup = true; break; }
            if (!dup) {
                EditScheme es;
                es.name = s_scheme_name;
                es.mask = 0;
                s_schemes.push_back(std::move(es));
                if (s_active_scheme < 0) s_active_scheme = 0;  /* first = active */
                s_scheme_name[0] = 0;
                input_save();
            }
        }
        ImGui::EndDisabled();

        /* Auto last-used-device switching toggle. */
        if (ImGui::Checkbox(jce_editor_i18n("inputManager.scheme.autoSwitch"),
                            &s_auto_switch))
            input_save();

        ImGui::Separator();

        const char **dg_labels = device_group_labels();
        for (int si = 0; si < (int)s_schemes.size(); ++si) {
            ImGui::PushID(2000 + si);
            EditScheme &es = s_schemes[si];

            /* Active radio: pins this scheme as the active one on save. */
            bool active = (s_active_scheme == si);
            if (ImGui::RadioButton("##active", active)) {
                s_active_scheme = si;
                input_save();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                    jce_editor_i18n("inputManager.scheme.active"));
            ImGui::SameLine();

            /* Rename. */
            char nb[JCE_SCHEME_NAME_MAX];
            std::snprintf(nb, sizeof(nb), "%s", es.name.c_str());
            ImGui::SetNextItemWidth(160);
            if (ImGui::InputText("##scheme_name", nb, sizeof(nb)))
                es.name = nb;
            if (ImGui::IsItemDeactivatedAfterEdit()) input_save();

            ImGui::SameLine();
            if (ImGui::SmallButton(jce_editor_i18n("inputManager.scheme.remove"))) {
                s_schemes.erase(s_schemes.begin() + si);
                /* Keep the active index valid (or -1 when none remain). */
                if (s_schemes.empty())            s_active_scheme = -1;
                else if (s_active_scheme >= (int)s_schemes.size())
                    s_active_scheme = (int)s_schemes.size() - 1;
                input_save();
                ImGui::PopID();
                break;
            }

            /* Device-group membership checkboxes (mask bits). */
            ImGui::Indent();
            for (int g = JCE_DEVICE_KBM; g < JCE_DEVICE_GROUP_COUNT; ++g) {
                bool on = (es.mask & JCE_DEVICE_BIT(g)) != 0;
                if (ImGui::Checkbox(dg_labels[g], &on)) {
                    if (on) es.mask |=  JCE_DEVICE_BIT(g);
                    else    es.mask &= ~JCE_DEVICE_BIT(g);
                    input_save();
                }
                if (g + 1 < JCE_DEVICE_GROUP_COUNT) ImGui::SameLine();
            }
            ImGui::Unindent();

            ImGui::PopID();
        }

        if (s_schemes.empty())
            ImGui::TextDisabled("%s",
                jce_editor_i18n("inputManager.scheme.none"));
    }

    ImGui::Separator();
    ImGui::TextDisabled("%s",
        jce_editor_i18n("inputManager.runtimeNote"));
}

extern "C" void jce_editor_panel_input_manager(void)
{
    /* Reserved. */
}
