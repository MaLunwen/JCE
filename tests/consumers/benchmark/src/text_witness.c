#include "text_witness.h"
#include <jce/api_core.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "benchmark_text"

bool bench_text_witness_spawn(JceScene *scene)
{
    const char *mode = getenv("JCE_BENCH_TEXT");
    if (!mode || !mode[0]) return true;
    const char *font = getenv("JCE_BENCH_TEXT_FONT");
    const char *fallback = getenv("JCE_BENCH_TEXT_FALLBACK");
    if (strcmp(mode, "multilingual") || !font || !jce_fs_host_exists_file(font) ||
            strlen(font) >= sizeof(((JceUITextComponent *)0)->font_path) ||
            (fallback && fallback[0] && !jce_fs_host_exists_file(fallback)))
        return false;
    jce_ui_canvas_set_font_fallbacks(fallback);
    JceEntity root = jce_scene_create_entity(scene, "TextWitness");
    if (!root) return false;
    JceCanvasComponent canvas = {0};
    canvas.reference_resolution[0] = 1280;
    canvas.reference_resolution[1] = 720;
    canvas.match_width_or_height = 0.5f;
    jce_scene_set_canvas(scene, root, &canvas);
    static const char *lines[] = {
        "English: office ffi AV 0123456789",
        "Chinese: \xe4\xb8\xad\xe6\x96\x87\xe6\xb5\x8b\xe8\xaf\x95 \xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c",
        "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85",
        "Mixed: JCE \xe4\xb8\xad\xe6\x96\x87 123 \xce\xa9"
    };
    for (uint32_t i = 0; i < 4u; ++i) {
        JceEntity entity = jce_scene_create_entity(scene, "TextLine");
        if (!entity) return false;
        JceUITextComponent text = {0};
        memcpy(text.text, lines[i], strlen(lines[i]) + 1u);
        memcpy(text.font_path, font, strlen(font) + 1u);
        text.font_size = 40;
        text.color[0] = text.color[1] = text.color[2] = text.color[3] = 1;
        text.rect.anchor_min[1] = text.rect.anchor_max[1] = 1;
        text.rect.pivot[1] = 1;
        text.rect.anchored_position[0] = 70;
        text.rect.anchored_position[1] = -(70.0f + i * 125.0f);
        text.rect.size_delta[0] = 1140;
        text.rect.size_delta[1] = 100;
        jce_scene_set_parent(scene, entity, root);
        jce_scene_set_ui_text(scene, entity, &text);
    }
    LOG_INFO(LOG_TAG, "benchmark-text: mode=multilingual labels=4 font=%s fallback=%s",
             font, fallback ? fallback : "");
    return true;
}
