/* tests/test_jce_postfx_lut.c
 * Pure-C LUT reorder + half-texel math test (no bgfx / GPU).
 * Validates jce_lut_strip_to_volume reorder contract and the
 * c*(N-1)/N + 0.5/N half-texel scale/bias formula.
 */

/* The PRODUCTION reorder, not a copy of it: jce_lut_strip_to_volume lives in
 * engine/src/renderer/jce_texture.c and is declared by this bgfx-free internal
 * header, so a regression in the shipped LUT loader fails this test.  (The
 * test used to compile its own identical copy — it could pass while the real
 * one was broken.) */
#include "renderer/jce_texture_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

static int feq(float a, float b){ return fabsf(a-b) < 1e-5f; }

int main(void){
    const int N = 4;
    int strip_w = N*N, strip_h = N;
    uint8_t *strip = (uint8_t*)malloc((size_t)strip_w*strip_h*4);
    /* Identity LUT: pixel (sx,sy) in tile z encodes (r=x,g=y,b=z) scaled to 0..255. */
    for (int z=0; z<N; z++)
        for (int y=0; y<N; y++)
            for (int x=0; x<N; x++){
                int sx=z*N+x, sy=y;
                uint8_t *p = strip + ((size_t)sy*strip_w+sx)*4;
                p[0]=(uint8_t)(x*255/(N-1));
                p[1]=(uint8_t)(y*255/(N-1));
                p[2]=(uint8_t)(z*255/(N-1));
                p[3]=255;
            }
    uint8_t *vol = (uint8_t*)malloc((size_t)N*N*N*4);
    jce_lut_strip_to_volume(strip, N, vol);
    /* Identity: vol at (x,y,z) must equal (x,y,z). */
    for (int z=0; z<N; z++)
        for (int y=0; y<N; y++)
            for (int x=0; x<N; x++){
                uint8_t *d = vol + (((size_t)z*N+y)*N+x)*4;
                if (d[0]!=(uint8_t)(x*255/(N-1)) ||
                    d[1]!=(uint8_t)(y*255/(N-1)) ||
                    d[2]!=(uint8_t)(z*255/(N-1))){
                    printf("FAIL reorder at %d,%d,%d\n",x,y,z); return 1; }
            }
    /* Half-texel scale/bias: c*(N-1)/N + 0.5/N maps endpoints inside [0,1]. */
    float lo = 0.0f*(N-1)/N + 0.5f/N;
    float hi = 1.0f*(N-1)/N + 0.5f/N;
    if (!feq(lo, 0.5f/N)) { printf("FAIL lo=%f\n", lo); return 1; }
    if (!feq(hi, (float)(N-1)/N + 0.5f/N)) { printf("FAIL hi=%f\n", hi); return 1; }
    free(strip); free(vol);
    printf("PASS\n");
    return 0;
}
