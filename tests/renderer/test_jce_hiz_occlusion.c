/* test_jce_hiz_occlusion.c
 *
 * Hi-Z occlusion-test math (large-world #5): the scalar core of the GPU cull
 * shader's per-instance occlusion test, verified on the CPU (self-contained,
 * no engine link — same pattern as test_jce_aerial_fog_math).  The cull shader
 * (cs_cull_frustum / cs_cull_compact, via hiz_occlusion.sh) mirrors this math.
 *
 * Hi-Z is a MAX-depth pyramid of the scene depth buffer (depth: 0=near, 1=far).
 * An instance is OCCLUDED iff the nearest point of its screen-space AABB is
 * farther than the farthest recorded occluder surface over its footprint, i.e.
 *     aabb_min_depth > max(HiZ over footprint)
 * Conservative: anything off-screen, straddling the near plane, or larger than
 * the pyramid's coarsest level is treated as VISIBLE (never wrongly culled).
 */

#include "unity.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Hi-Z pyramid (max-depth), built from a level-0 grid by 2x2 max-reduce ── */
#define HIZ_L0   16            /* level 0 = 16x16 */
#define HIZ_MIPS 5             /* 16,8,4,2,1 */
typedef struct {
    float lvl[HIZ_MIPS][HIZ_L0 * HIZ_L0];  /* only [0..size*size) used per mip */
    int   dim[HIZ_MIPS];
    int   mips;
} HiZ;

static void hiz_build(HiZ *h, const float *l0)
{
    h->mips = HIZ_MIPS;
    h->dim[0] = HIZ_L0;
    memcpy(h->lvl[0], l0, sizeof(float) * HIZ_L0 * HIZ_L0);
    for (int m = 1; m < HIZ_MIPS; ++m) {
        int d  = HIZ_L0 >> m;
        int pd = HIZ_L0 >> (m - 1);
        h->dim[m] = d;
        for (int y = 0; y < d; ++y)
            for (int x = 0; x < d; ++x) {
                float a = h->lvl[m-1][(2*y  )*pd + (2*x  )];
                float b = h->lvl[m-1][(2*y  )*pd + (2*x+1)];
                float c = h->lvl[m-1][(2*y+1)*pd + (2*x  )];
                float e = h->lvl[m-1][(2*y+1)*pd + (2*x+1)];
                float mx = a; if (b>mx)mx=b; if (c>mx)mx=c; if (e>mx)mx=e;
                h->lvl[m][y*d + x] = mx;
            }
    }
}

/* mat4 (column-major, bgfx mul(mat,vec) convention) * vec4 */
static void m4v4(const float m[16], const float v[4], float out[4])
{
    for (int i = 0; i < 4; ++i)
        out[i] = m[0*4+i]*v[0] + m[1*4+i]*v[1] + m[2*4+i]*v[2] + m[3*4+i]*v[3];
}

/* THE OCCLUSION TEST (mirror of hiz_occlusion.sh). Returns true = occluded. */
static int hiz_aabb_occluded(const HiZ *h, const float vp[16],
                             const float center[3], const float extent[3])
{
    float mn_uvx=1e9f, mn_uvy=1e9f, mx_uvx=-1e9f, mx_uvy=-1e9f, mn_z=1e9f;
    for (int c = 0; c < 8; ++c) {
        float p[4] = {
            center[0] + ((c&1)?extent[0]:-extent[0]),
            center[1] + ((c&2)?extent[1]:-extent[1]),
            center[2] + ((c&4)?extent[2]:-extent[2]), 1.0f };
        float clip[4]; m4v4(vp, p, clip);
        if (clip[3] <= 1e-6f) return 0;          /* straddles near plane -> visible */
        float ndcx = clip[0]/clip[3], ndcy = clip[1]/clip[3], ndcz = clip[2]/clip[3];
        float ux = ndcx*0.5f + 0.5f, uy = ndcy*0.5f + 0.5f;
        if (ux<mn_uvx)mn_uvx=ux; if (ux>mx_uvx)mx_uvx=ux;
        if (uy<mn_uvy)mn_uvy=uy; if (uy>mx_uvy)mx_uvy=uy;
        if (ndcz<mn_z)mn_z=ndcz;
    }
    /* fully off-screen -> visible (frustum cull already handles in-pipeline) */
    if (mx_uvx<0.0f||mn_uvx>1.0f||mx_uvy<0.0f||mn_uvy>1.0f) return 0;
    if (mn_z < 0.0f) return 0;                    /* in front of near -> visible */

    /* pick the mip where the footprint spans <= ~2 texels (level-0 px size). */
    float wpx = (mx_uvx-mn_uvx) * (float)h->dim[0];
    float hpx = (mx_uvy-mn_uvy) * (float)h->dim[0];
    float span = wpx>hpx?wpx:hpx;
    int mip = (int)ceilf(log2f(span>1.0f?span:1.0f));
    if (mip < 0) mip = 0;
    if (mip >= h->mips) return 0;                 /* bigger than pyramid -> visible */

    /* sample the (clamped) 2x2 footprint at that mip, take the max occluder z */
    int d = h->dim[mip];
    int x0 = (int)(mn_uvx*d), x1 = (int)(mx_uvx*d);
    int y0 = (int)(mn_uvy*d), y1 = (int)(mx_uvy*d);
    if (x0<0)x0=0; if (y0<0)y0=0; if (x1>d-1)x1=d-1; if (y1>d-1)y1=d-1;
    float occ = 0.0f;
    for (int y=y0; y<=y1; ++y) for (int x=x0; x<=x1; ++x) {
        float v = h->lvl[mip][y*d+x]; if (v>occ) occ=v;
    }
    return mn_z > occ;                            /* nearest pt behind occluder */
}

/* identity viewproj: clip = (x,y,z,1) so UV = xy*0.5+0.5, depth = z. */
static const float VP_ID[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

static void fill(float *g, int n, float v){ for(int i=0;i<n;++i) g[i]=v; }

static void test_box_behind_occluder_is_occluded(void)
{
    float l0[HIZ_L0*HIZ_L0]; fill(l0, HIZ_L0*HIZ_L0, 0.5f);  /* flat wall at z=0.5 */
    HiZ h; hiz_build(&h, l0);
    /* small box at screen center, depth 0.75..0.85 (entirely behind 0.5) */
    float c[3]={0,0,0.8f}, e[3]={0.05f,0.05f,0.05f};
    TEST_ASSERT_TRUE(hiz_aabb_occluded(&h, VP_ID, c, e));
}

static void test_box_in_front_is_visible(void)
{
    float l0[HIZ_L0*HIZ_L0]; fill(l0, HIZ_L0*HIZ_L0, 0.5f);
    HiZ h; hiz_build(&h, l0);
    float c[3]={0,0,0.2f}, e[3]={0.05f,0.05f,0.05f};   /* nearest 0.15 < 0.5 */
    TEST_ASSERT_FALSE(hiz_aabb_occluded(&h, VP_ID, c, e));
}

static void test_offscreen_is_visible(void)
{
    float l0[HIZ_L0*HIZ_L0]; fill(l0, HIZ_L0*HIZ_L0, 0.5f);
    HiZ h; hiz_build(&h, l0);
    float c[3]={5.0f,0,0.8f}, e[3]={0.05f,0.05f,0.05f}; /* UV x ~3 -> off-screen */
    TEST_ASSERT_FALSE(hiz_aabb_occluded(&h, VP_ID, c, e));
}

static void test_near_plane_straddle_is_visible(void)
{
    float l0[HIZ_L0*HIZ_L0]; fill(l0, HIZ_L0*HIZ_L0, 0.5f);
    HiZ h; hiz_build(&h, l0);
    /* viewproj that yields w<=0 for the box (simulate behind-eye): negate w row */
    float vp[16]; memcpy(vp, VP_ID, sizeof vp); vp[3*4+3] = -1.0f; /* w = -1 */
    float c[3]={0,0,0.8f}, e[3]={0.05f,0.05f,0.05f};
    TEST_ASSERT_FALSE(hiz_aabb_occluded(&h, vp, c, e));
}

static void test_conservative_mixed_footprint_visible(void)
{
    /* far wall (0.9) with a near strip (0.1) down the middle. A box behind the
     * wall (nearest 0.6) but whose footprint also covers the near strip must
     * stay VISIBLE: the conservative MAX over the footprint is 0.9, and
     * 0.6 > 0.9 is false. (Never cull something partly seen through a near gap.) */
    float l0[HIZ_L0*HIZ_L0]; fill(l0, HIZ_L0*HIZ_L0, 0.9f);
    for (int y=0;y<HIZ_L0;++y){ l0[y*HIZ_L0+7]=0.1f; l0[y*HIZ_L0+8]=0.1f; }
    HiZ h; hiz_build(&h, l0);
    float c[3]={0,0,0.7f}, e[3]={0.20f,0.05f,0.10f};   /* nearest 0.6, wide footprint */
    TEST_ASSERT_FALSE(hiz_aabb_occluded(&h, VP_ID, c, e));
}

static void test_huge_object_is_visible(void)
{
    /* an object whose screen footprint is larger than the coarsest mip cannot be
     * tested conservatively -> treated as visible (never wrongly culled). */
    float l0[HIZ_L0*HIZ_L0]; fill(l0, HIZ_L0*HIZ_L0, 0.5f);
    HiZ h; hiz_build(&h, l0);
    float c[3]={0,0,0.8f}, e[3]={2.0f,2.0f,0.05f};     /* spans well beyond screen */
    TEST_ASSERT_FALSE(hiz_aabb_occluded(&h, VP_ID, c, e));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_box_behind_occluder_is_occluded);
    RUN_TEST(test_box_in_front_is_visible);
    RUN_TEST(test_offscreen_is_visible);
    RUN_TEST(test_near_plane_straddle_is_visible);
    RUN_TEST(test_conservative_mixed_footprint_visible);
    RUN_TEST(test_huge_object_is_visible);
    return UNITY_END();
}
