/* Private LineRenderer colour schema, shared by full JSON parsing and the
 * bounded colour-only component patch. No live pointer survives the call. */
#ifndef JCE_SCENE_LINE_COLORS_INTERNAL_H
#define JCE_SCENE_LINE_COLORS_INTERNAL_H

#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_json.h>
#include <float.h>
#include <math.h>
#include <string.h>

static const char *const scene_line_color_keys[8] = {
    "colorStartR", "colorStartG", "colorStartB", "colorStartA",
    "colorEndR", "colorEndG", "colorEndB", "colorEndA"
};

/* Full parsing defaults to white; an admitted partial patch keeps unmentioned
 * live channels. Both use the same numeric conversion and field schema. */
static inline void scene_line_parse_colors(JceLineRendererComponent *line,
                                           const JceJson *json, bool partial)
{
    for (int i = 0; i < 4; ++i) {
        line->color_start[i] = (float)jce_json_get_number(json, scene_line_color_keys[i],
            partial ? line->color_start[i] : 1.0);
        line->color_end[i] = (float)jce_json_get_number(json, scene_line_color_keys[i + 4],
            partial ? line->color_end[i] : 1.0);
    }
}

static inline bool scene_line_try_color_patch(JceScene *scene, JceEntity entity,
                                              const JceJson *patch)
{
    unsigned seen = 0;
    for (const JceJson *item = jce_json_first_child(patch); item;
         item = jce_json_next_sibling(item)) {
        int i = 0;
        const char *key = jce_json_member_key(item);
        if (!key) return false;
        while (i < 8 && strcmp(key, scene_line_color_keys[i]) != 0) ++i;
        const double number = jce_json_number_value(item, 0.0);
        if (i == 8 || (seen & (1u << i)) || !jce_json_is_number(item) ||
            !isfinite(number) || number > FLT_MAX || number < -FLT_MAX) return false;
        seen |= 1u << i;
    }
    if (!seen) return false;

    const JceLineRendererComponent *current = jce_scene_get_line_renderer(scene, entity);
    if (!current || current->position_count < 0 ||
        current->position_count > JCE_LINE_MAX_POINTS) return false;
    const char *end = memchr(current->material_path, '\0', sizeof current->material_path);
    if (!end || !isfinite(current->width_start) || !isfinite(current->width_end))
        return false;
    for (int i = 0; i < 4; ++i)
        if (!isfinite(current->color_start[i]) || !isfinite(current->color_end[i]))
            return false;
    for (int i = 0; i < current->position_count; ++i)
        for (int axis = 0; axis < 3; ++axis)
            if (!isfinite(current->positions[i][axis])) return false;

    JceLineRendererComponent value = *current;
    /* Match the ordinary parser's zeroed inactive storage and string tail,
     * rather than preserving bytes that a full round trip would discard. */
    const size_t material_size = (size_t)(end - current->material_path) + 1;
    memset(value.material_path + material_size, 0, sizeof value.material_path - material_size);
    memset(value.positions + value.position_count, 0,
           (JCE_LINE_MAX_POINTS - (size_t)value.position_count) * sizeof value.positions[0]);
    scene_line_parse_colors(&value, patch, true);
    jce_scene_set_line_renderer(scene, entity, &value);
    return true;
}

#endif
