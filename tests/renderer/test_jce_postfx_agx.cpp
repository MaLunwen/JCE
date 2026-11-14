/* Exact composite-source CPU arithmetic controls. No device or shader compiler. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <algorithm>
#include <cmath>

struct vec2 {
    float x, y;
};
struct vec3 {
    float r, g, b;
    vec3(float v = 0.0f) : r(v), g(v), b(v) { }
    vec3(float x, float y, float z) : r(x), g(y), b(z) { }
};
static vec3 operator+(vec3 a, vec3 b) { return {a.r+b.r, a.g+b.g, a.b+b.b}; }
static vec3 operator-(vec3 a, vec3 b) { return {a.r-b.r, a.g-b.g, a.b-b.b}; }
static vec3 operator*(vec3 a, vec3 b) { return {a.r*b.r, a.g*b.g, a.b*b.b}; }
static vec3 operator/(vec3 a, vec3 b) { return {a.r/b.r, a.g/b.g, a.b/b.b}; }
static vec3 &operator+=(vec3 &a, vec3 b) { a = a+b; return a; }
static vec3 &operator-=(vec3 &a, vec3 b) { a = a-b; return a; }
static vec3 &operator*=(vec3 &a, vec3 b) { a = a*b; return a; }
static float min(float a, float b) { return std::min(a,b); }
static float max(float a, float b) { return std::max(a,b); }
static vec3 max(vec3 a, vec3 b)
{
    return {max(a.r,b.r), max(a.g,b.g), max(a.b,b.b)};
}
static vec3 clamp(vec3 v, vec3 lo, vec3 hi)
{
    return {std::clamp(v.r,lo.r,hi.r), std::clamp(v.g,lo.g,hi.g),
            std::clamp(v.b,lo.b,hi.b)};
}
static vec3 log2(vec3 v) { return {std::log2(v.r),std::log2(v.g),std::log2(v.b)}; }
static vec3 pow(vec3 v, vec3 e)
{
    return {std::pow(v.r,e.r),std::pow(v.g,e.g),std::pow(v.b,e.b)};
}
static vec3 mix(vec3 a, vec3 b, float t) { return a*(1.0f-t)+b*t; }
static vec3 vec3_splat(float v) { return vec3(v); }
struct mat3 { vec3 column[3]; };
static mat3 mtxFromCols(vec3 a, vec3 b, vec3 c) { return {{a,b,c}}; }
static vec3 mul(mat3 m, vec3 v)
{
    return m.column[0]*v.r + m.column[1]*v.g + m.column[2]*v.b;
}
struct vec4 {
    float x, y, z, w;
    vec3 rgb() const { return {x,y,z}; }
};
static vec4 u_tonemapParams = {1,2.2f,2,0};
static vec4 u_compositeFlags = {0,1,0,0};
static vec4 u_bloomParams = {0,0.3f,0,0};
static vec3 scene_input, bloom_input;
static const int s_texBloom = 1;
static vec3 sceneSample(vec2) { return scene_input; }
static vec4 texture2D(int sampler, vec2)
{
    CHECK(sampler == s_texBloom);
    return {bloom_input.r,bloom_input.g,bloom_input.b,1};
}
namespace actual {
#include JCE_TEST_POSTFX_TONEMAP_OWNER
}
namespace historical {
#include JCE_TEST_POSTFX_TONEMAP_OLD
}

static void close(vec3 a, vec3 b, float tolerance = 0.00005f)
{
    CHECK(std::isfinite(a.r));
    CHECK(std::isfinite(a.g));
    CHECK(std::isfinite(a.b));
    CHECK(std::abs(a.r-b.r) <= tolerance);
    CHECK(std::abs(a.g-b.g) <= tolerance);
    CHECK(std::abs(a.b-b.b) <= tolerance);
}

TEST_CASE("AgX middle grey has one display encoding")
{
    // The AgX neutral sigmoid places scene 0.18 near display code 0.5.
    // These independent ranges reject returning encoded values as linear.
    const vec3 linear = actual::agxTonemap(vec3(0.18f));
    CHECK(linear.r > 0.20f);
    CHECK(linear.r < 0.23f);
    CHECK(linear.g > 0.20f);
    CHECK(linear.g < 0.23f);
    CHECK(linear.b > 0.20f);
    CHECK(linear.b < 0.23f);
    scene_input = vec3(0.18f);
    u_compositeFlags = {0,1,0,0};
    u_tonemapParams = {1,2.2f,2,0};
    close(actual::combineTonemap({0.5f,0.5f}),
          {0.4966762f,0.4967449f,0.4967491f},0.0001f);
    CHECK(historical::combineTonemap({0.5f,0.5f}).r > 0.72f);
}

TEST_CASE("AgX returns bounded display-linear black colour and HDR")
{
    const vec3 inputs[] = {
        vec3(0), vec3(0.001f), vec3(0.01f), vec3(0.18f),
        vec3(1), vec3(4), vec3(16.3f), vec3(10000),
        {0.1f,0.4f,0.02f}, {3,0.1f,0.001f}, {0.002f,0.08f,6},
        {0,1,0}, {5,0,0}, {0,0,5}
    };
    for (vec3 input : inputs) {
        const vec3 result = actual::agxTonemap(input);
        const vec3 encoded_reference = historical::agxTonemap(input);
        // Historical sigmoid/outset is encoded. Decode exactly once before
        // the shared final gamma; the independent grey check anchors domain.
        close(result,pow(encoded_reference,vec3(2.2f)));
        CHECK(result.r >= 0); CHECK(result.r <= 1);
        CHECK(result.g >= 0); CHECK(result.g <= 1);
        CHECK(result.b >= 0); CHECK(result.b <= 1);
    }
    close(actual::agxTonemap(vec3(0)),vec3(0),0);
}

TEST_CASE("AgX respects project output gamma exposure and bloom order")
{
    const vec3 inputs[] = {{0.002f,0.01f,0.05f},{0.18f,0.3f,0.04f},{1,4,16}};
    for (vec3 input : inputs) {
        for (float exposure : {0.25f,0.96f,1.0f,2.0f}) {
            for (float gamma : {1.0f,2.2f,2.4f}) {
                for (float bloom : {0.0f,1.0f}) {
                    scene_input = input;
                    bloom_input = {0.03f,0.07f,0.11f};
                    u_compositeFlags = {bloom,1,0,0};
                    u_tonemapParams = {exposure,gamma,2,0};
                    // Execute the actual former combine path with gamma one
                    // to observe encoded AgX after the actual bloom/exposure.
                    u_tonemapParams.y = 1;
                    const vec3 encoded = historical::combineTonemap({0.2f,0.8f});
                    u_tonemapParams.y = gamma;
                    close(actual::combineTonemap({0.2f,0.8f}),
                          pow(pow(encoded,vec3(2.2f)),vec3(1.0f/gamma)));
                }
            }
        }
    }
}

TEST_CASE("ACES Neutral and tonemap-off are unchanged")
{
    const vec3 inputs[] = {vec3(0),{0.002f,0.01f,0.05f},
                          {0.18f,0.3f,0.04f},{1,4,16}};
    for (vec3 input : inputs) {
        for (float op : {0.0f,1.0f,2.0f}) {
            for (float gamma : {1.0f,2.2f,2.4f}) {
                for (float enabled : {0.0f,1.0f}) {
                    if (op == 2 && enabled == 1) continue;
                    for (float bloom : {0.0f,1.0f}) {
                        scene_input = input;
                        bloom_input = {0.03f,0.07f,0.11f};
                        u_compositeFlags = {bloom,enabled,0,0};
                        u_tonemapParams = {0.96f,gamma,op,0};
                        close(actual::combineTonemap({0.2f,0.8f}),
                              historical::combineTonemap({0.2f,0.8f}),0);
                    }
                }
            }
        }
    }
}
