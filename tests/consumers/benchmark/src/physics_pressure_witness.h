#ifndef BENCH_PHYSICS_PRESSURE_WITNESS_H
#define BENCH_PHYSICS_PRESSURE_WITNESS_H
#include <jce/api_scene.h>
#include <jce/api_app.h>
#include <jce/api_physics.h>

typedef struct BenchPressureBody BenchPressureBody;
typedef struct BenchPressureHandle BenchPressureHandle;
typedef struct BenchPhysicsPressure {
    JceScene *scene;
    JcePhysicsWorld *world;
    BenchPressureBody *bodies;
    BenchPressureHandle *handles;
    const char *kind, *output;
    unsigned frame, count, collected, rigidbodies, statics;
    uint64_t begin_events, persist_events, end_events;
    bool requested, enabled, observe, listener, valid;
} BenchPhysicsPressure;
bool bench_physics_pressure_requested(void);
bool bench_physics_pressure_enabled(void);
bool bench_physics_pressure_initialize(BenchPhysicsPressure *s,
                                      JceScene *scene, JceRuntime *runtime);
void bench_physics_pressure_before(BenchPhysicsPressure *s, unsigned frame);
bool bench_physics_pressure_after(BenchPhysicsPressure *s, unsigned frame);
void bench_physics_pressure_destroy(BenchPhysicsPressure *s);
#endif
