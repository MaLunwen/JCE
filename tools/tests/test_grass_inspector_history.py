"""Run the actual Grass drawer and common undo helpers with counted CPU ports.

This verifies field-to-history consumption, gesture boundaries and stored values.
The history port is a counted snapshot store, not actual scene serialization or
physical GUI input. Missing wiring negatives must compile and fail at runtime.
"""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("grass_source_helper",
    ROOT / "tools/tests/test_canvas_transparent_draw.py")
HELPERS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HELPERS)


def probe(control, mutation=None):
    common = (ROOT / "editor/src/panels/jce_panel_inspector_common.cpp").read_text(encoding="utf-8")
    header = (ROOT / "editor/src/panels/jce_panel_inspector_common.h").read_text(encoding="utf-8")
    gameplay = (ROOT / "editor/src/panels/jce_panel_inspector_gameplay.cpp").read_text(encoding="utf-8")
    public = (ROOT / "engine/include/jce/middleware/scene/jce_scene.h").read_text(encoding="utf-8")
    path_source = (ROOT / "editor/src/dialogs/jce_path_input.cpp").read_text(encoding="utf-8")
    path_header = (ROOT / "editor/src/dialogs/jce_path_input.h").read_text(encoding="utf-8")
    path_owner = HELPERS.production_function(path_source, "bool jce_draw_path_input(")
    path_layout = HELPERS.production_function(path_header, "enum class JcePathKind") + ";\n"
    path_layout += HELPERS.production_function(path_header, "struct JcePathInputOpts") + ";\n"
    path_layout += HELPERS.production_function(path_source, "struct Slot") + ";\n"
    if mutation == "async_before":
        old = 'snprintf(changed_before, sizeof(changed_before), "%s", slot.last_seen);'
        assert path_owner.count(old) == 1
        path_owner = path_owner.replace(old,
            'snprintf(changed_before, sizeof(changed_before), "%s", buf);', 1)
    end = public.index("} JceGrassFieldComponent;") + len("} JceGrassFieldComponent;")
    layout = public[public.rfind("typedef struct {", 0, end):end]
    drawer = HELPERS.production_function(gameplay, "void draw_comp_grass_field(")
    if mutation == "visible":
        old = "insp_undo_bool(&g->visible);"
        assert drawer.count(old) == 1
        drawer = drawer.replace(old, "(void)g->visible;", 1)
    elif mutation == "continuous":
        # Removing the first immediate tracker recreates the missing gesture
        # boundary without copying the historical drawer algorithm.
        drawer = drawer.replace("    insp_track_edit();", "", 1)
    state = "\n".join(line for line in common.splitlines() if line.startswith(
        ("bool              s_insp_batch_open", "static uint32_t s_insp_edit_scope")))
    helpers = "\n".join(HELPERS.production_function(common, signature) for signature in (
        "void insp_set_edit_scope(", "void insp_track_edit(", "void insp_undo_bool("))
    helpers += "\n" + HELPERS.production_function(header, "template <typename T, typename U>")
    helpers += "\n" + HELPERS.production_function(header, "struct InspUndoScope") + ";\n"
    helpers += next(line for line in header.splitlines() if line.startswith("#define INSP_UNDO_SCOPE"))
    return (PORTS + layout + path_layout + HISTORY_PORTS + PATH_PORTS + path_owner + "\n" + state + "\n"
        + helpers + "\n" + drawer + "\n" + CONTROL + control)


def execute(test, code, negative=False):
    spec = importlib.util.spec_from_file_location("grass_build_driver", ROOT / "tools/build/jce.py")
    driver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(driver)
    if driver.HOST == "windows":
        env = driver.msvc_env(driver.resolve_target(None))
        compiler = ["cl", "/nologo", "/TP", "/std:c++20", "/EHsc", "/W3"]
    else:
        cxx = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
        if not cxx:
            test.skipTest("No C++ compiler for exact Grass drawer controls")
        env, compiler = None, [cxx, "-std=c++20"]
    base = ROOT / "build/tests/grass-inspector-history-tests"
    base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=base) as temporary:
        directory = Path(temporary)
        file = directory / "probe.cpp"
        file.write_text(code, encoding="utf-8", newline="\n")
        executable = directory / ("probe.exe" if driver.HOST == "windows" else "probe")
        command = compiler + [str(file)] + (["/Fe:probe.exe", "/Fo:probe.obj"]
            if driver.HOST == "windows" else ["-o", str(executable)])
        result = subprocess.run(command, cwd=directory, env=env, capture_output=True, text=True)
        test.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True)
        if negative:
            test.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            test.assertEqual(result.returncode, 0, result.stdout + result.stderr)


PORTS = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define REQUIRE(value) do { if (!(value)) { fprintf(stderr, "Grass history failed line %d: %s\n", __LINE__, #value); exit(3); } } while (0)
static const char *target = "", *last_item = "";
static int action;
static bool matches(const char *label) { last_item = label; return strcmp(label, target) == 0; }
static bool change_frame(void) { return action == 2 || action == 3; }
struct ImVec2 { float x,y; ImVec2(float a=0,float b=0):x(a),y(b){} };
struct ImVec4 { float x=0,y=0,z=0,w=1; };
struct ImGuiStyle { ImVec2 ItemInnerSpacing; ImVec4 Colors[3];float FrameRounding=0; };
struct ImGuiIO { float MouseDoubleClickTime=0.3f; };
struct ImDrawList { template<class... T> void AddRect(T...) {} };
struct ImGuiPayload { const void *Data; };
using ImGuiID=unsigned;
static const int ImGuiCol_Text=0,ImGuiCol_TextDisabled=1,ImGuiCol_DragDropTarget=2,ImGuiMouseButton_Left=0;
static const char *JCE_DND_ASSET_PATH="asset-path";
static const char *path_mode="typing";
namespace ImGui {
static bool IsItemActivated(void) { return action == 1 && strcmp(last_item, target) == 0; }
static bool IsItemDeactivated(void) { return action == 4 && strcmp(last_item, target) == 0; }
static bool change_float(const char *label, float *value, unsigned n) {
    if (!matches(label) || !change_frame()) return false;
    for (unsigned i=0; i<n; ++i) value[i] += 0.25f;
    return true;
}
template<class... T> static bool DragFloat(const char *label, float *value, T...) { return change_float(label,value,1); }
template<class... T> static bool DragFloat2(const char *label, float *value, T...) { return change_float(label,value,2); }
template<class... T> static bool SliderFloat(const char *label, float *value, T...) { return change_float(label,value,1); }
static bool ColorEdit3(const char *label, float *value) { return change_float(label,value,3); }
static bool SliderInt(const char *label, int *value, int, int) {
    if (!matches(label) || !change_frame()) return false; ++*value; return true;
}
static bool DragScalar(const char *label, int, void *value, float, const void *, const void *) {
    if (!matches(label) || !change_frame()) return false; ++*(uint32_t *)value; return true;
}
static bool Checkbox(const char *label, bool *value) {
    if (!matches(label) || action != 2) return false; *value = !*value; return true;
}
static bool Button(const char *label, ImVec2 = {}) {
    if (action==2 && strcmp(target,"##gf_density_mask")==0 &&
        ((strcmp(path_mode,"browse")==0 && strncmp(label,"...##",5)==0) ||
         (strcmp(path_mode,"clear")==0 && strncmp(label,"x##",3)==0))) { last_item=label;return true; }
    return matches(label) && action == 2;
}
static void SameLine(float=0,float=0) {}
static ImGuiID GetID(const char *) { return 1; }
static const ImGuiStyle &GetStyle(void) { static ImGuiStyle style;return style; }
static const ImGuiIO &GetIO(void) { static ImGuiIO io;return io; }
static float GetFrameHeight(void) { return 20; }
static float CalcItemWidth(void) { return 300; }
static bool IsItemHovered(void) { return false; }
static bool IsItemActive(void) { return false; }
static bool IsMouseDoubleClicked(int) { return false; }
static double GetTime(void) { return 1; }
static void ClearActiveID(void) {}
static void PushStyleColor(int,ImVec4) {}
static void PopStyleColor(void) {}
static void PushItemWidth(float) {}
static void PopItemWidth(void) {}
static ImVec4 GetStyleColorVec4(int) { return {}; }
static ImVec2 GetItemRectMin(void) { return {}; }
static ImVec2 GetItemRectMax(void) { return {}; }
static unsigned GetColorU32(ImVec4) { return 0; }
static ImDrawList *GetWindowDrawList(void) { static ImDrawList list;return &list; }
template<class... T> static void SetTooltip(const char *, T...) {}
static bool InputTextWithHint(const char *label,const char *,char *buf,size_t size) {
    if(!matches(label) || action!=2 || strcmp(path_mode,"typing"))return false;
    snprintf(buf,size,"textures/new-mask.png");return true;
}
static bool BeginDragDropTarget(void) { return action==2 && !strcmp(last_item,target) && !strcmp(path_mode,"drop"); }
static const ImGuiPayload *AcceptDragDropPayload(const char *) { static ImGuiPayload payload={"textures/drop-mask.png"};return &payload; }
static void EndDragDropTarget(void) {}

static void Separator(void) { last_item = "separator"; }
static void TextUnformatted(const char *) { last_item = "text"; }
template<class... T> static void TextDisabled(const char *, T...) { last_item = "estimate-or-hint"; }
}
static const int ImGuiDataType_U32=1, JCE_ASSET_KIND_TEXTURE=2;
static const char *jce_editor_i18n(const char *key) { return key; }
static const char *jce_editor_i18n_id(const char *key, const char *) { return key; }
static const char *jce_editor_i18n_or(const char *key, const char *) { return key; }

'''
HISTORY_PORTS = r'''
static JceGrassFieldComponent grass, pending;
struct Entry { JceGrassFieldComponent before, after; };
static std::vector<Entry> undo_stack, redo_stack;
static unsigned begins, ends, nesting, scoped;
static bool dirty;
static void jce_state_begin_batch_edit(void) {
    if (!nesting) { pending=grass; ++begins; } ++nesting;
}
static void jce_state_begin_entity_edit(uint32_t entity) { REQUIRE(entity==19); ++scoped; jce_state_begin_batch_edit(); }
static void jce_state_end_batch_edit(void) {
    REQUIRE(nesting>0); if (--nesting) return; ++ends;
    if (memcmp(&pending,&grass,sizeof grass)) { undo_stack.push_back({pending,grass}); redo_stack.clear(); dirty=true; }
}
static void undo(void) { REQUIRE(!undo_stack.empty() && !nesting); Entry e=undo_stack.back();undo_stack.pop_back();grass=e.before;redo_stack.push_back(e); }
static void redo(void) { REQUIRE(!redo_stack.empty() && !nesting); Entry e=redo_stack.back();redo_stack.pop_back();grass=e.after;undo_stack.push_back(e); }
'''
PATH_PORTS = r'''
static Slot path_slot;
static char *browse_destination;
static size_t browse_capacity;
static bool *browse_ready,*browse_cancel;
static Slot &slot_for(const char *) { return path_slot; }
static const char *kBrowseGlyph="...",*kClearGlyph="x";
static const char *tooltip_for(JcePathKind) { return ""; }
static const char *placeholder_for(JcePathKind,int) { return ""; }
static void open_asset_preview(const char *) {}
static void jce_editor_asset_picker_open(const char *,int,char *destination,size_t capacity,bool *ready,bool *cancel) {
    REQUIRE(destination==grass.density_mask_path);browse_destination=destination;browse_capacity=capacity;
    browse_ready=ready;browse_cancel=cancel;
}
template<class... T> static void open_file_dialog_async(T...) { REQUIRE(false); }
template<class... T> static void save_file_dialog_async(T...) { REQUIRE(false); }
template<class... T> static void pick_folder_dialog_async(T...) { REQUIRE(false); }
static void jce_editor_path_to_relative(char *out,size_t size,const char *path) { snprintf(out,size,"%s",path); }
'''
CONTROL = r'''
static void reset(void) {
    memset(&grass,0,sizeof grass);grass.visible=grass.cast_shadow=true;
    grass.density=1;grass.area_x=grass.area_z=2;grass.cards=3;
    strcpy(grass.density_mask_path,"textures/old-mask.png");
    pending=grass;undo_stack.clear();redo_stack.clear();begins=ends=nesting=scoped=0;dirty=false;
    s_insp_batch_open=false;action=0;target="";insp_set_edit_scope(19);
    path_slot=Slot{};path_mode="typing";browse_destination=nullptr;browse_ready=browse_cancel=nullptr;
}
static void frame(int phase) { action=phase;draw_comp_grass_field(&grass); }
static void verify_roundtrip(JceGrassFieldComponent before) {
    REQUIRE(undo_stack.size()==1 && redo_stack.empty() && dirty && !nesting && !s_insp_batch_open);
    REQUIRE(begins==1 && ends==1);JceGrassFieldComponent after=grass;
    REQUIRE(memcmp(&before,&after,sizeof grass)!=0);undo();REQUIRE(memcmp(&before,&grass,sizeof grass)==0);
    REQUIRE(undo_stack.empty() && redo_stack.size()==1);redo();REQUIRE(memcmp(&after,&grass,sizeof grass)==0);
}
static void discrete(const char *key) { reset();target=key;JceGrassFieldComponent before=grass;frame(2);verify_roundtrip(before); }
static void continuous(const char *key) {
    reset();target=key;JceGrassFieldComponent before=grass;
    frame(1);REQUIRE(begins==1 && ends==0 && scoped==1 && !dirty && undo_stack.empty());
    frame(2);frame(3);REQUIRE(begins==1 && ends==0 && !dirty && undo_stack.empty());
    frame(4);verify_roundtrip(before);
    frame(0);REQUIRE(begins==1 && ends==1 && undo_stack.size()==1);
    reset();target=key;frame(1);frame(4);REQUIRE(begins==1 && ends==1 && !dirty && undo_stack.empty());
}
'''
DISCRETE_CONTROL = r'''
int main(void) {
    draw_comp_grass_field(nullptr);
    discrete("inspector.grassField.visible");
    discrete("inspector.grassField.castShadow");
    discrete("inspector.grassField.reseed");
    return 0;
}
'''
CONTINUOUS_CONTROL = r'''
int main(void) {
    const char *keys[]={"density","seed","areaX","areaZ","maxSlope","scaleMin","scaleMax",
        "bladeHeight","bladeWidth","cards","rootColor","tipColor","hueJitter","windDir",
        "windSpeed","windAmplitude","fadeStart","fadeEnd","densityThreshold","maskWorldSize"};
    char key[128];for(const char *suffix:keys) { snprintf(key,sizeof key,"inspector.grassField.%s",suffix);continuous(key); }
    return 0;
}
'''


PATH_CONTROL = r'''
int main(void) {
    for(const char *mode:{"typing","drop","clear"}) {
        reset();target="##gf_density_mask";path_mode=mode;frame(0);
        JceGrassFieldComponent before=grass;frame(2);verify_roundtrip(before);
    }
    reset();target="##gf_density_mask";path_mode="browse";frame(0);
    JceGrassFieldComponent before=grass;frame(2);
    REQUIRE(browse_destination==grass.density_mask_path && browse_capacity==sizeof grass.density_mask_path);
    REQUIRE(!dirty && begins==0 && undo_stack.empty() && browse_ready && browse_cancel);
    snprintf(browse_destination,browse_capacity,"textures/async-mask.png");*browse_ready=true;
    frame(0);verify_roundtrip(before);
    REQUIRE(!strcmp(grass.density_mask_path,"textures/async-mask.png") && !path_slot.browse_ready);
    frame(0);REQUIRE(begins==1 && undo_stack.size()==1);
    /* Undo is external to the widget; the next edit must resync its old value. */
    undo();REQUIRE(!strcmp(grass.density_mask_path,before.density_mask_path));
    path_mode="typing";frame(2);REQUIRE(undo_stack.size()==1 && redo_stack.empty());
    undo();REQUIRE(!strcmp(grass.density_mask_path,before.density_mask_path));
    reset();char output[256];memset(output,'Q',sizeof output);JcePathInputOpts opts;
    opts.changed_before=output;opts.changed_before_size=255;target="##gf_density_mask";action=2;
    REQUIRE(!jce_draw_path_input(target,grass.density_mask_path,sizeof grass.density_mask_path,JcePathKind::AssetVfs,&opts));
    REQUIRE(output[0]=='Q' && !strcmp(grass.density_mask_path,"textures/old-mask.png"));
    opts.changed_before_size=sizeof output;action=0;
    REQUIRE(!jce_draw_path_input(target,grass.density_mask_path,sizeof grass.density_mask_path,JcePathKind::AssetVfs,&opts));
    REQUIRE(output[0]=='Q');action=2;
    REQUIRE(jce_draw_path_input(target,grass.density_mask_path,sizeof grass.density_mask_path,JcePathKind::AssetVfs,&opts));
    REQUIRE(!strcmp(output,"textures/old-mask.png") && output[strlen(output)]=='\0');
    return 0;
}
'''


class GrassInspectorHistoryTests(unittest.TestCase):
    def test_flags_and_reseed_record_discrete_undo(self):
        execute(self, probe(DISCRETE_CONTROL))

    def test_all_twenty_continuous_fields_record_gesture_boundaries(self):
        execute(self, probe(CONTINUOUS_CONTROL))

    def test_actual_path_owner_returns_before_for_browse_type_drop_clear_and_undo(self):
        execute(self, probe(PATH_CONTROL))

    def test_negative_async_pre_edit_from_new_buffer_is_detected(self):
        execute(self, probe(PATH_CONTROL, "async_before"), negative=True)

    def test_negative_missing_visible_record_is_detected(self):
        execute(self, probe(DISCRETE_CONTROL, "visible"), negative=True)

    def test_negative_missing_density_tracker_is_detected(self):
        execute(self, probe(CONTINUOUS_CONTROL, "continuous"), negative=True)


if __name__ == "__main__":
    unittest.main()
