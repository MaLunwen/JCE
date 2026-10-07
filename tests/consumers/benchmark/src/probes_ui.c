#include "probes.h"
#include <jce/api_scene.h>
#include <math.h>
#include <string.h>

#define WARMUP 2u
#define WIDTH 1280.0f
#define HEIGHT 720.0f

bool bench_probe_ui(const char *name, const BenchProbeConfig *config)
{
    if (config->count > 10000u || config->concurrency != 1u || config->depth != 1u)
        return false;
    JceScene *scene = jce_scene_create();
    JceUICanvas *ui = jce_ui_canvas_create(NULL, NULL);
    JceEntity *entities = jce_malloc((size_t)config->count * sizeof(*entities));
    bool correct = scene && ui && entities;
    JceEntity root = scene ? jce_scene_create_entity(scene, "LayoutProbe") : 0;
    JceCanvasComponent canvas = {0};
    canvas.reference_resolution[0] = WIDTH;
    canvas.reference_resolution[1] = HEIGHT;
    canvas.match_width_or_height = 0.5f;
    if (root) jce_scene_set_canvas(scene, root, &canvas);
    else correct = false;
    for (uint32_t i = 0; correct && i < config->count; ++i) {
        entities[i] = jce_scene_create_entity(scene, "ImageProbe");
        correct = entities[i] != 0;
        JceUIImageComponent image;
        memset(&image, 0, sizeof(image));
        image.color[0] = image.color[1] = image.color[2] = image.color[3] = 1;
        image.raycast_target = true;
        image.rect.anchor_min[1] = image.rect.anchor_max[1] = 1;
        image.rect.pivot[1] = 1;
        image.rect.anchored_position[0] = 10.0f + (float)(i % 100u) * 12.0f;
        image.rect.anchored_position[1] = -(10.0f + (float)(i / 100u) * 6.0f);
        image.rect.size_delta[0] = 8;
        image.rect.size_delta[1] = 4;
        if (correct) {
            jce_scene_set_parent(scene, entities[i], root);
            jce_scene_set_ui_image(scene, entities[i], &image);
        }
    }
    for (uint32_t round = 0; correct && round < config->rounds + WARMUP; ++round) {
        uint64_t started = jce_time_perf_counter();
        jce_ui_canvas_render(ui, scene, 0, UINT16_MAX, WIDTH, HEIGHT, NULL, 1.0f / 60.0f);
        uint64_t ended = jce_time_perf_counter();
        uint64_t checksum = 0;
        for (uint32_t i = 0; correct && i < config->count; ++i) {
            float x = 0, y = 0, width = 0, height = 0;
            correct = jce_ui_canvas_entity_rect(ui, entities[i], &x, &y, &width, &height) &&
                fabsf(x - (10.0f + (i % 100u) * 12.0f)) < 0.01f &&
                fabsf(y - (10.0f + (i / 100u) * 6.0f)) < 0.01f &&
                fabsf(width - 8.0f) < 0.01f && fabsf(height - 4.0f) < 0.01f;
            if (correct) ++checksum;
        }
        if (round >= WARMUP)
            bench_probe_sample(name, config, round - WARMUP, started, ended,
                               correct ? config->count : 0u, checksum, correct);
    }
    if (ui) jce_ui_canvas_destroy(ui);
    if (scene) jce_scene_destroy(scene);
    jce_free(entities);
    return correct;
}
