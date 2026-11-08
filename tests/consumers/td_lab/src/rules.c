/* Independently authored bounded rules, specified from the reference behavior. */
#include "rules.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <jce/middleware/ai/jce_graph_astar.h>
const TdBalance td_balance[3] = {
    {1, 1, 1, 2.0f, 20, 1.6f}, {9, 4, 1, 1.0f, 25, 4.8f},
    {20, 3, 3, 0.75f, 30, 9.8f}
};
static uint32_t random_next(TdGame *g)
{
    g->rng ^= g->rng << 13; g->rng ^= g->rng >> 17; g->rng ^= g->rng << 5;
    return g->rng;
}
static double random_unit(TdGame *g) {
    if(g->random_cursor<g->random_count) return g->random_samples[g->random_cursor++];
    return random_next(g)/4294967296.0;
}
TdVec td_cell_position(int cell)
{
    TdVec v = {(float)(cell / TD_SIDE - 5) * 9, 0,
               (float)(5 - cell % TD_SIDE) * 9};
    return v;
}
static float distance2(TdVec a, TdVec b)
{
    float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return x*x + y*y + z*z;
}
/* Grid topology is game policy; graph search belongs to the public JCE AI API. */
typedef struct { const TdGame *game; int forbidden; } GridQuery;
static uint32_t neighbors(uint32_t cell,uint32_t *ids,float *costs,uint32_t capacity,void *user)
{
    GridQuery *q=(GridQuery *)user; int candidates[4],count=0,i; uint32_t n=0;
    if(cell/11>0) candidates[count++]=(int)cell-11;
    if(cell/11<10) candidates[count++]=(int)cell+11;
    if(cell%11>0) candidates[count++]=(int)cell-1;
    if(cell%11<10) candidates[count++]=(int)cell+1;
    for(i=0;i<count && n<capacity;++i) {
        int next=candidates[i];
        if(next==q->forbidden || q->game->towers[next].active) continue;
        ids[n]=(uint32_t)next; costs[n++]=1;
    }
    return n;
}
static float heuristic(uint32_t node,uint32_t goal,void *user)
{
    (void)user; return (float)(abs((int)(goal/11)-(int)(node/11))+abs((int)(goal%11)-(int)(node%11)));
}
int td_path(const TdGame *g,int start,int forbidden,int out[TD_CELLS])
{
    JceGraphAstar *search; GridQuery query={g,forbidden};
    uint32_t path[TD_CELLS],count=0,i; bool found;
    if(start<0||start>=TD_CELLS||start==forbidden) return 0;
    search=jce_graph_astar_create(); if(!search) return 0;
    jce_graph_astar_set_callbacks(search,neighbors,heuristic,&query);
    found=jce_graph_astar_search(search,(uint32_t)start,TD_CELLS-1,path,TD_CELLS,&count);
    jce_graph_astar_destroy(search);
    if(!found || count<2) return 0;
    for(i=1;i<count;++i) out[i-1]=(int)path[i];
    return (int)count-1;
}
void td_reset(TdGame *g, uint32_t seed)
{
    memset(g, 0, sizeof *g); g->rng = seed ? seed : 1;
    g->resources = 555; g->lives = 50;
}
static bool enemy_path(TdGame *g, TdEnemy *e)
{
    int path[TD_CELLS], count = td_path(g, e->cell, -1, path), i;
    if (!count) return false;
    e->length = count + 1; e->progress = 0;
    e->path[0] = e->position; e->cells[0] = e->cell;
    for (i = 0; i < count; ++i) {
        e->path[i+1] = td_cell_position(path[i]); e->cells[i+1] = path[i];
    }
    return true;
}
bool td_place(TdGame *g, int cell, int kind)
{
    int path[TD_CELLS], i;
    if (cell <= 0 || cell >= TD_CELLS-1 || kind < 0 || kind > 2 ||
        g->towers[cell].active || g->resources < td_balance[kind].cost ||
        !td_path(g, 0, cell, path)) return false;
    g->resources -= td_balance[kind].cost;
    g->towers[cell].active = true; g->towers[cell].kind = kind;
    g->towers[cell].target = -1; g->towers[cell].last_shot_ms = g->wall_epoch_ms ? g->wall_epoch_ms : (double)g->time*1000.0;
    for (i = 0; i < TD_ENEMIES; ++i)
        if (g->enemies[i].active) enemy_path(g, &g->enemies[i]);
    return true;
}
bool td_wave(TdGame *g)
{
    if (g->wave >= 2) return false;
    g->wave_pending[g->wave] = 5; g->wave_clock[g->wave] = 0;
    ++g->wave; g->pending += 5;
    return true;
}
int td_spawn(TdGame *g, int kind)
{
    int i;
    for (i = 0; i < TD_ENEMIES; ++i) if (!g->enemies[i].active) {
        TdEnemy *e = &g->enemies[i];
        memset(e, 0, sizeof *e); e->kind = kind == 1 ? 1 : 0;
        e->hp = e->kind ? 4 : 2; e->generation = ++g->next_generation;
        e->position = td_cell_position(0); e->cell = 0;
        if (!enemy_path(g, e)) return -1;
        e->position.y = e->kind ? 2 : 1; e->active = true; ++g->spawned;
        e->bob_phase = fmod((g->wall_epoch_ms ? g->wall_epoch_ms : g->time*1000.0) * .01525, 6.283185307179586);
        return i;
    }
    return -1;
}
static float spline(float a, float b, float c, float d, float t)
{
    float ab = (c-a)*0.5f, cd = (d-b)*0.5f;
    return ((2*b-2*c+ab+cd)*t + (-3*b+3*c-2*ab-cd))*t*t + ab*t + b;
}
static TdVec sample(const TdVec *points, int length, float p)
{
    float u = p * (length-1), t; int b = (int)u, a, c, d;
    TdVec v;
    if (b >= length-1) return points[length-1];
    a = b > 0 ? b-1 : 0; c = b+1; d = c+1 < length ? c+1 : c; t = u-b;
    v.x = spline(points[a].x, points[b].x, points[c].x, points[d].x, t);
    v.y = spline(points[a].y, points[b].y, points[c].y, points[d].y, t);
    v.z = spline(points[a].z, points[b].z, points[c].z, points[d].z, t);
    return v;
}
static int target(TdGame *g, TdVec origin, float range)
{
    int i, list[TD_ENEMIES], count = 0;
    for (i = 0; i < TD_ENEMIES; ++i)
        if (g->enemies[i].active && distance2(origin,g->enemies[i].position) <= range*range)
            list[count++] = i;
    return count ? list[(int)(random_unit(g)*count) % count] : -1;
}
static void fire(TdGame *g, int cell, int enemy, int shot)
{
    int i, kind = g->towers[cell].kind;
    for (i = 0; i < TD_BULLETS; ++i) if (!g->bullets[i].active) {
        TdBullet *b = &g->bullets[i];
        memset(b,0,sizeof *b); b->active = true; b->kind = kind == 2;
        b->target = enemy; b->generation = g->enemies[enemy].generation;
        b->damage = td_balance[kind].damage;
        b->position = td_cell_position(cell); b->position.y = td_balance[kind].height;
        if (kind == 1) { b->position.x -= (shot%2)*2; b->position.z += (shot/2)*2; }
        b->curve[0] = b->position;
        ++g->shots; return;
    }
}
static void step(TdGame *g, float seconds, bool legacy)
{
    const double now_ms = legacy ? (g->wall_epoch_ms ? g->wall_epoch_ms :
                          ((double)g->time+(double)seconds)*1000.0) :
                          (double)(g->frames+1)*1000.0/60.0;
    int i, j; ++g->frames;
    if (legacy) g->time += seconds;
    else g->time = (float)((double)g->frames/60.0);
    for (j=0;j<2;++j) if (g->wave_pending[j]) {
        g->wave_clock[j] += seconds;
        if (g->wave_clock[j] + 0.00001f >= 0.5f) {
            g->wave_clock[j] -= 0.5f;
            if (td_spawn(g,0) >= 0) { --g->wave_pending[j]; --g->pending; }
        }
    }
    for (i = 0; i < TD_CELLS; ++i) {
        TdTower *t = &g->towers[i]; const TdBalance *cfg;
        TdVec origin = td_cell_position(i);
        if (!t->active) continue;
        cfg = &td_balance[t->kind];
        /* All normal cooldowns use simulation time; legacy ordering is only
         * retained for explicit reference captures and diagnostic controls. */
        if (t->last_shot_ms+(double)cfg->interval*1000.0+0.0001 >=
            (legacy ? g->previous_time_ms : now_ms)) continue;
        t->last_shot_ms = now_ms;
        for (j = 0; j < cfg->shots; ++j) {
            if (t->kind || t->target < 0 || !g->enemies[t->target].active ||
                distance2(origin,g->enemies[t->target].position) > cfg->range*cfg->range)
                t->target = target(g,origin,cfg->range);
            if (t->target >= 0) fire(g,i,t->target,j);
        }
    }
    for (i = 0; i < TD_ENEMIES; ++i) {
        TdEnemy *e = &g->enemies[i]; int at;
        if (!e->active) continue;
        if (e->progress >= 1) {
            e->active = false;
            /* Remaining lives stop at zero; every arrival still despawns. */
            if (g->lives > 0) --g->lives;
            ++g->escaped; continue;
        }
        { TdVec previous=e->position;
            e->position = sample(e->path,e->length,e->progress);
            if (!e->kind) e->rotation=atan2f(e->position.z-previous.z,previous.x-e->position.x);
            else e->rotation += 0.015f;
        }
        e->position.y = e->kind ? (float)(2 + sin(e->bob_phase)*0.5) : 1;
        e->bob_phase += 0.0305;
        at = (int)((e->length-1)*e->progress); e->cell = e->cells[at];
        e->progress += 0.025f/(e->length+1);
    }
    for (i = 0; i < TD_BULLETS; ++i) {
        TdBullet *b = &g->bullets[i]; TdEnemy *e; float denominator;
        if (!b->active) continue;
        b->age += TD_STEP; e = &g->enemies[b->target];
        if (b->position.y < -1) { b->active = false; continue; }
        if (b->age <= TD_STEP) continue;
        if (!e->active || e->generation != b->generation) {
            ++b->lost_frames;
            if (!b->kind) {
                b->position.x += b->velocity.x; b->position.y += b->velocity.y;
                b->position.z += b->velocity.z;
            } else {
                b->position = sample(b->curve,4,b->progress); b->progress += 0.008f;
                if (b->progress>=1) b->active = false;
            }
            if (b->lost_frames>=180) b->active = false;
            continue;
        }
        if (distance2(e->position,b->position) <= 1) {
            e->hp -= b->damage; ++g->hits; b->active = false;
            if (e->hp <= 0) {
                if (!e->kind) {
                    TdBlood blood;
                    blood.rotation=(float)(random_unit(g)*3.14159265358979323846);
                    blood.position.x=e->position.x+(float)(random_unit(g)*2-1);
                    blood.position.z=e->position.z-(float)(random_unit(g)*2-1);
                    blood.position.y=0.1f;
                    if(g->blood_count<TD_BLOOD) g->blood[g->blood_count++]=blood;
                }
                e->active = false; ++g->kills;
                g->resources += e->kind ? 3 : 1; g->score += e->kind ? 300 : 100;
            }
            continue;
        }
        if (b->kind) {
            if(!b->curve_ready) {
                b->curve[0]=b->position; b->curve[1]=b->position;
                b->curve[1].x+=(float)(random_unit(g)*24-12);
                b->curve[1].z-=(float)(random_unit(g)*24-12);
                b->curve[1].y+=(float)(random_unit(g)*24);
                b->curve[2]=b->curve[1];
                b->curve[2].x+=(float)(random_unit(g)*4-2);
                b->curve[2].z-=(float)(random_unit(g)*4-2);
                b->curve[2].y+=(float)(random_unit(g)*4);
                b->curve_ready=true;
            }
            b->curve[3] = e->position; b->position = sample(b->curve,4,b->progress);
            b->progress += 0.008f; if (b->progress >= 1) b->active = false;
        } else {
            TdVec delta = {e->position.x-b->position.x,e->position.y-b->position.y,
                           e->position.z-b->position.z};
            denominator = fabsf(delta.x)+fabsf(delta.y)+fabsf(delta.z);
            if (denominator > 0.00001f) {
                b->velocity.x = delta.x/denominator; b->velocity.y = delta.y/denominator;
                b->velocity.z = delta.z/denominator;
                b->position.x += b->velocity.x; b->position.y += b->velocity.y;
                b->position.z += b->velocity.z;
            }
        }
    }
    g->previous_time_ms = now_ms;
}
void td_tick(TdGame *g, float seconds)
{
    const double quantum = 1.0/60.0;
    double accepted; int steps=0;
    if (!isfinite(seconds) || seconds <= 0) return;
    accepted = seconds > .25f ? .25 : (double)seconds;
    g->dropped_seconds += (double)seconds-accepted;
    g->accumulator += accepted;
    /* Bounded work prevents a loading/suspension hitch becoming a fast-forward.
     * Rates are calibrated to the reference at 60 Hz, independently of draw. */
    while (steps < 15 && g->accumulator + 0.0000001 >= quantum) {
        g->accumulator -= quantum;
        if (g->accumulator < 0) g->accumulator = 0;
        step(g,TD_STEP,false); ++steps;
    }
}
void td_frame(TdGame *g, float seconds)
{
    if (!isfinite(seconds) || seconds < 0) return;
    step(g,seconds,true);
}
uint64_t td_hash(const TdGame *g)
{
    const unsigned char *bytes = (const unsigned char *)g;
    uint64_t hash = UINT64_C(1469598103934665603); size_t i;
    for (i = 0; i < sizeof *g; ++i) { hash ^= bytes[i]; hash *= UINT64_C(1099511628211); }
    return hash;
}
