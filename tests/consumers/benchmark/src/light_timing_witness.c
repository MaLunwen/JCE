/* One public component inventory before the measured interval; no draw policy. */
#include "light_timing_witness.h"
#include <jce/api_core.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct LightInventory {
    unsigned point, spot, directional, area, positive;
} LightInventory;

static void count_lights(JceScene *scene, JceEntity entity, void *user)
{
    LightInventory *counts = user;
    const JcePointLight *point = jce_scene_get_point_light(scene, entity);
    const JceSpotLight *spot = jce_scene_get_spot_light(scene, entity);
    const JceDirectionalLight *directional = jce_scene_get_dir_light(scene, entity);
    const JceAreaLight *area = jce_scene_get_area_light(scene, entity);
    if (point) { ++counts->point; counts->positive += isfinite(point->intensity) && point->intensity > 0; }
    if (spot) { ++counts->spot; counts->positive += isfinite(spot->intensity) && spot->intensity > 0; }
    if (directional) { ++counts->directional; counts->positive += isfinite(directional->intensity) && directional->intensity > 0; }
    if (area) { ++counts->area; counts->positive += isfinite(area->intensity) && area->intensity > 0; }
}

bool bench_light_timing_observe(JceScene *scene, unsigned frame)
{
    const char *kind = getenv("JCE_BENCH_LIGHT_TIMING_KIND");
    if (!kind || frame != 100) return true;
    const char *text = getenv("JCE_BENCH_LIGHT_TIMING_COUNT");
    unsigned expected;
    if (text && !strcmp(text, "0")) expected = 0;
    else if (text && !strcmp(text, "10")) expected = 10;
    else if (text && !strcmp(text, "100")) expected = 100;
    else return false;
    if (strcmp(kind, "point") && strcmp(kind, "spot")) return false;
    LightInventory counts = {0};
    jce_scene_each_entity(scene, count_lights, &counts);
    LOG_INFO("benchmark_light_timing", "benchmark-light-timing: frame=100 kind=%s requested=%u point=%u spot=%u directional=%u area=%u positive=%u",
        kind, expected, counts.point, counts.spot, counts.directional, counts.area, counts.positive);
    return counts.point == (!strcmp(kind, "point") ? expected : 0) &&
        counts.spot == (!strcmp(kind, "spot") ? expected : 0) &&
        !counts.directional && !counts.area && counts.positive == expected;
}
