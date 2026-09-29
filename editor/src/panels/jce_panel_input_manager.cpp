/*
 * jce_panel_input_manager.cpp  Input Manager window (Sprint 2 / 0.8.18)
 *
 * Project-wide editor surface for authoring action bindings consumed by
 * jce_input_actions (engine layer). Actions are PROJECT-scoped: stored in
 * the open project's <root>/.jce/input_actions.json so they travel with
 * the project (version control) and the shipped game loads them at start.
 * Older builds wrote a per-user ~/.jce copy (and, before the explicit-root
 * fix, a CWD-relative one); both are migrated forward on first run when
 * the project has no copy yet.
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
#include "core/jce_editor_device_strip.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_state.h"    /* jce_editor_engine_input() */
#include "ui/jce_editor_panels.h"

#include <jce/tools/jce_imgui.hpp>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <jce/os/platform/jce_input_actions.h>
#include <jce/os/platform/jce_input.h>   /* live ImGui->engine action synthesis (Top 6) */
#include <jce/os/platform/jce_keys.h>    /* JCE_KEY_COUNT */
#include <jce/os/core/jce_filesystem.h>
}

#include "core/jce_editor_config.h"   /* jce_editor_dotjce_path (legacy ~/.jce) */

/* Open-project root — owned by dialog_project.cpp (same explicit-root
 * pattern as jce_project_settings / jce_pak_key).  Declared at global
 * scope: inside an anonymous namespace the extern would acquire internal
 * linkage and never bind to the definition. */
extern char s_current_project_root[512];

/* Action bindings are PROJECT-scoped: they travel with the project and the
 * shipped game reads them, so they persist under the OPEN project's root
 * (<root>/.jce/input_actions.json), resolved through the explicit root —
 * the editor never chdirs, so a bare CWD-relative path only hit the project
 * by luck and wrote into the LAUNCH dir after a project switch.  When no
 * project is open we keep the CWD-relative location (the engine's
 * jce_select_input_actions_path() reads that first) so behaviour there is
 * unchanged. */
static const char *input_dir(void) {
    static char d[1024];
    if (s_current_project_root[0])
        std::snprintf(d, sizeof(d), "%s/.jce", s_current_project_root);
    else
        std::snprintf(d, sizeof(d), ".jce");
    return d;
}
static const char *input_path(void) {
    static char p[1024];
    if (s_current_project_root[0])
        std::snprintf(p, sizeof(p), "%s/.jce/input_actions.json",
                      s_current_project_root);
    else
        std::snprintf(p, sizeof(p), ".jce/input_actions.json");
    return p;
}
#define INPUT_PATH input_path()
#define INPUT_DIR  input_dir()

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

/* The five sources this panel can author, in combo order.
 *
 * The combo used to write its INDEX straight into b.type, which was only ever
 * correct because schema 1 numbered KEY 0 .. COMPOSITE 4.  JceBinding v2
 * renumbers (JCE_SRC_NONE took slot 0), so an index is no longer a type: with
 * the old code, picking "Key" would have stored JCE_SRC_NONE and picking
 * "Composite" would have stored JCE_SRC_PAD_BUTTON -- silently, on every
 * binding a user touched.  Index and type are separated here, permanently. */
static const JceBindType k_panel_bind_types[] = {
    JCE_SRC_KEY, JCE_SRC_MOUSE_BUTTON, JCE_SRC_PAD_BUTTON,
    JCE_SRC_PAD_AXIS, JCE_SRC_COMPOSITE
};
static const int k_panel_bind_type_count =
    (int)(sizeof(k_panel_bind_types) / sizeof(k_panel_bind_types[0]));

/* Combo index for a stored type; 0 (Key) for anything this panel cannot
 * author yet, which is what the old `t >= count -> 0` clamp did for garbage. */
static int bind_type_to_combo_index(int t)
{
    for (int i = 0; i < k_panel_bind_type_count; ++i)
        if ((int)k_panel_bind_types[i] == t) return i;
    return 0;
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
            /* The panel's flat int model can only express KEY sub-sources,
             * which is exactly what the pre-v2 bare ints always meant.  Plan B
             * Batch 7 replaces this model with the typed row editor.
             *
             * Take the code ONLY when the slot really is a key: the save half
             * below re-types every non-zero code as JCE_SRC_KEY, so carrying a
             * pad sub-source's code through here would silently convert it to
             * a keyboard bind on the next save.  Dropping the slot loses it
             * visibly (it shows as empty); keeping the code corrupts it
             * invisibly. */
            auto sub_key = [](const JceInputSource &s) {
                return s.type == JCE_SRC_KEY ? s.code : 0;
            };
            EditBinding eb{ (int)bind.type, bind.code,
                            bind.scale, bind.deadzone_inner,
                            sub_key(bind.comp[JCE_COMP_POS]),
                            sub_key(bind.comp[JCE_COMP_NEG]),
                            sub_key(bind.comp[JCE_COMP_UP]),
                            sub_key(bind.comp[JCE_COMP_DOWN]),
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
            JceBinding b;
            jce_binding_init(&b, (JceBindType)eb.type, eb.code);
            b.scale          = eb.scale;
            b.deadzone_inner = eb.deadzone;
            /* Schema 1 axes are signed, unpaired and saturate at 1.0 exactly.
             * jce_binding_init() defers a pad axis to the device profile
             * (-1); the panel's model has no field for that, so pin the
             * schema-1 shape rather than let an edit silently change it. */
            b.deadzone_outer = 1.0f;
            b.side           = (uint8_t)JCE_AXIS_SIDE_FULL;
            b.pair_axis      = JCE_BIND_PAIR_NONE;
            /* Carry composite sub-keys through so a composite binding survives
             * load/edit/save instead of being flattened to a bare scalar. */
            const int sub[4] = { eb.comp_pos, eb.comp_neg, eb.comp_up, eb.comp_down };
            for (int c = 0; c < 4; ++c) {
                b.comp[c].type = (int16_t)(sub[c] ? JCE_SRC_KEY : JCE_SRC_NONE);
                b.comp[c].side = (int16_t)JCE_AXIS_SIDE_FULL;
                b.comp[c].code = sub[c];
            }
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

/* One-time forward-migration into <root>/.jce/input_actions.json.  Two
 * legacy sources, newest store generation first:
 *   1. CWD-relative .jce/ — the pre-explicit-root path; after a project
 *      switch it pointed at the LAUNCH dir, so bindings authored then live
 *      there.  (With no project open INPUT_PATH IS the CWD copy, so the
 *      early return below makes this self-copy-safe.)
 *   2. per-user ~/.jce/ — the original store. */
static void migrate_legacy_actions(void)
{
    if (jce_fs_host_exists_file(INPUT_PATH)) return;        /* project copy wins */
    const char *src = nullptr;
    if (s_current_project_root[0] &&
        jce_fs_host_exists_file(".jce/input_actions.json"))
        src = ".jce/input_actions.json";
    if (!src) {
        const char *legacy = legacy_input_path();
        if (legacy[0] && jce_fs_host_exists_file(legacy)) src = legacy;
    }
    if (!src) return;
    JceInputActions *a = jce_actions_load_file(src);
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

/* Project root the current action table was loaded for. */
static char s_actions_root[512] = {0};

static void ensure_init(void)
{
    /* Follow the open project (the store is project-scoped): a project
     * switch reloads from the new root instead of saving the previous
     * project's table into it. */
    if (s_initialized &&
        strcmp(s_actions_root, s_current_project_root) != 0)
        s_initialized = false;
    if (s_initialized) return;
    s_initialized = true;
    std::snprintf(s_actions_root, sizeof(s_actions_root), "%s",
                  s_current_project_root);
    s_selected = -1;
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

/* ── Device strip (Plan D task 11) ───────────────────────────────────
 *
 * THE FIRST SURFACE IN THE PRODUCT WHERE A CONNECTED DEVICE IS VISIBLE.
 * Everything above this line edits the action map ON DISK; none of it asks the
 * engine what is plugged in, which is why an XInput pad could be connected and
 * the owner could still read this panel as "keyboard/mouse and touch only".
 *
 * It reads the ENGINE's JceInput -- the one jce_engine.c pumps SDL into every
 * frame -- and NOT s_live_input above, whose synthesized frame carries ImGui
 * keyboard state and no device table at all.
 *
 * NO SILKSCREEN COLUMN.  Drawing SOUTH as "A" or "Cross" needs a GLYPH FAMILY
 * (PlayStation / Xbox / Nintendo / Generic); JceGamepadStyle is MODEL
 * granularity (PS4 vs PS5, because a DualSense has adaptive triggers and a
 * DualShock 4 does not).  The two value sets are not substitutable, the
 * function that bridges them does not exist yet, and a wrong label is the
 * exact class of defect this whole strip exists to end.  Missing is honest. */

static const char *tr(const char *key) { return jce_editor_i18n(key); }

static const char *device_class_key(int cls)
{
    switch (cls) {
    case JCE_DEVCLASS_KEYBOARD: return "inputManager.deviceStrip.class.keyboard";
    case JCE_DEVCLASS_MOUSE:    return "inputManager.deviceStrip.class.mouse";
    case JCE_DEVCLASS_TOUCH:    return "inputManager.deviceStrip.class.touch";
    case JCE_DEVCLASS_GAMEPAD:  return "inputManager.deviceStrip.class.gamepad";
    case JCE_DEVCLASS_JOYSTICK: return "inputManager.deviceStrip.class.joystick";
    default:                    return "inputManager.deviceStrip.class.unknown";
    }
}

static const char *power_state_key(int state)
{
    switch (state) {
    case JCE_POWER_WIRED:      return "inputManager.deviceStrip.power.wired";
    case JCE_POWER_ON_BATTERY: return "inputManager.deviceStrip.power.onBattery";
    case JCE_POWER_CHARGING:   return "inputManager.deviceStrip.power.charging";
    case JCE_POWER_CHARGED:    return "inputManager.deviceStrip.power.charged";
    default:                   return "inputManager.deviceStrip.power.unknown";
    }
}

/* One status line under the table: the verdict of the last button pressed.
 * Held as text rather than as a key so the formatted variants (slot number)
 * survive to the next frame. */
static char s_strip_status[256];

static void strip_status(const char *text)
{
    std::snprintf(s_strip_status, sizeof(s_strip_status), "%s", text);
}

/* ── Live per-frame channels ─────────────────────────────────────────
 *
 * The three channels a game needs for TEXT ENTRY and 2D scrolling, read
 * straight off the engine input this frame.  They are here because they are
 * invisible everywhere else: composed text and IME commits never appear as
 * keycodes, key AUTO-REPEAT is not the same event as a key press, and the
 * horizontal wheel axis is a separate axis a trackpad produces and a mouse
 * usually does not.  All three were on the wire with no accessor, which is
 * how the shipped runtime came to open an IME over the game with nothing
 * listening, delete one byte per held Backspace, and ignore sideways scroll.
 * A row that shows them is how an author sees whether their device is
 * actually producing them. */
static void draw_live_channels(JceInput *in)
{
    if (!in) return;
    if (!ImGui::CollapsingHeader(tr("inputManager.liveChannels.title")))
        return;

    const char *typed = jce_input_text(in);
    ImGui::Text("%s", tr("inputManager.liveChannels.text"));
    ImGui::SameLine();
    if (typed && typed[0]) ImGui::TextUnformatted(typed);
    else                   ImGui::TextDisabled("--");

    /* Any key repeating this frame (the first one found is enough to show the
     * channel is alive). */
    int rep = -1;
    for (int k = 0; k < JCE_KEY_COUNT; ++k)
        if (jce_input_key_repeated(in, (JceKey)k)) { rep = k; break; }
    ImGui::Text("%s", tr("inputManager.liveChannels.keyRepeat"));
    ImGui::SameLine();
    if (rep >= 0) ImGui::Text("%s %d", tr("inputManager.liveChannels.scancode"), rep);
    else          ImGui::TextDisabled("--");

    ImGui::Text("%s", tr("inputManager.liveChannels.wheel"));
    ImGui::SameLine();
    ImGui::Text("v %.2f   h %.2f",
                (double)jce_input_mouse_wheel(in),
                (double)jce_input_mouse_wheel_h(in));
}

static void draw_device_strip(void)
{
    if (!ImGui::CollapsingHeader(tr("inputManager.deviceStrip.title"),
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;

    JceInput *in = jce_editor_engine_input();
    if (!in) {
        /* NOT "no devices connected".  There is no input system to ask, and
         * printing the other sentence here would be the same conflation of
         * configuration with reality that this strip exists to end. */
        ImGui::TextDisabled("%s", tr("inputManager.deviceStrip.noEngineInput"));
        return;
    }

    JceEditorDeviceRow rows[JCE_INPUT_MAX_DEVICES];
    const int total = jce_editor_device_strip_collect(in, rows,
                                                      IM_ARRAYSIZE(rows));
    const int shown = (total < IM_ARRAYSIZE(rows)) ? total : IM_ARRAYSIZE(rows);

    if (shown <= 0) {
        ImGui::TextDisabled("%s", tr("inputManager.deviceStrip.none"));
        return;
    }

    const ImGuiTableFlags tf = ImGuiTableFlags_Borders |
                               ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_SizingStretchProp |
                               ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##device_strip", 7, tf,
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() *
                                        (float)(shown + 2)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("inputManager.deviceStrip.colDevice"),
                                ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn(tr("inputManager.deviceStrip.colClass"),
                                ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(tr("inputManager.deviceStrip.colLayout"),
                                ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(tr("inputManager.player"),
                                ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn(tr("inputManager.deviceStrip.colControls"),
                                ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(tr("inputManager.deviceStrip.colFeatures"),
                                ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn(tr("inputManager.deviceStrip.battery"),
                                ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < shown; ++i) {
            const JceEditorDeviceRow &r = rows[i];
            ImGui::PushID((int)r.info.id);
            ImGui::TableNextRow();

            /* Device: id + reported name. */
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("#%u  %s", (unsigned)r.info.id,
                        r.info.name[0] ? r.info.name : "?");

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(tr(device_class_key(r.info.cls)));

            /* Layout.  A raw device has ORDINALS AND NO SEMANTIC MAP -- that is
             * the definition of the class, not a missing field -- so the cell
             * says what it has instead of leaving a semantic hole. */
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(
                r.semantic ? tr("inputManager.deviceStrip.layout.gamepad")
                           : tr("inputManager.deviceStrip.layout.raw"));
            if (!r.semantic && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("inputManager.deviceStrip.rawNote"));

            /* Player slot.  Entry 0 is "unassigned"; entries 1..N are slots
             * 0..N-1, spelled with the API's own 0-based numbers so the value
             * on screen is the value jce_input_player_* takes. */
            ImGui::TableSetColumnIndex(3);
            {
                const char *slots[1 + JCE_INPUT_MAX_PLAYERS] = {
                    tr("inputManager.deviceStrip.playerNone"),
                    "0", "1", "2", "3"
                };
                static_assert(JCE_INPUT_MAX_PLAYERS == 4,
                              "slot labels above are spelled one per player");

                int sel = (r.info.player == JCE_INPUT_PLAYER_NONE)
                              ? 0 : (int)r.info.player + 1;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##player", &sel, slots,
                                 1 + JCE_INPUT_MAX_PLAYERS)) {
                    if (sel == 0) {
                        if (r.info.player != JCE_INPUT_PLAYER_NONE)
                            jce_input_player_release_device(
                                in, (int)r.info.player, r.info.id);
                    } else if (!jce_input_player_assign_device(in, sel - 1,
                                                               r.info.id)) {
                        char msg[192];
                        std::snprintf(msg, sizeof(msg),
                                      tr("inputManager.deviceStrip.claimRefused"),
                                      sel - 1);
                        strip_status(msg);
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s",
                        tr("inputManager.deviceStrip.claimSlot"));
            }

            /* Axes / buttons / hats. */
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%u / %u / %u", (unsigned)r.info.axis_count,
                        (unsigned)r.info.button_count,
                        (unsigned)r.info.hat_count);

            /* Features: what THIS BUILD reports about THIS unit.  `caps` comes
             * from the drivers compiled into SDL, so it is never phrased as a
             * property of the plastic. */
            ImGui::TableSetColumnIndex(5);
            {
                char feats[192];
                feats[0] = '\0';
                auto add = [&](const char *key) {
                    if (feats[0]) {
                        const size_t n = std::strlen(feats);
                        std::snprintf(feats + n, sizeof(feats) - n, " · ");
                    }
                    const size_t n = std::strlen(feats);
                    std::snprintf(feats + n, sizeof(feats) - n, "%s", tr(key));
                };
                if (r.info.caps & JCE_INPUT_CAP_RUMBLE)
                    add("inputManager.deviceStrip.cap.rumble");
                if (r.info.caps & JCE_INPUT_CAP_TRIGGER_RUMBLE)
                    add("inputManager.deviceStrip.cap.triggerRumble");
                if (r.info.caps & JCE_INPUT_CAP_LED)
                    add("inputManager.deviceStrip.cap.led");
                if (!r.info.active)
                    add("inputManager.deviceStrip.inactive");
                if (!feats[0])
                    ImGui::TextDisabled("%s",
                        tr("inputManager.deviceStrip.capsNone"));
                else
                    ImGui::TextUnformatted(feats);
            }

            /* Battery.  A percentage is drawn ONLY for a state that has one --
             * jce_input_device_power() answers -1 for UNKNOWN and for WIRED,
             * and "-1%" is a number this column must never print. */
            ImGui::TableSetColumnIndex(6);
            if (!r.battery_readable) {
                ImGui::TextDisabled("%s",
                    tr("inputManager.deviceStrip.batteryUnreadable"));
            } else {
                if (r.battery_has_percent)
                    ImGui::Text("%s  %d%%", tr(power_state_key(r.power_state)),
                                r.battery_percent);
                else
                    ImGui::TextUnformatted(tr(power_state_key(r.power_state)));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s",
                        tr("inputManager.deviceStrip.batteryNotLive"));
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    /* Per-device actions, one row of buttons per device.  Kept below the table
     * rather than in an eighth column so the buttons keep their labels at any
     * panel width instead of being clipped to nothing. */
    for (int i = 0; i < shown; ++i) {
        const JceEditorDeviceRow &r = rows[i];
        ImGui::PushID((1 << 20) | (int)r.info.id);

        ImGui::TextDisabled("#%u", (unsigned)r.info.id);
        ImGui::SameLine();

        /* GATE 2 IS THE STATE AN EDITOR GREYS A BUTTON ON: the capability bit
         * being down means this build finds no motors on this device, and the
         * owner should learn that BEFORE pressing rather than after. */
        const bool can_rumble = (r.info.caps & JCE_INPUT_CAP_RUMBLE) != 0u;
        ImGui::BeginDisabled(!can_rumble);
        if (ImGui::Button(tr("inputManager.deviceStrip.testRumble"))) {
            /* Past the bit, a false is gate 3 OR the backend's own refusal, and
             * jce_input_device.h states plainly that this API does not separate
             * those two -- so the message names both instead of guessing. */
            switch (jce_editor_device_strip_rumble(in, r.info.id,
                                                   0.6f, 0.6f, 300u)) {
            case JCE_EDITOR_RUMBLE_SENT:
                strip_status(tr("inputManager.deviceStrip.rumbleSent"));
                break;
            case JCE_EDITOR_RUMBLE_NO_MOTORS:
                strip_status(tr("inputManager.deviceStrip.rumbleNoMotors"));
                break;
            case JCE_EDITOR_RUMBLE_REFUSED:
                strip_status(tr("inputManager.deviceStrip.rumbleRefused"));
                break;
            case JCE_EDITOR_RUMBLE_GONE:
                strip_status(tr("inputManager.deviceStrip.rumbleGone"));
                break;
            }
        }
        ImGui::EndDisabled();
        if (!can_rumble && ImGui::IsItemHovered(
                               ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s",
                tr("inputManager.deviceStrip.rumbleNoMotors"));

        /* A raw device is one SDL's mapping database does not know.  The useful
         * action for it is not a semantic map it does not have -- it is a stub
         * the owner can fill in. */
        if (!r.semantic) {
            char stub[256];
            const int len = jce_editor_device_strip_mapping_stub(
                &r.info, stub, (int)sizeof(stub));
            ImGui::SameLine();
            ImGui::BeginDisabled(len <= 0);
            if (ImGui::Button(tr("inputManager.deviceStrip.copyMappingStub"))) {
                ImGui::SetClipboardText(stub);
                strip_status(tr("inputManager.deviceStrip.mappingStubCopied"));
            }
            ImGui::EndDisabled();
            if (len <= 0 && ImGui::IsItemHovered(
                                ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s",
                    tr("inputManager.deviceStrip.mappingStubUnavailable"));
        }

        ImGui::PopID();
    }

    if (s_strip_status[0])
        ImGui::TextDisabled("%s", s_strip_status);

    draw_live_channels(in);
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

    /* What is actually PLUGGED IN, above the map that is merely AUTHORED.
     * Order is deliberate: the panel used to show only the second and the
     * owner read it as the first. */
    draw_device_strip();
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
            static_assert(IM_ARRAYSIZE(types) ==
                          sizeof(k_panel_bind_types) / sizeof(k_panel_bind_types[0]),
                          "combo labels and k_panel_bind_types must stay 1:1");
            int t = bind_type_to_combo_index(b.type);
            if (ImGui::Combo("##type", &t, types, IM_ARRAYSIZE(types))) {
                b.type = (int)k_panel_bind_types[t];
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
