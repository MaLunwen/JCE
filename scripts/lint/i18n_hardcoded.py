import re, sys
from pathlib import Path
# Find ImGui::Text-like and Begin / Button / MenuItem calls with hardcoded
# English literal strings (not via jce_editor_i18n / not starting with #).
# Heuristic: literal of >=4 chars containing a letter and a space, NOT inside
# a jce_editor_i18n call.
pat_call = re.compile(
    r'ImGui::(?:Text|TextUnformatted|TextDisabled|TextColored|TextWrapped|'
    r'Button|SmallButton|MenuItem|BeginMenu|BeginTabItem|CollapsingHeader|'
    r'TreeNode|Selectable|Checkbox|InputText|SliderFloat|DragFloat|Combo|'
    r'BulletText|LabelText|SetTooltip)\s*\(([^;]*?)\)\s*;', re.DOTALL)
i18n_pat = re.compile(r'jce_editor_i18n')
str_pat = re.compile(r'"((?:[^"\\]|\\.)*?)"')
panels = [
    'jce_panel_vfx_graph','jce_panel_sprite_editor','jce_panel_tile_palette',
    'jce_panel_lighting','jce_panel_audio_mixer','jce_panel_input_manager',
    'jce_panel_package_manager','jce_panel_frame_debugger',
    'jce_panel_test_runner','jce_panel_build_profiles',
    'jce_panel_animation_editor','jce_panel_animator_sm','jce_panel_curve_editor',
    'jce_panel_sequencer','jce_panel_material_graph','jce_panel_particle_editor',
    'jce_panel_navmesh','jce_panel_terrain','jce_panel_lightmap_bake',
    'jce_panel_profiler']
root = Path('editor/src/panels')
total = 0
for name in panels:
    p = root / (name + '.cpp')
    if not p.exists(): continue
    src = p.read_text(encoding='utf-8')
    found = []
    for m in pat_call.finditer(src):
        chunk = m.group(0)
        if i18n_pat.search(chunk): continue
        for sm in str_pat.finditer(chunk):
            s = sm.group(1)
            # ignore format specifiers / IDs / control codes
            if not s: continue
            if s.startswith('##') or s.startswith('###'): continue
            if s.startswith('%'): continue
            if all(c in '%sdfxlu .,:#-+0123456789' for c in s): continue
            if not re.search(r'[A-Za-z]{3}', s): continue
            if ' ' in s or s[0].isupper():
                # filter common short tokens
                if len(s) >= 3:
                    line = src.count('\n',0,m.start())+1
                    found.append((line, s[:80]))
                    break
    if found:
        print(f'== {name} =={len(found)}')
        for ln, s in found[:50]: print(f'  L{ln}: {s}')
        total += len(found)
print('TOTAL hardcoded:', total)
