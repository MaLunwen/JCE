#ifndef BENCH_ASYNC_LOAD_WITNESS_H
#define BENCH_ASYNC_LOAD_WITNESS_H
#include "load_witness.h"
#include <jce/api_core.h>
typedef struct BenchAsyncLoad {
    JceAsyncTask *task;
    BenchLoadWitness loaded;
    char kind[16], path[4096], entry[4096];
    uint64_t requested_ns, main_thread;
    bool requested, complete;
} BenchAsyncLoad;
bool bench_async_load_tick(BenchAsyncLoad *, const JceServices *, unsigned frame);
void bench_async_load_destroy(BenchAsyncLoad *);
#endif
