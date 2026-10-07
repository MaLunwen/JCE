/* Public SDK component-presence transition; no alternate rendering path. */
#include "nodraw_witness.h"
#include <jce/api_core.h>
#include <stdlib.h>
#include <string.h>

typedef struct { JceEntity target; unsigned matches, lines; } Observation;
static void observe_entity(JceScene *scene, JceEntity entity, void *user)
{
    Observation *out = user;
    const JceEditorMeta *meta = jce_scene_get_editor_meta(scene, entity);
    if (meta && !strcmp(meta->name, "TransitionLine")) {
        out->target = entity; ++out->matches;
    }
    if (jce_scene_has_line_renderer(scene, entity)) ++out->lines;
}

bool bench_nodraw_update(JceScene *scene, unsigned frame)
{
    const char *mode = getenv("JCE_BENCH_NODRAW");
    if (!mode || !*mode) return true;
    bool steady = !strcmp(mode, "steady"), add = !strcmp(mode, "add");
    bool cycle = !strcmp(mode, "cycle"), empty = !strcmp(mode, "empty");
    if (!steady && !add && !cycle && !empty) return false;
    if (frame != 1 && frame != 30 && frame != 50 && frame != 70 && frame != 90) return true;
    Observation state = {0};
    jce_scene_each_entity(scene, observe_entity, &state);
    if (state.matches != 1) return false;
    unsigned added = 0, removed = 0;
    if ((steady && frame == 1) || ((add || cycle) && frame == 30)) {
        if (jce_scene_has_line_renderer(scene, state.target)) return false;
        JceLineRendererComponent line = {0};
        line.position_count = 2; line.use_world_space = true;
        line.positions[0][0] = -8; line.positions[0][1] = -4;
        line.positions[1][0] = 8; line.positions[1][1] = 4;
        line.width_start = line.width_end = 1;
        line.color_start[0] = line.color_end[0] = 1;
        line.color_start[1] = line.color_end[1] = 0.2f;
        line.color_start[3] = line.color_end[3] = 1;
        jce_scene_set_line_renderer(scene, state.target, &line); added = 1;
    }
    if (cycle && frame == 70) {
        if (!jce_scene_has_line_renderer(scene, state.target)) return false;
        jce_scene_remove_line_renderer(scene, state.target); removed = 1;
    }
    state.lines = 0; state.matches = 0;
    jce_scene_each_entity(scene, observe_entity, &state);
    unsigned expected = steady || (add && frame >= 30) || (cycle && frame >= 30 && frame < 70);
    if (state.matches != 1 || state.lines != expected ||
        jce_scene_has_line_renderer(scene, state.target) != (expected != 0)) return false;
    LOG_INFO("benchmark_nodraw", "benchmark-nodraw: mode=%s frame=%u added=%u removed=%u lines=%u target=%llu present=%u flags=%llu",
             mode, frame, added, removed, state.lines,
             (unsigned long long)state.target, expected,
             (unsigned long long)jce_scene_get_component_flags(scene, state.target));
    return true;
}
