#ifndef BENCH_LOAD_WITNESS_H
#define BENCH_LOAD_WITNESS_H
#include <jce/api_app.h>
#include <jce/api_render.h>
#include <jce/api_audio.h>
#include <jce/api_resource.h>
#include <jce/api_graphics.h>
typedef struct BenchLoadWitness {
    JceModel *model;
    JceTexture texture;
    JceSound sound;
    JceAudio *audio;
    bool active;
} BenchLoadWitness;
typedef struct BenchLoadCpuPhase {
    JceModelCpu *model;
    JceTextureCpu *texture;
    JceAudioCpu *audio;
    void *encoded, *unpacked;
    JceFileSystem *fs;
    JceBundleFile *bundle;
    uint64_t read_bytes, packed_bytes, payload_bytes;
    uint64_t read_ns, open_ns, decompress_ns, decode_ns, decoded_at;
    uint64_t submit_ns;
    bool ok;
} BenchLoadCpuPhase;
bool bench_load_kind_valid(const char *kind);
bool bench_load_cpu_decode(BenchLoadCpuPhase *, const char *, const char *, const char *);
bool bench_load_submit(BenchLoadWitness *, BenchLoadCpuPhase *, const JceServices *, bool);
void bench_load_cpu_destroy(BenchLoadCpuPhase *);
void bench_load_phase_log(const BenchLoadCpuPhase *, const char *, unsigned, bool);
bool bench_load_prepare(bool *requested);
bool bench_load_witness(BenchLoadWitness *, const JceServices *, unsigned);
void bench_load_destroy(BenchLoadWitness *);
#endif
