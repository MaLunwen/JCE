#include "rules.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#define CHECK(x) do { ++checks; if (!(x)) { printf("TD_TEST FAIL line=%d: %s\n", __LINE__, #x); return 1; } } while (0)
int td_tests(void)
{
    static TdGame g, copy; int path[TD_CELLS], checks = 0, i, repeat; uint64_t expected = 0;
    td_reset(&g,7); CHECK(g.resources == 555 && g.lives == 50 && g.wave == 0);
    CHECK(td_path(&g,0,-1,path) == 20);
    CHECK(!td_place(&g,0,0) && !td_place(&g,120,0) && !td_place(&g,-1,0));
    CHECK(td_place(&g,60,1) && g.resources == 546);
    CHECK(!td_place(&g,60,2) && g.resources == 546);
    g.resources = 0; CHECK(!td_place(&g,61,2));
    td_reset(&g,7);
    for (i = 1; i <= 10; ++i) CHECK(td_place(&g,i,0));
    CHECK(!td_place(&g,11,0)); CHECK(!g.towers[11].active);
    td_reset(&g,7); CHECK(td_wave(&g));
    for (i = 0; i < 180; ++i) td_tick(&g,TD_STEP);
    CHECK(g.spawned == 5 && g.pending == 0); CHECK(td_wave(&g));
    for (i = 0; i < 1400; ++i) td_tick(&g,TD_STEP);
    CHECK(!td_wave(&g)); CHECK(g.escaped == 10 && g.lives == 40);
    td_reset(&g,7); CHECK(td_wave(&g) && td_wave(&g) && !td_wave(&g));
    for(i=0;i<180;++i) td_tick(&g,TD_STEP);
    CHECK(g.spawned==10);
    /* Reaching zero does not stop actor retirement or escape accounting. */
    td_reset(&g,7); g.lives=1; CHECK(td_spawn(&g,0)==0);
    g.enemies[0].progress=1; td_tick(&g,TD_STEP);
    CHECK(g.lives==0 && g.escaped==1 && !g.enemies[0].active);
    CHECK(g.resources==555 && g.score==0 && g.kills==0);
    CHECK(td_spawn(&g,1)==0);
    g.enemies[0].progress=1; td_tick(&g,TD_STEP);
    CHECK(g.lives==0 && g.escaped==2 && !g.enemies[0].active);
    CHECK(g.spawned==2 && g.frames==2);
    /* Multiple arrivals in one fixed step cannot overdraw remaining lives. */
    td_reset(&g,7); g.lives=2;
    for(i=0;i<3;++i) {
        CHECK(td_spawn(&g,i%2)==i); g.enemies[i].progress=1;
    }
    td_tick(&g,TD_STEP); CHECK(g.lives==0 && g.escaped==3);
    for(i=0;i<3;++i) CHECK(!g.enemies[i].active);
    CHECK(g.spawned==3 && g.resources==555 && g.score==0 && g.kills==0);
    for (repeat = 0; repeat < 10; ++repeat) {
        td_reset(&g,7); CHECK(td_place(&g,11,1)); CHECK(td_place(&g,23,2));
        CHECK(td_wave(&g));
        for (i = 0; i < 1000; ++i) td_tick(&g,TD_STEP);
        CHECK(g.hits > 0 && g.kills > 0 && g.score == g.kills*100);
        if (!repeat) expected = td_hash(&g); else CHECK(td_hash(&g) == expected);
    }
    copy = g; td_tick(&g,-1); td_tick(&g,NAN); CHECK(memcmp(&g,&copy,sizeof g) == 0);
    td_reset(&g,7);
    for (i = 0; i < TD_ENEMIES; ++i) CHECK(td_spawn(&g,i%2) == i);
    CHECK(td_spawn(&g,0) == -1);
    for (i = 1; i <= 100; ++i) CHECK(td_place(&g,i,0) || td_path(&g,0,i,path) == 0);
    for (i = 0; i < 300; ++i) td_tick(&g,TD_STEP);
    /* Normal gameplay uses one simulation clock at all presentation rates. */
    td_reset(&g,7); CHECK(td_place(&g,11,1) && td_place(&g,23,2)); CHECK(td_wave(&g));
    for(i=0;i<600;++i) td_tick(&g,1.0f/60.0f);
    CHECK(g.frames==600 && fabsf(g.time-10.0f)<.00001f); copy=g;
    for(repeat=0;repeat<3;++repeat) {
        int rate=repeat==0?30:repeat==1?120:144;
        td_reset(&g,7); CHECK(td_place(&g,11,1) && td_place(&g,23,2)); CHECK(td_wave(&g));
        for(i=0;i<rate*10;++i) td_tick(&g,1.0f/rate);
        CHECK(g.frames==copy.frames && g.time==copy.time);
        CHECK(g.shots==copy.shots && g.hits==copy.hits && g.kills==copy.kills);
        CHECK(g.rng==copy.rng && g.resources==copy.resources);
        CHECK(memcmp(g.enemies,copy.enemies,sizeof g.enemies)==0);
        CHECK(memcmp(g.bullets,copy.bullets,sizeof g.bullets)==0);
    }
    td_reset(&g,7); CHECK(td_wave(&g)); CHECK(td_spawn(&g,0)==0);
    td_tick(&g,7.0f);
    CHECK(g.frames==15 && fabsf(g.time-.25f)<.00001f);
    CHECK(g.pending==5 && g.shots==0);
    td_tick(&g,TD_STEP); CHECK(g.frames==16); /* No deferred hitch backlog. */
    copy=g; td_tick(&g,0); td_tick(&g,INFINITY); td_tick(&g,NAN); td_tick(&g,-1);
    CHECK(memcmp(&g,&copy,sizeof g)==0);
    /* Historical frame mode is diagnostic only; retain its negative control. */
    td_reset(&g,7); td_spawn(&g,0); td_wave(&g);
    for(i=0;i<60;++i) td_frame(&g,1.0f/60.0f);
    copy=g;
    td_reset(&g,7); td_spawn(&g,0); td_wave(&g);
    for(i=0;i<120;++i) td_frame(&g,1.0f/120.0f);
    CHECK(fabsf(g.time-copy.time)<0.00001f);
    CHECK(g.spawned==copy.spawned && g.pending==copy.pending);
    CHECK(fabsf(g.enemies[0].progress-2*copy.enemies[0].progress)<0.00001f);
    CHECK(g.frames==120 && copy.frames==60);
    copy=g; td_frame(&g,-1); td_frame(&g,NAN); CHECK(memcmp(&g,&copy,sizeof g)==0);
    td_reset(&g,7); CHECK(td_spawn(&g,0)==0);
    td_frame(&g,7.0f);
    CHECK(fabsf(g.time-7.0f)<0.00001f && g.frames==1);
    CHECK(fabsf(g.enemies[0].progress-.025f/22.0f)<0.00001f);
    td_frame(&g,0.0f);
    CHECK(fabsf(g.time-7.0f)<0.00001f && g.frames==2);
    CHECK(fabsf(g.enemies[0].progress-.05f/22.0f)<0.00001f);
    /* A source timer sees the previous sampled time; a late render cannot
     * extend the next shot by the duration of the loading hitch. */
    td_reset(&g,7); CHECK(td_spawn(&g,0)==0); CHECK(td_place(&g,11,0));
    td_frame(&g,7.0f); CHECK(g.shots==0);
    td_frame(&g,.016f); CHECK(g.shots==1);
    td_frame(&g,.016f); CHECK(g.shots==1);
    td_frame(&g,2.1f); CHECK(g.shots==1);
    td_frame(&g,0.0f); CHECK(g.shots==2);
    printf("TD_TEST PASS checks=%d deterministic_repeats=10 hash=%llu shots=%d hits=%d\n",
           checks,(unsigned long long)expected,g.shots,g.hits);
    return 0;
}
