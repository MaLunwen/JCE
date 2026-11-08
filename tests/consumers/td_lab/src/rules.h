#ifndef TD_RULES_H
#define TD_RULES_H
#include <stdbool.h>
#include <stdint.h>
#define TD_SIDE 11
#define TD_CELLS 121
#define TD_ENEMIES 500
#define TD_BULLETS 1000
#define TD_BLOOD 500
#define TD_STEP (1.0f / 60.0f)
typedef struct { float x, y, z; } TdVec;
typedef struct { int cost, shots, damage; float interval, range, height; } TdBalance;
typedef struct { bool active; int kind, target; double last_shot_ms; } TdTower;
typedef struct {
    bool active; int kind, hp, generation, cell, length;
    float progress, rotation; double bob_phase; TdVec position, path[TD_CELLS + 1];
    int cells[TD_CELLS + 1];
} TdEnemy;
typedef struct {
    bool active; int kind, target, generation, damage, lost_frames;
    bool curve_ready; float age, progress; TdVec position, velocity, curve[4];
} TdBullet;
typedef struct {
    TdVec position; float rotation;
} TdBlood;
typedef struct {
    TdTower towers[TD_CELLS]; TdEnemy enemies[TD_ENEMIES];
    TdBullet bullets[TD_BULLETS];
    TdBlood blood[TD_BLOOD]; int blood_count;
    int resources, score, lives, wave, pending, spawned, kills, hits, shots, escaped;
    float time, spawn_clock;
    double accumulator, dropped_seconds;
    double wall_epoch_ms, previous_time_ms; uint64_t frames;
    float wave_clock[2]; int wave_pending[2];
    uint32_t rng; int next_generation;
    /* Optional recorded random input for reference replay, never pose data. */
    double random_samples[4096]; int random_count, random_cursor;
} TdGame;
extern const TdBalance td_balance[3];
TdVec td_cell_position(int cell);
int td_path(const TdGame *g, int start, int forbidden, int out[TD_CELLS]);
void td_reset(TdGame *g, uint32_t seed);
bool td_place(TdGame *g, int cell, int kind);
bool td_wave(TdGame *g);
int td_spawn(TdGame *g, int kind);
/* Normal game clock: 60 Hz, at most 15 steps per call; excess time is paused. */
void td_tick(TdGame *g, float seconds);
/* Explicit legacy diagnostic only: one movement update per presented frame. */
void td_frame(TdGame *g, float seconds);
uint64_t td_hash(const TdGame *g);
int td_tests(void);
#endif
