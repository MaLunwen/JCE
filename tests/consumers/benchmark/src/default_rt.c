#include "default_rt.h"
#include <jce/api_core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static JceRenderReadbackQueue *queue;
static JceRenderReadbackTicket ticket;
static const char *target_name;
static const char *output_prefix;
static bool completed;

bool bench_default_rt_init(const JceServices *services)
{
    target_name = getenv("JCE_BENCH_RT_TARGET");
    output_prefix = getenv("JCE_BENCH_RT_OUTPUT");
    if (!target_name && !output_prefix) return true;
    if (!target_name || !output_prefix || !output_prefix[0] ||
        (strcmp(target_name, "gbuffer.normal") &&
         strcmp(target_name, "gbuffer.albedo") &&
         strcmp(target_name, "reflection.ssr"))) return false;
    queue = jce_render_readback_create(services->renderer);
    return queue != NULL;
}

void bench_default_rt_tick(const JceSceneRenderer *renderer, unsigned frame)
{
    if (!queue || completed) return;
    if (ticket) {
        JceRenderReadbackState state = jce_render_readback_poll(queue, ticket);
        if (state == JCE_RENDER_READBACK_PENDING) return;
        if (state == JCE_RENDER_READBACK_READY) {
            JceRenderReadbackInfo info = {0};
            void *pixels = NULL;
            char path[1536], metadata[384];
            bool ok = jce_render_readback_get_info(queue, ticket, &info) &&
                      info.format == JCE_RENDER_FORMAT_RGBA8 &&
                      info.byte_size == (uint64_t)info.width * info.height * 4u &&
                      info.byte_size <= SIZE_MAX;
            if (ok) pixels = jce_malloc((size_t)info.byte_size);
            ok = ok && pixels && jce_render_readback_copy(queue, ticket, pixels,
                                                        (size_t)info.byte_size);
            int n = snprintf(path, sizeof(path), "%s.rgba", output_prefix);
            ok = ok && n > 0 && n < (int)sizeof(path) &&
                 jce_fs_host_write_all(path, pixels, info.byte_size);
            n = snprintf(metadata, sizeof(metadata),
                "{\"target\":\"%s\",\"width\":%u,\"height\":%u,"
                "\"format\":\"RGBA8\",\"bottom_up\":%s,"
                "\"request_frame\":60,\"completion_frame\":%u}\n",
                target_name, info.width, info.height,
                jce_renderer_origin_bottom_left() ? "true" : "false", frame);
            ok = ok && n > 0 && n < (int)sizeof(metadata);
            int path_size = snprintf(path, sizeof(path), "%s.json", output_prefix);
            ok = ok && path_size > 0 && path_size < (int)sizeof(path) &&
                 jce_fs_host_write_all(path, metadata, (uint64_t)n);
            jce_free(pixels);
            if (ok) LOG_INFO("stock_rt", "typed target completed: %s frame=%u", target_name, frame);
            else LOG_ERROR("stock_rt", "%s", "typed target copy/write failed");
        } else LOG_ERROR("stock_rt", "%s", "typed target readback failed");
        jce_render_readback_release(queue, ticket);
        ticket = 0;
        completed = true;
        return;
    }
    if (frame == 60) {
        int count = jce_scene_renderer_debug_target_count(renderer);
        for (int i = 0; i < count; ++i) {
            JceRenderTargetInfo target;
            if (!jce_scene_renderer_debug_target_get(renderer, i, &target) ||
                strcmp(target.name, target_name)) continue;
            JceRenderReadbackDesc desc = jce_render_readback_desc_default();
            desc.source = target.texture;
            desc.format = JCE_RENDER_FORMAT_RGBA8;
            desc.width = target.width;
            desc.height = target.height;
            desc.view_id = 249;
            if (jce_render_readback_submit(queue, &desc, &ticket)) return;
            break;
        }
        LOG_ERROR("stock_rt", "typed target unavailable: %s", target_name);
        completed = true;
    }
}

void bench_default_rt_exit(void)
{
    if (queue && !completed) LOG_ERROR("stock_rt", "%s", "typed target observation incomplete");
    jce_render_readback_destroy(queue);
    queue = NULL;
}
