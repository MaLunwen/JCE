/*
 * test_jce_probe_convolution.c — the reflection probe bake's two convolution
 * steps did no work at all.
 *
 * "CONVOLVING_IRRADIANCE" advanced a progress bar and returned.  The specular
 * step counted mips and returned.  The container was written with mip_count=1,
 * so every roughness sampled mip 0.  And the .irr.ktx sidecar was written from
 * THE SAME face bytes as the specular map -- an irradiance map that is a copy
 * of the radiance it was supposed to integrate, so a probe's diffuse
 * contribution was a mirror image of its surroundings.
 *
 * The integrals were already in this layer, in jce_ibl.c, driven purely by a
 * direction; only the sampler tied them to equirectangular input.  What is
 * asserted here is that the cube sampler addresses the same cubemap the rest
 * of the engine does (a round trip against cube_dir's own convention), and
 * that each convolution actually integrates rather than copying.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include "renderer/jce_ibl_convolve.h"
#include "renderer/jce_ktx2_writer.h"

#include <jce/middleware/scene/jce_scene_probe_capture.h>

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_timer.h>
#include <jce/renderer/jce_reflection_probe_bake.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define FS 16u   /* source face size; small, because the integral is O(n^2) */

static uint8_t *alloc_faces(uint32_t fs)
{
    return (uint8_t *)calloc((size_t)fs * fs * 6u * 4u, 1u);
}

static void fill_uniform(uint8_t *f, uint32_t fs, uint8_t r, uint8_t g,
                         uint8_t b)
{
    for (size_t i = 0; i < (size_t)fs * fs * 6u; ++i) {
        f[i * 4 + 0] = r; f[i * 4 + 1] = g; f[i * 4 + 2] = b; f[i * 4 + 3] = 255;
    }
}

/* Face order is +X,-X,+Y,-Y,+Z,-Z. */
static void fill_face(uint8_t *f, uint32_t fs, int face, uint8_t r, uint8_t g,
                      uint8_t b)
{
    uint8_t *p = f + (size_t)face * fs * fs * 4u;
    for (size_t i = 0; i < (size_t)fs * fs; ++i) {
        p[i * 4 + 0] = r; p[i * 4 + 1] = g; p[i * 4 + 2] = b; p[i * 4 + 3] = 255;
    }
}

static void test_a_uniform_cubemap_integrates_to_itself(void)
{
    /* THE CALIBRATION.  Cosine-weighted irradiance of a constant environment
     * is that constant: the weights sum to 1 by construction (the PI factor
     * and the 1/PI Lambert term cancel).  If this is off, every probe in
     * every scene is off by the same factor and nothing downstream can tell. */
    uint8_t *src = alloc_faces(FS);
    uint8_t *out = alloc_faces(8u);
    TEST_ASSERT_NOT_NULL(src); TEST_ASSERT_NOT_NULL(out);
    fill_uniform(src, FS, 200, 100, 50);

    jce_ibl_convolve_cube_irradiance_rgba8(src, FS, 8u, out);

    for (size_t i = 0; i < (size_t)8 * 8 * 6; ++i) {
        TEST_ASSERT_INT_WITHIN_MESSAGE(3, 200, out[i * 4 + 0],
            "irradiance of a constant environment must be that constant");
        TEST_ASSERT_INT_WITHIN(3, 100, out[i * 4 + 1]);
        TEST_ASSERT_INT_WITHIN(3,  50, out[i * 4 + 2]);
    }
    free(src); free(out);
}

static void test_irradiance_is_not_a_copy_of_the_radiance(void)
{
    /* THE DEFECT, PINNED.  One bright face against five black ones: a copy
     * keeps the hard edge (255 on one face, 0 on the others); an integral
     * spreads it, so the bright face DIMS and the opposite face LIGHTS UP.
     * The old sidecar would pass a "is it non-zero" check and fail this. */
    uint8_t *src = alloc_faces(FS);
    uint8_t *out = alloc_faces(8u);
    TEST_ASSERT_NOT_NULL(src); TEST_ASSERT_NOT_NULL(out);
    fill_uniform(src, FS, 0, 0, 0);
    fill_face(src, FS, 2, 255, 255, 255);   /* +Y only */

    jce_ibl_convolve_cube_irradiance_rgba8(src, FS, 8u, out);

    const uint8_t *py = out + (size_t)2 * 8 * 8 * 4;   /* +Y face */
    const uint8_t *ny = out + (size_t)3 * 8 * 8 * 4;   /* -Y face */
    const int centre_py = py[(4 * 8 + 4) * 4];
    const int centre_ny = ny[(4 * 8 + 4) * 4];

    TEST_ASSERT_TRUE_MESSAGE(centre_py < 250,
        "a normal pointing INTO the bright face must be dimmer than the "
        "source -- it integrates a hemisphere, most of which is black");
    TEST_ASSERT_TRUE_MESSAGE(centre_py > 100,
        "...but still bright: the whole bright face is in that hemisphere");
    TEST_ASSERT_TRUE_MESSAGE(centre_ny < 20,
        "a normal pointing AWAY from the only light must be near black");
    TEST_ASSERT_TRUE_MESSAGE(centre_py > centre_ny * 4,
        "and the two must differ by a lot, or nothing was integrated");
}

static void test_the_cube_sampler_addresses_the_faces_it_thinks_it_does(void)
{
    /* A mirrored or rotated cube convention produces a plausible image that
     * is wrong in a way nobody sees until reflections point the wrong way --
     * this tree's most repeated class of bug.  Six single-face environments,
     * six irradiance results: the brightest output face must be the one that
     * was lit, every time.  That pins the sampler against cube_dir's own
     * convention without either being able to drift alone. */
    for (int lit = 0; lit < 6; ++lit) {
        uint8_t *src = alloc_faces(FS);
        uint8_t *out = alloc_faces(8u);
        TEST_ASSERT_NOT_NULL(src); TEST_ASSERT_NOT_NULL(out);
        fill_uniform(src, FS, 0, 0, 0);
        fill_face(src, FS, lit, 255, 255, 255);

        jce_ibl_convolve_cube_irradiance_rgba8(src, FS, 8u, out);

        int best = -1, best_v = -1;
        for (int f = 0; f < 6; ++f) {
            const uint8_t *p = out + (size_t)f * 8 * 8 * 4;
            const int centre = p[(4 * 8 + 4) * 4];
            if (centre > best_v) { best_v = centre; best = f; }
        }
        char msg[96];
        snprintf(msg, sizeof msg,
                 "lit face %d must be the brightest, got %d", lit, best);
        TEST_ASSERT_EQUAL_INT_MESSAGE(lit, best, msg);
        free(src); free(out);
    }
}

static void test_the_specular_chain_has_every_mip_and_gets_rougher(void)
{
    /* The container used to declare one mip while claiming a chain, so a
     * rough material read the mirror image.  Two things are asserted: the
     * chain is the size the writer will read, and it actually blurs -- mip 0
     * keeps the hard face edge, the last mip does not. */
    const uint32_t mips = jce_ibl_cube_mip_count(FS);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(5u, mips,
        "16 -> 8 -> 4 -> 2 -> 1 is five mips");

    const size_t bytes = jce_ibl_cube_mipchain_bytes(FS);
    size_t expect = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        uint32_t s = FS >> m; if (!s) s = 1;
        expect += (size_t)s * s * 6u * 4u;
    }
    TEST_ASSERT_EQUAL_size_t_MESSAGE(expect, bytes,
        "the chain must be exactly what a KTX reader will walk");

    uint8_t *src   = alloc_faces(FS);
    uint8_t *chain = (uint8_t *)calloc(bytes, 1u);
    TEST_ASSERT_NOT_NULL(src); TEST_ASSERT_NOT_NULL(chain);
    fill_uniform(src, FS, 0, 0, 0);
    fill_face(src, FS, 4, 255, 255, 255);   /* +Z only */

    jce_ibl_convolve_cube_specular_rgba8(src, FS, chain);

    /* MIP-MAJOR, because that is what bimg's imageWriteKtx walks: all six
     * faces of mip 0, then all six of mip 1.  The first version of this test
     * indexed face-major -- copying the same wrong assumption the code under
     * test had copied from jce_ktx2_writer.cpp's comment -- so it agreed with
     * a scrambled container.  A test that shares the assumption it is meant
     * to check is not a check. */
    const size_t face_px = (size_t)FS * FS * 4u;
    const uint8_t *m0_f4 = chain + 4u * face_px;        /* mip 0, +Z */
    const int mip0_centre = m0_f4[((FS / 2) * FS + FS / 2) * 4];
    TEST_ASSERT_TRUE_MESSAGE(mip0_centre > 200,
        "mip 0 is roughness 0: the lit face stays lit, so a probe's mirror "
        "reflection is unchanged by this");

    /* Last mip is 1x1 per face -- the whole environment averaged through the
     * roughest GGX lobe.  One lit face out of six cannot leave it white. */
    size_t off = 0;
    for (uint32_t m = 0; m < mips - 1u; ++m) {
        uint32_t s = FS >> m; if (!s) s = 1;
        off += (size_t)s * s * 6u * 4u;      /* whole mip: SIX faces */
    }
    const int last = chain[off + 4u * 4u];   /* 1x1 mip, +Z face */
    TEST_ASSERT_TRUE_MESSAGE(last < mip0_centre,
        "the roughest mip must be dimmer than the mirror mip, or the chain "
        "is six copies of the same image");

    free(src); free(chain);
}

/* ── The container round trip ─────────────────────────────────────────
 *
 * A mip chain is only worth writing if it is read back the same way, and the
 * two layouts involved are transposes of each other:
 *
 *   KTX1 file : MIP-MAJOR   for each mip { imageSize; 6 faces }
 *   bgfx cube : SIDE-MAJOR  for each face { that face's whole mip chain }
 *
 * Confusing them yields a cube that parses, uploads and renders, with rough
 * reflections showing other faces' pixels -- nothing reports it.  The only
 * check that catches it is a real write followed by a real parse, which is
 * why jce__ktx_parse_cubemap exists as a seam: it needs no GPU. */
static void test_the_chain_survives_write_then_parse(void)
{
    const uint32_t fs   = 4u;
    const uint32_t mips = jce_ibl_cube_mip_count(fs);   /* 4,2,1 => 3 */
    TEST_ASSERT_EQUAL_UINT32(3u, mips);

    /* Distinguishable per (mip, face): red = mip index, green = face index. */
    const size_t bytes = jce_ibl_cube_mipchain_bytes(fs);
    uint8_t *chain = (uint8_t *)calloc(bytes, 1u);
    TEST_ASSERT_NOT_NULL(chain);
    size_t o = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        uint32_t ms = fs >> m; if (!ms) ms = 1u;
        for (uint32_t f = 0; f < 6u; ++f)
            for (uint32_t px = 0; px < ms * ms; ++px) {
                chain[o++] = (uint8_t)(10u * (m + 1u));
                chain[o++] = (uint8_t)(10u * (f + 1u));
                chain[o++] = 0u;
                chain[o++] = 255u;
            }
    }
    TEST_ASSERT_EQUAL_size_t(bytes, o);

    const char *path = "test_probe_chain.ktx";
    TEST_ASSERT_TRUE_MESSAGE(
        jce__ktx2_write_cubemap(path, fs, mips, chain, 4u),
        "the writer must accept a multi-mip cube");

    uint64_t file_bytes = 0;
    void *file = jce_fs_host_read_all(path, &file_bytes);
    TEST_ASSERT_NOT_NULL(file);

    uint32_t got_fs = 0, got_mips = 0;
    uint8_t *side_major = NULL;
    size_t   side_size = 0;
    uint32_t got_bpp = 0;
    TEST_ASSERT_TRUE(jce__ktx_parse_cubemap(file, (size_t)file_bytes,
                                            &got_fs, &got_mips,
                                            &side_major, &side_size,
                                            &got_bpp));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(4u, got_bpp,
        "an RGBA8 cube must read back as RGBA8 -- the reader takes the format "
        "from the container now instead of assuming four bytes");
    jce_fs_buffer_free(file);
    TEST_ASSERT_EQUAL_UINT32(fs, got_fs);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(mips, got_mips,
        "the container must declare every mip, or a sampler reads only the "
        "mirror image no matter how rough the material is");

    /* Side-major: face f's chain starts at f * per_face. */
    size_t per_face = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        uint32_t ms = fs >> m; if (!ms) ms = 1u;
        per_face += (size_t)ms * ms * 4u;
    }
    TEST_ASSERT_EQUAL_size_t(per_face * 6u, side_size);

    for (uint32_t f = 0; f < 6u; ++f) {
        size_t off = f * per_face;
        for (uint32_t m = 0; m < mips; ++m) {
            uint32_t ms = fs >> m; if (!ms) ms = 1u;
            char msg[96];
            snprintf(msg, sizeof msg,
                     "face %u mip %u landed on the wrong texel", f, m);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)(10u * (m + 1u)),
                                            side_major[off + 0], msg);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)(10u * (f + 1u)),
                                            side_major[off + 1], msg);
            off += (size_t)ms * ms * 4u;
        }
    }

    jce_free(side_major);
    free(chain);
    remove(path);
}

/* ── The capture's camera basis ───────────────────────────────────────
 *
 * Six 90-degree renders only produce a correct cubemap if each camera's basis
 * matches the (face, u, v) -> direction mapping the samplers use.  Get one
 * face's up vector backwards and that face is stored upside down: every
 * reflection in the scene still looks like a reflection, and only a mirror
 * held against a recognisable object reveals it.
 *
 * So the basis is asserted against jce_ibl_cube_direction -- the one exported
 * authority for the convention -- and not against a second copy of the table
 * it was derived from. */
static void test_the_capture_basis_matches_the_cube_convention(void)
{
    for (int f = 0; f < 6; ++f) {
        jce_vec3 fwd, up;
        char msg[96];
        snprintf(msg, sizeof msg, "face %d", f);
        TEST_ASSERT_TRUE_MESSAGE(jce_probe_face_basis(f, &fwd, &up), msg);

        /* The face centre is the camera's forward. */
        float cx, cy, cz;
        jce_ibl_cube_direction(f, 0.5f, 0.5f, &cx, &cy, &cz);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, cx, fwd.x, msg);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, cy, fwd.y, msg);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, cz, fwd.z, msg);

        /* v runs DOWN the face, so increasing v must move AGAINST up.  This
         * is the assertion that catches an upside-down face. */
        float bx, by, bz;
        jce_ibl_cube_direction(f, 0.5f, 0.9f, &bx, &by, &bz);
        const float dot_up = bx * up.x + by * up.y + bz * up.z;
        snprintf(msg, sizeof msg,
                 "face %d: +v must go DOWN, dot(up)=%.3f", f, (double)dot_up);
        TEST_ASSERT_TRUE_MESSAGE(dot_up < -0.05f, msg);

        /* ...and increasing u must move along right = cross(up, forward),
         * which pins the handedness rather than just the axis. */
        const jce_vec3 right = {
            up.y * fwd.z - up.z * fwd.y,
            up.z * fwd.x - up.x * fwd.z,
            up.x * fwd.y - up.y * fwd.x,
        };
        float rx, ry, rz;
        jce_ibl_cube_direction(f, 0.9f, 0.5f, &rx, &ry, &rz);
        const float dot_r = rx * right.x + ry * right.y + rz * right.z;
        snprintf(msg, sizeof msg,
                 "face %d: +u must go RIGHT, dot(right)=%.3f", f, (double)dot_r);
        TEST_ASSERT_TRUE_MESSAGE(dot_r > 0.05f, msg);

        /* Orthonormal, or the render is sheared. */
        const float d = fwd.x * up.x + fwd.y * up.y + fwd.z * up.z;
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, 0.0f, d,
            "forward and up must be perpendicular");
    }
}

static void test_every_face_is_a_different_direction(void)
{
    /* A copy-paste slip in the table would give two faces the same forward,
     * and the cubemap would then miss a whole side of the scene while looking
     * entirely normal. */
    jce_vec3 f[6], u[6];
    for (int i = 0; i < 6; ++i)
        TEST_ASSERT_TRUE(jce_probe_face_basis(i, &f[i], &u[i]));
    for (int a = 0; a < 6; ++a)
        for (int b = a + 1; b < 6; ++b) {
            const float d = f[a].x * f[b].x + f[a].y * f[b].y + f[a].z * f[b].z;
            char msg[64];
            snprintf(msg, sizeof msg, "faces %d and %d point the same way", a, b);
            TEST_ASSERT_TRUE_MESSAGE(d < 0.99f, msg);
        }
    jce_vec3 dummy_f, dummy_u;
    TEST_ASSERT_FALSE(jce_probe_face_basis(-1, &dummy_f, &dummy_u));
    TEST_ASSERT_FALSE(jce_probe_face_basis(6, &dummy_f, &dummy_u));
}

/* ── The captured faces reach the artefact ────────────────────────────
 *
 * The GPU half of a capture (six scene renders + read-back) needs a device
 * and cannot run here.  Everything AFTER it can: the bake worker is CPU-only,
 * so a known set of faces goes in and the container comes back out, and mip 0
 * -- roughness 0, copied verbatim -- must be exactly the faces that went in.
 *
 * That is the assertion that says a probe now contains the scene rather than
 * the procedural gradient: if submit_faces ignored its argument and fell
 * through to the generator, mip 0 would be a sky-and-ground ramp instead. */
static void test_captured_faces_become_the_artefact(void)
{
    const uint32_t fs = 16u;
    const size_t face_px = (size_t)fs * fs * 4u;
    uint8_t *src = (uint8_t *)calloc(face_px * 6u, 1u);
    TEST_ASSERT_NOT_NULL(src);
    /* Per-face constant, none of which a sky gradient would produce on every
     * texel of a face. */
    for (uint32_t f = 0; f < 6u; ++f)
        for (size_t i = 0; i < (size_t)fs * fs; ++i) {
            uint8_t *px = src + f * face_px + i * 4u;
            px[0] = (uint8_t)(17u * (f + 1u));
            px[1] = (uint8_t)(3u  * (f + 1u));
            px[2] = (uint8_t)(200u - 11u * f);
            px[3] = 255u;
        }

    JceReflectionProbeBakeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.cubemap_size       = fs;
    desc.specular_mip_count = 5u;
    desc.include_skybox     = true;
    desc.output_path_ktx2   = "test_probe_captured.ktx";

    JceReflectionProbeBakeHandle h =
        jce_reflection_probe_bake_submit_faces(&desc, src);
    TEST_ASSERT_TRUE_MESSAGE(h != 0u, "submit_faces must accept the capture");

    /* Drive to a terminal state, bounded by wall time -- an iteration count is
     * an instrument tuned to whatever the work happened to cost that day. */
    JceReflectionProbeBakeProgress prog;
    JceBakeStatus last = JCE_BAKE_STATUS_IDLE;
    const uint64_t t0 = jce_time_perf_counter();
    for (int i = 0; ; ++i) {
        if ((i & 1023) == 0 &&
            jce_time_perf_to_ms(t0, jce_time_perf_counter()) > 60000.0) break;
        memset(&prog, 0, sizeof prog);
        const bool active = jce_reflection_probe_bake_poll(h, &prog);
        if (prog.status != JCE_BAKE_STATUS_IDLE) last = prog.status;
        if (!active) break;
        if (last == JCE_BAKE_STATUS_DONE || last == JCE_BAKE_STATUS_FAILED ||
            last == JCE_BAKE_STATUS_CANCELLED) break;
        JceAsyncPumpBudget budget;
        jce_async_pump_budget_init(&budget);
        (void)jce_async_default_pump(&budget);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_BAKE_STATUS_DONE, last,
                                  "the bake must finish");

    uint64_t bytes = 0;
    void *file = jce_fs_host_read_all("test_probe_captured.ktx", &bytes);
    TEST_ASSERT_NOT_NULL_MESSAGE(file, "the artefact must exist");

    uint32_t got_fs = 0, got_mips = 0;
    uint8_t *side = NULL; size_t side_size = 0;
    uint32_t got_bpp2 = 0;
    TEST_ASSERT_TRUE(jce__ktx_parse_cubemap(file, (size_t)bytes, &got_fs,
                                            &got_mips, &side, &side_size,
                                            &got_bpp2));
    TEST_ASSERT_EQUAL_UINT32(4u, got_bpp2);
    jce_fs_buffer_free(file);
    TEST_ASSERT_EQUAL_UINT32(fs, got_fs);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(jce_ibl_cube_mip_count(fs), got_mips,
        "the captured bake must still write the whole roughness chain");

    /* side-major: face f's chain starts at f * per_face, mip 0 first. */
    size_t per_face = 0;
    for (uint32_t m = 0; m < got_mips; ++m) {
        uint32_t ms = fs >> m; if (!ms) ms = 1u;
        per_face += (size_t)ms * ms * 4u;
    }
    for (uint32_t f = 0; f < 6u; ++f) {
        const uint8_t *got = side + (size_t)f * per_face;
        char msg[96];
        snprintf(msg, sizeof msg,
                 "face %u mip 0 must be the CAPTURED pixels, not a gradient", f);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)(17u * (f + 1u)), got[0], msg);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)(3u * (f + 1u)),  got[1], msg);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)(200u - 11u * f), got[2], msg);
    }

    jce_free(side);
    free(src);
    remove("test_probe_captured.ktx");
    remove("test_probe_captured.irr.ktx");
}

/* HDR: the values above 1.0 are the whole point.
 *
 * A reflection probe exists so that a mirror-like surface shows what is
 * around it.  What is around it, in any scene with a sun or a lamp, contains
 * values far above 1.0 -- and an RGBA8 container cannot hold one of them.
 * Clamped, a sun reflects at exactly the brightness of a sheet of white
 * paper, which is the one thing the reflection was for.
 *
 * The two cases below are each other's control: the SAME radiance goes into
 * both containers, and the assertion is that one keeps it and the other
 * provably cannot.  Without the LDR half, "the HDR path returned 4.0" would
 * not distinguish a working feature from a test that measured nothing.
 *
 * Half-float encode/decode is written out here rather than borrowed, because
 * borrowing jce_ibl.c's private pair would let a bug in it agree with
 * itself. */
static uint16_t t_f32_to_f16(float f)
{
    uint32_t b; memcpy(&b, &f, 4);
    const uint32_t sign = (b >> 16) & 0x8000u;
    int32_t  exp  = (int32_t)((b >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = b & 0x7FFFFFu;
    if (exp <= 0)  return (uint16_t)sign;                 /* underflow */
    if (exp >= 31) return (uint16_t)(sign | 0x7C00u);     /* inf */
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
}

static float t_f16_to_f32(uint16_t h)
{
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    const uint32_t exp  = (uint32_t)(h >> 10) & 0x1Fu;
    const uint32_t mant = (uint32_t)(h & 0x3FFu);
    uint32_t b;
    if (exp == 0)       b = sign;                         /* zero / denormal */
    else if (exp == 31) b = sign | 0x7F800000u | (mant << 13);
    else                b = sign | ((exp + 127u - 15u) << 23) | (mant << 13);
    float f; memcpy(&f, &b, 4); return f;
}

#define HDR_RADIANCE 4.0f   /* four times white paper: a lamp, not a wall */

static void test_hdr_keeps_the_radiance_that_ldr_provably_cannot(void)
{
    const uint32_t out_fs = 8u;
    const size_t   texels = (size_t)FS * FS * 6u;
    const size_t   out_tx = (size_t)out_fs * out_fs * 6u;

    /* ONE radiance, two containers. */
    uint16_t *src16 = (uint16_t *)calloc(texels * 4u, sizeof(uint16_t));
    uint8_t  *src8  = alloc_faces(FS);
    TEST_ASSERT_NOT_NULL(src16);
    TEST_ASSERT_NOT_NULL(src8);
    const uint16_t h = t_f32_to_f16(HDR_RADIANCE);
    for (size_t i = 0; i < texels; ++i) {
        src16[i * 4 + 0] = h; src16[i * 4 + 1] = h; src16[i * 4 + 2] = h;
        src16[i * 4 + 3] = t_f32_to_f16(1.0f);
    }
    /* The same 4.0 encoded into a byte: it saturates.  That is not a defect
     * in the encoder, it is the reason this path exists. */
    fill_uniform(src8, FS, 255u, 255u, 255u);

    uint16_t *out16 = (uint16_t *)calloc(out_tx * 4u, sizeof(uint16_t));
    uint8_t  *out8  = (uint8_t  *)calloc(out_tx * 4u, 1u);
    TEST_ASSERT_NOT_NULL(out16);
    TEST_ASSERT_NOT_NULL(out8);
    jce_ibl_convolve_cube_irradiance_rgba16f(src16, FS, out_fs, out16);
    jce_ibl_convolve_cube_irradiance_rgba8(src8, FS, out_fs, out8);

    /* A uniform cubemap integrates to itself, so both answers should BE the
     * radiance they were given -- and only one of them can be. */
    float lo16 = 1e9f, hi8 = 0.0f;
    for (size_t i = 0; i < out_tx; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float v16 = t_f16_to_f32(out16[i * 4 + (size_t)c]);
            const float v8  = (float)out8[i * 4 + (size_t)c] / 255.0f;
            if (v16 < lo16) lo16 = v16;
            if (v8  > hi8)  hi8  = v8;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(lo16 > 3.5f,
        "the HDR convolution must carry the radiance it was given; a value "
        "at or below 1.0 here means it clamped exactly like the 8-bit path "
        "and the format change bought nothing");
    TEST_ASSERT_TRUE_MESSAGE(hi8 <= 1.0f,
        "the negative control: an 8-bit container cannot exceed 1.0, which "
        "is why a sun in an LDR probe reflects like white paper");

    free(out8); free(out16); free(src8); free(src16);
}

static void test_an_hdr_chain_survives_write_then_parse(void)
{
    const uint32_t fs   = 4u;
    const uint32_t mips = jce_ibl_cube_mip_count(fs);
    /* mipchain_bytes counts RGBA8 texels, which is the ELEMENT count here. */
    const size_t   elems = jce_ibl_cube_mipchain_bytes(fs);
    uint16_t *chain = (uint16_t *)calloc(elems, sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(chain);

    /* Every texel above 1.0, and distinguishable per (mip, face): a container
     * that halved the stride would read another mip's texels. */
    size_t o = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        uint32_t ms = fs >> m; if (!ms) ms = 1u;
        for (uint32_t f = 0; f < 6u; ++f)
            for (uint32_t px = 0; px < ms * ms; ++px) {
                chain[o++] = t_f32_to_f16(2.0f + (float)m);
                chain[o++] = t_f32_to_f16(8.0f + (float)f);
                chain[o++] = t_f32_to_f16(0.0f);
                chain[o++] = t_f32_to_f16(1.0f);
            }
    }
    TEST_ASSERT_EQUAL_size_t(elems, o);

    const char *path = "test_probe_chain_hdr.ktx";
    TEST_ASSERT_TRUE_MESSAGE(
        jce__ktx2_write_cubemap(path, fs, mips, (const uint8_t *)chain, 8u),
        "the writer must accept an RGBA16F cube");

    uint64_t file_bytes = 0;
    void *file = jce_fs_host_read_all(path, &file_bytes);
    TEST_ASSERT_NOT_NULL(file);

    uint32_t got_fs = 0, got_mips = 0, got_bpp = 0;
    uint8_t *side_major = NULL;
    size_t   side_size  = 0;
    TEST_ASSERT_TRUE(jce__ktx_parse_cubemap(file, (size_t)file_bytes,
                                            &got_fs, &got_mips,
                                            &side_major, &side_size,
                                            &got_bpp));
    jce_fs_buffer_free(file);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(8u, got_bpp,
        "the reader must take the format from the container's own "
        "glType/glInternalFormat; assuming 4 reads an RGBA16F cube as a "
        "differently-shaped RGBA8 one and uploads it without complaining");
    TEST_ASSERT_EQUAL_UINT32(fs, got_fs);
    TEST_ASSERT_EQUAL_UINT32(mips, got_mips);

    size_t per_face = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        uint32_t ms = fs >> m; if (!ms) ms = 1u;
        per_face += (size_t)ms * ms * 8u;
    }
    TEST_ASSERT_EQUAL_size_t(per_face * 6u, side_size);

    for (uint32_t f = 0; f < 6u; ++f) {
        size_t off = f * per_face;
        for (uint32_t m = 0; m < mips; ++m) {
            uint32_t ms = fs >> m; if (!ms) ms = 1u;
            uint16_t px[2];
            memcpy(px, side_major + off, sizeof px);
            char msg[96];
            snprintf(msg, sizeof msg,
                     "face %u mip %u landed on the wrong texel", f, m);
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(2.0f + (float)m,
                                            t_f16_to_f32(px[0]), msg);
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(8.0f + (float)f,
                                            t_f16_to_f32(px[1]), msg);
            off += (size_t)ms * ms * 8u;
        }
    }

    jce_free(side_major);
    free(chain);
    remove(path);
}

/* The flag has to SURVIVE THE BAKE, which is a different claim.
 *
 * `hdr` travels from JceReflectionProbeBakeDesc through four allocations
 * sized by bytes-per-texel, two convolution branches and two writer calls.
 * The cases above would pass unchanged if the bake dropped it at any one of
 * them and wrote an 8-bit file, so this one submits captured faces and reads
 * back the artefact the bake actually produced. */
static void test_an_hdr_bake_writes_an_hdr_artefact(void)
{
    const uint32_t fs = 16u;
    const size_t   face_tx = (size_t)fs * fs;
    uint16_t *src = (uint16_t *)calloc(face_tx * 6u * 4u, sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(src);

    /* Per-face constant, every one of them ABOVE 1.0 -- so a chain that
     * quietly went through 8 bits would come back saturated and equal, and
     * the per-face assertion below could not pass by accident. */
    for (uint32_t f = 0; f < 6u; ++f)
        for (size_t i = 0; i < face_tx; ++i) {
            uint16_t *px = src + (f * face_tx + i) * 4u;
            px[0] = t_f32_to_f16(2.0f + (float)f);
            px[1] = t_f32_to_f16(9.0f - (float)f);
            px[2] = t_f32_to_f16(1.5f);
            px[3] = t_f32_to_f16(1.0f);
        }

    JceReflectionProbeBakeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.cubemap_size       = fs;
    desc.specular_mip_count = 5u;
    desc.include_skybox     = true;
    desc.hdr                = true;          /* THE FLAG */
    desc.output_path_ktx2   = "test_probe_hdr_bake.ktx";

    JceReflectionProbeBakeHandle h =
        jce_reflection_probe_bake_submit_faces(&desc, (const uint8_t *)src);
    TEST_ASSERT_TRUE_MESSAGE(h != 0u, "submit_faces must accept the capture");

    JceReflectionProbeBakeProgress prog;
    JceBakeStatus last = JCE_BAKE_STATUS_IDLE;
    const uint64_t t0 = jce_time_perf_counter();
    for (int i = 0; ; ++i) {
        if ((i & 1023) == 0 &&
            jce_time_perf_to_ms(t0, jce_time_perf_counter()) > 60000.0) break;
        memset(&prog, 0, sizeof prog);
        const bool active = jce_reflection_probe_bake_poll(h, &prog);
        if (prog.status != JCE_BAKE_STATUS_IDLE) last = prog.status;
        if (!active) break;
        if (last == JCE_BAKE_STATUS_DONE || last == JCE_BAKE_STATUS_FAILED ||
            last == JCE_BAKE_STATUS_CANCELLED) break;
        JceAsyncPumpBudget budget;
        jce_async_pump_budget_init(&budget);
        (void)jce_async_default_pump(&budget);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_BAKE_STATUS_DONE, last,
                                  "the HDR bake must finish");

    uint64_t bytes = 0;
    void *file = jce_fs_host_read_all("test_probe_hdr_bake.ktx", &bytes);
    TEST_ASSERT_NOT_NULL_MESSAGE(file, "the artefact must exist");

    uint32_t got_fs = 0, got_mips = 0, got_bpp = 0;
    uint8_t *side = NULL; size_t side_size = 0;
    TEST_ASSERT_TRUE(jce__ktx_parse_cubemap(file, (size_t)bytes, &got_fs,
                                            &got_mips, &side, &side_size,
                                            &got_bpp));
    jce_fs_buffer_free(file);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(8u, got_bpp,
        "the bake must WRITE the format it was asked for; 4 here means the "
        "flag was dropped somewhere between the desc and the container and "
        "every value above 1.0 is already gone");
    TEST_ASSERT_EQUAL_UINT32(fs, got_fs);
    TEST_ASSERT_EQUAL_UINT32(jce_ibl_cube_mip_count(fs), got_mips);

    size_t per_face = 0;
    for (uint32_t m = 0; m < got_mips; ++m) {
        uint32_t ms = fs >> m; if (!ms) ms = 1u;
        per_face += (size_t)ms * ms * 8u;
    }
    TEST_ASSERT_EQUAL_size_t(per_face * 6u, side_size);

    for (uint32_t f = 0; f < 6u; ++f) {
        uint16_t px[3];
        memcpy(px, side + (size_t)f * per_face, sizeof px);
        char msg[96];
        snprintf(msg, sizeof msg,
                 "face %u mip 0 must be the CAPTURED radiance, unclamped", f);
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(2.0f + (float)f,
                                        t_f16_to_f32(px[0]), msg);
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(9.0f - (float)f,
                                        t_f16_to_f32(px[1]), msg);
    }

    jce_free(side);
    free(src);
    remove("test_probe_hdr_bake.ktx");
    remove("test_probe_hdr_bake.irr.ktx");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_uniform_cubemap_integrates_to_itself);
    RUN_TEST(test_irradiance_is_not_a_copy_of_the_radiance);
    RUN_TEST(test_the_cube_sampler_addresses_the_faces_it_thinks_it_does);
    RUN_TEST(test_the_specular_chain_has_every_mip_and_gets_rougher);
    RUN_TEST(test_the_chain_survives_write_then_parse);
    RUN_TEST(test_the_capture_basis_matches_the_cube_convention);
    RUN_TEST(test_every_face_is_a_different_direction);
    RUN_TEST(test_captured_faces_become_the_artefact);
    RUN_TEST(test_hdr_keeps_the_radiance_that_ldr_provably_cannot);
    RUN_TEST(test_an_hdr_chain_survives_write_then_parse);
    RUN_TEST(test_an_hdr_bake_writes_an_hdr_artefact);
    return UNITY_END();
}
