/* Property checks run in the actual SDK-built executable. */
#include "board.h"
#include <jce/api_core.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { jce_log_write(JCE_LOG_LEVEL_ERROR,"mines.test",__FILE__,__LINE__,"FAIL: %s",#c); return 1; } } while (0)
int mines_self_test(void)
{
    static const int configs[3][3] = {{9,9,10},{16,16,40},{30,16,99}};
    MinesBoard b, copy;
    int d, seed, first, i, k, n, near[8], actual, count, safe, mine;
    CHECK(!mines_reset(&b, 31,16,99,1));
    CHECK(!mines_reset(&b, 5,5,17,1));
    for (d=0; d<3; ++d) for (seed=1; seed<=1000; ++seed) {
        CHECK(mines_reset(&b,configs[d][0],configs[d][1],configs[d][2],(uint32_t)seed));
        first=(seed*37)%(b.width*b.height);
        CHECK(mines_neighbors(&b,0,near)==3);
        CHECK(mines_neighbors(&b,1,near)==5);
        CHECK(mines_neighbors(&b,b.width+1,near)==8);
        mines_tick(&b,3); CHECK(b.elapsed==0);
        CHECK(mines_flag(&b,first)); CHECK(!mines_reveal(&b,first));
        CHECK(b.state==MINES_READY && b.flags==1);
        CHECK(mines_flag(&b,first)); CHECK(b.flags==0);
        CHECK(mines_reveal(&b,first)); CHECK(!b.cells[first].mine);
        n=mines_neighbors(&b,first,near);
        for(k=0;k<n;++k) CHECK(!b.cells[near[k]].mine);
        count=0;
        for(i=0;i<b.width*b.height;++i) {
            n=mines_neighbors(&b,i,near); actual=0;
            for(k=0;k<n;++k) actual+=b.cells[near[k]].mine?1:0;
            CHECK(actual==b.cells[i].adjacent);
            count+=b.cells[i].mine?1:0;
            CHECK(!(b.cells[i].mine && b.cells[i].revealed));
        }
        CHECK(count==b.mines); CHECK(b.revealed>1);
        safe=b.revealed; CHECK(!mines_reveal(&b,first)); CHECK(b.revealed==safe);
        mines_tick(&b,2.5); CHECK(b.elapsed==2.5);
        copy=b;
        for(i=0;i<b.width*b.height;++i) if(!b.cells[i].mine) mines_reveal(&b,i);
        CHECK(b.state==MINES_WON && b.flags==0);
        safe=b.revealed; mines_tick(&b,5); CHECK(b.elapsed==2.5);
        CHECK(!mines_flag(&b,0)); CHECK(!mines_reveal(&b,0)); CHECK(b.revealed==safe);
        b=copy; mine=-1;
        for(i=0;i<b.width*b.height;++i) if(b.cells[i].mine) { mine=i; break; }
        CHECK(mine>=0 && mines_reveal(&b,mine)); CHECK(b.state==MINES_LOST);
        CHECK(b.cells[mine].exploded); mines_tick(&b,5); CHECK(b.elapsed==2.5);
        CHECK(!mines_flag(&b,0)); CHECK(!mines_reveal(&b,0));
        CHECK(mines_reset(&b,b.width,b.height,b.mines,(uint32_t)seed+1));
        CHECK(b.flags==0 && b.revealed==0 && b.elapsed==0 && b.state==MINES_READY);
        for(i=0;i<b.width*b.height;++i) CHECK(!b.cells[i].mine && !b.cells[i].flagged);
    }
    /* A flag inside a zero region must survive flood fill until removed. */
    CHECK(mines_reset(&b,9,9,10,7)); CHECK(mines_flag(&b,1));
    CHECK(mines_reveal(&b,0)); CHECK(!b.cells[1].revealed);
    CHECK(mines_flag(&b,1)); CHECK(mines_reveal(&b,1));
    copy=b; CHECK(mines_reset(&b,9,9,10,7)); CHECK(mines_reveal(&b,0));
    for(i=0;i<81;++i) CHECK(b.cells[i].mine==copy.cells[i].mine);
    jce_log_write(JCE_LOG_LEVEL_INFO,"mines.test",__FILE__,__LINE__,
                  "MINES_TEST PASS: 3000 seeded boards, safety, counts, adjacency, flood, flags, win/loss, timer, reset");
    return 0;
}
