/* Rules contain no renderer, UI, file system or model dependency. */
#include "board.h"
#include <string.h>

static uint32_t next_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *state = x;
    return x;
}

int mines_neighbors(const MinesBoard *b, int index, int out[8])
{
    int x, y, dx, dy, n = 0;
    if (index < 0 || index >= b->width * b->height) return 0;
    x = index % b->width; y = index / b->width;
    for (dy = -1; dy <= 1; ++dy) for (dx = -1; dx <= 1; ++dx) {
        int nx = x + dx, ny = y + dy;
        if ((dx || dy) && nx >= 0 && nx < b->width && ny >= 0 && ny < b->height)
            out[n++] = ny * b->width + nx;
    }
    return n;
}

bool mines_reset(MinesBoard *b, int w, int h, int mines, uint32_t seed)
{
    if (!b || w < 5 || w > 30 || h < 5 || h > 16 || mines < 1 || mines > w*h-9)
        return false;
    memset(b, 0, sizeof *b);
    b->width = w; b->height = h; b->mines = mines;
    b->seed = seed ? seed : 1; b->state = MINES_READY;
    return true;
}

static void generate(MinesBoard *b, int first)
{
    int candidates[MINES_CAPACITY], count = 0, i, k, neighbors[8];
    int fx = first % b->width, fy = first / b->width;
    uint32_t random = b->seed;
    for (i = 0; i < b->width*b->height; ++i) {
        int dx = i % b->width - fx, dy = i / b->width - fy;
        if (dx < -1 || dx > 1 || dy < -1 || dy > 1) candidates[count++] = i;
    }
    /* Partial Fisher-Yates: exactly mines selections, no retry loop. */
    for (i = 0; i < b->mines; ++i) {
        int j = i + (int)(next_random(&random) % (uint32_t)(count-i));
        int v = candidates[i]; candidates[i] = candidates[j]; candidates[j] = v;
        b->cells[candidates[i]].mine = true;
    }
    for (i = 0; i < b->width*b->height; ++i) {
        int n = mines_neighbors(b, i, neighbors);
        for (k = 0; k < n; ++k) if (b->cells[neighbors[k]].mine) ++b->cells[i].adjacent;
    }
    b->state = MINES_PLAYING;
}

static void reveal_safe(MinesBoard *b, int first)
{
    int queue[MINES_CAPACITY], head = 0, tail = 0;
    b->cells[first].revealed = true; ++b->revealed;
    queue[tail++] = first;
    while (head < tail) {
        int i = queue[head++], nearby[8], n, k;
        if (b->cells[i].adjacent) continue;
        n = mines_neighbors(b, i, nearby);
        for (k = 0; k < n; ++k) {
            MinesCell *c = &b->cells[nearby[k]];
            if (c->revealed || c->flagged || c->mine) continue;
            /* Mark at enqueue time, so every cell enters the bounded queue once. */
            c->revealed = true; ++b->revealed;
            queue[tail++] = nearby[k];
        }
    }
}

bool mines_reveal(MinesBoard *b, int index)
{
    MinesCell *c;
    if (b->state >= MINES_WON || index < 0 || index >= b->width*b->height) return false;
    c = &b->cells[index];
    if (c->revealed || c->flagged) return false;
    if (b->state == MINES_READY) generate(b, index);
    if (c->mine) { c->exploded = true; b->state = MINES_LOST; return true; }
    reveal_safe(b, index);
    if (b->revealed == b->width*b->height-b->mines) b->state = MINES_WON;
    return true;
}

bool mines_flag(MinesBoard *b, int index)
{
    MinesCell *c;
    if (b->state >= MINES_WON || index < 0 || index >= b->width*b->height) return false;
    c = &b->cells[index];
    if (c->revealed) return false;
    c->flagged = !c->flagged; b->flags += c->flagged ? 1 : -1;
    return true;
}

bool mines_chord(MinesBoard *b, int index)
{
    int nearby[8], n, k, flags = 0;
    bool changed = false;
    if (b->state != MINES_PLAYING || index < 0 || index >= b->width*b->height ||
        !b->cells[index].revealed || !b->cells[index].adjacent) return false;
    n = mines_neighbors(b, index, nearby);
    for (k = 0; k < n; ++k) if (b->cells[nearby[k]].flagged) ++flags;
    if (flags != b->cells[index].adjacent) return false;
    for (k = 0; k < n && b->state == MINES_PLAYING; ++k)
        if (mines_reveal(b, nearby[k])) changed = true;
    return changed;
}

void mines_tick(MinesBoard *b, double dt)
{
    if (b->state == MINES_PLAYING && dt > 0.0 && dt < 60.0) b->elapsed += dt;
}
