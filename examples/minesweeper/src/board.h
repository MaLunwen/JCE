/* Bounded, allocation-free rules shared by the game and its checks. */
#ifndef MINES_BOARD_H
#define MINES_BOARD_H
#include <stdbool.h>
#include <stdint.h>
#define MINES_CAPACITY 480

typedef enum { MINES_READY, MINES_PLAYING, MINES_WON, MINES_LOST } MinesState;
typedef struct { bool mine, revealed, flagged, exploded; unsigned char adjacent; } MinesCell;
typedef struct {
    int width, height, mines, flags, revealed;
    uint32_t seed;
    double elapsed;
    MinesState state;
    MinesCell cells[MINES_CAPACITY];
} MinesBoard;
bool mines_reset(MinesBoard *b, int w, int h, int mines, uint32_t seed);
bool mines_reveal(MinesBoard *b, int index);
bool mines_flag(MinesBoard *b, int index);
bool mines_chord(MinesBoard *b, int index);
void mines_tick(MinesBoard *b, double dt);
int mines_neighbors(const MinesBoard *b, int index, int out[8]);
int mines_self_test(void);
#endif
