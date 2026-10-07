#ifndef BENCH_STREAM_ROUTE_H
#define BENCH_STREAM_ROUTE_H
#include <jce/api_scene.h>
#include <jce/api_render.h>
#include <jce/api_graphics.h>
#include <jce/api_streaming.h>
#include <jce/api_resource.h>
typedef struct BenchStreamRoute {
    JceWorldStreamer *world;
    JceFileSystem *fs;
    JceScene *scene;
    JceSceneRenderer *renderer;
    JceRenderer *device;
    uint64_t spawned, despawned, budget, started_ms;
    unsigned step, steps, expected_vertices, expected_primitives;
    bool active;
} BenchStreamRoute;
bool bench_stream_route_init(BenchStreamRoute *, JceScene *, JceSceneRenderer *, JceRenderer *);
bool bench_stream_route_update(BenchStreamRoute *, JceCamera *, unsigned frame);
bool bench_stream_route_observe(BenchStreamRoute *, unsigned frame);
void bench_stream_route_destroy(BenchStreamRoute *);
#endif
