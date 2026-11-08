/* Complete actual fog march through gl_FragColor, with typed CPU ports.
 * No shader compiler, renderer, native window, texture or GPU is initialized. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace {
struct vec2 {
    float x, y;
    vec2(float x_, float y_) : x(x_), y(y_) {}
};
struct vec3 {
    float x, y, z;
    vec3(float x_ = 0, float y_ = 0, float z_ = 0) : x(x_), y(y_), z(z_) {}
    vec3 &operator+=(vec3 other)
    { x += other.x; y += other.y; z += other.z; return *this; }
};
struct vec4 {
    float x, y, z, w, a;
    vec4(float x_ = 0, float y_ = 0, float z_ = 0, float w_ = 0)
        : x(x_), y(y_), z(z_), w(w_), a(w_) {}
    vec4(vec3 rgb_, float alpha) : vec4(rgb_.x, rgb_.y, rgb_.z, alpha) {}
    vec2 xy() const { return {x, y}; }
    vec3 rgb() const { return {x, y, z}; }
};
struct SunSample { vec3 position; float view_z = 0, visibility = 0; };
struct Counts {
    unsigned exponentials = 0, maxima = 0, vector_scales = 0;
    unsigned samples = 0, sunlight = 0, noise = 0, cloud = 0;
    std::array<vec3, 64> positions{};
    std::array<SunSample, 64> sun_samples{};
};
Counts counts;
std::array<float, 8> visibility;

vec3 operator+(vec3 left, vec3 right)
{
    const vec3 result(left.x + right.x, left.y + right.y, left.z + right.z);
    if (counts.samples < counts.positions.size()) counts.positions[counts.samples] = result;
    ++counts.samples;
    return result;
}
vec3 operator*(vec3 value, float scale)
{
    ++counts.vector_scales;
    return {value.x * scale, value.y * scale, value.z * scale};
}
vec3 operator*(vec3 left, vec3 right)
{ return {left.x * right.x, left.y * right.y, left.z * right.z}; }
float max(float left, float right)
{ ++counts.maxima; return std::max(left, right); }
float exp(float value)
{ ++counts.exponentials; return std::exp(value); }
float fract(float value) { return value - std::floor(value); }
float dot(vec2 left, vec2 right) { return left.x * right.x + left.y * right.y; }
float volfog_sunlight(vec3 position, float view_z)
{
    const unsigned index = counts.sunlight++;
    const float result = visibility[index % visibility.size()];
    if (index < counts.sun_samples.size()) counts.sun_samples[index] = {position, view_z, result};
    return result;
}

/* Typed inputs are the actual march's upstream scalar/vector dependencies.
 * Sun visibility is a counted port; cascade sampling itself has its own owner
 * fixture. No medium integration or result formula is implemented here. */
struct Inputs {
    float density = 0.00013f, scattering = 1, nearp = 0.1f, scene_z = 800;
    float step_n = 32, height_falloff = 0, height_origin = 0;
    float view_z_scale = 1, phase = 0.17f, sun_on = 4;
    bool noise_on = false;
    float noise_strength = 1.0f, fog_start = 0.0f;
    float segment_length() const
    {
        return (max(scene_z - nearp, 0.0f) / view_z_scale - fog_start) / step_n;
    }
    bool cloud_shadow_on = false;
    float cloud_transmittance = 1.0f;
    float volfog_cloud_transmittance(vec3) { ++counts.cloud; return cloud_transmittance; }
    float noise_multiplier = 1.0f;
    float volfog_density_noise(vec3, float) { ++counts.noise; return noise_multiplier; }
    vec3 cam_ws{18.25f, 10.69f, 27.32f}, ray_dir{-0.5f, -0.3f, -0.8124038f};
    vec4 u_volfog_color{0.17f, 0.28f, 0.43f, 0.05f};
    vec4 u_volfog_sun_color{0.14f, 0.39f, 0.54f, 4};
    vec4 gl_FragCoord{153.5f, 417.5f, 0, 0}, gl_FragColor;
};
struct Current : Inputs {
    vec4 run()
    {
        const float dt = segment_length();
#include JCE_TEST_VFOG_MARCH_CURRENT
        return gl_FragColor;
    }
};
struct Reference : Inputs {
    vec4 run()
    {
        const float dt = segment_length();
#include JCE_TEST_VFOG_MARCH_REFERENCE
        return gl_FragColor;
    }
};
struct Result { vec4 color; Counts operations; };
Result evaluate(bool optimized, const Inputs &input, bool changing_visibility = false)
{
    counts = {};
    visibility = changing_visibility ? std::array<float, 8>{0, 1, 0.25f, 0.75f, 1, 0, 0.5f, 1}
                                    : std::array<float, 8>{1, 1, 1, 1, 1, 1, 1, 1};
    vec4 output;
    if (optimized) {
        Current current;
        static_cast<Inputs &>(current) = input;
        output = current.run();
    } else {
        Reference reference;
        static_cast<Inputs &>(reference) = input;
        output = reference.run();
    }
    return {output, counts};
}
void equal_vector(vec3 actual, vec3 expected)
{ CHECK(actual.x == expected.x); CHECK(actual.y == expected.y); CHECK(actual.z == expected.z); }
void equal_results(const Result &actual, const Result &expected)
{
    CHECK(std::isfinite(actual.color.x)); CHECK(std::isfinite(actual.color.y));
    CHECK(std::isfinite(actual.color.z)); CHECK(std::isfinite(actual.color.w));
    CHECK(actual.color.x == expected.color.x); CHECK(actual.color.y == expected.color.y);
    CHECK(actual.color.z == expected.color.z); CHECK(actual.color.w == expected.color.w);
    REQUIRE(actual.operations.samples == expected.operations.samples);
    REQUIRE(actual.operations.samples <= actual.operations.positions.size());
    REQUIRE(actual.operations.sunlight == expected.operations.sunlight);
    REQUIRE(actual.operations.sunlight <= actual.operations.sun_samples.size());
    for (unsigned i = 0; i < actual.operations.samples; ++i)
        equal_vector(actual.operations.positions[i], expected.operations.positions[i]);
    for (unsigned i = 0; i < actual.operations.sunlight; ++i) {
        equal_vector(actual.operations.sun_samples[i].position, expected.operations.sun_samples[i].position);
        CHECK(actual.operations.sun_samples[i].view_z == expected.operations.sun_samples[i].view_z);
        CHECK(actual.operations.sun_samples[i].visibility == expected.operations.sun_samples[i].visibility);
    }
}
unsigned requested_samples(float steps) { return static_cast<unsigned>(std::ceil(steps)); }
}

TEST_CASE("zero falloff keeps final integration and every IGN sample while removing repeated exp work")
{
    for (float steps : {1.0f, 1.25f, 2.0f, 7.5f, 16.0f, 32.0f, 63.5f, 64.0f})
        for (float density : {0.0f, 0.00013f, 0.02f})
            for (float extent : {0.1f, 1.0f, 47.0f, 300.0f, 800.0f}) {
                Inputs in; in.step_n = steps; in.density = density; in.scene_z = extent;
                const auto old = evaluate(false, in), now = evaluate(true, in);
                equal_results(now, old);
                REQUIRE(now.operations.samples > 0);
                CHECK(old.operations.exponentials == 2 * old.operations.samples);
                CHECK(now.operations.exponentials == 1);
                CHECK(now.operations.maxima == 1);
                CHECK(now.operations.noise == 0);
                CHECK(old.operations.maxima == 1 + old.operations.samples);
            }
}
TEST_CASE("positive falloff retains the actual per-sample path and operation counts")
{
    for (float falloff : {0.000001f, 0.05f, 1.0f})
        for (float origin : {-12.0f, 0.0f, 25.0f})
            for (float steps : {1.0f, 7.5f, 32.0f, 64.0f}) {
                Inputs in; in.height_falloff = falloff; in.height_origin = origin; in.step_n = steps;
                const auto old = evaluate(false, in, true), now = evaluate(true, in, true);
                equal_results(now, old);
                CHECK(now.operations.exponentials == old.operations.exponentials);
                CHECK(now.operations.exponentials == 2 * now.operations.samples);
                CHECK(now.operations.maxima == old.operations.maxima);
                CHECK(now.operations.vector_scales == old.operations.vector_scales);
            }
}
TEST_CASE("no sun preserves bulk scattering and all authored sample counts")
{
    for (float sun : {0.0f, 0.5f}) for (float steps : {1.0f, 2.5f, 32.0f, 64.0f}) {
        Inputs in; in.sun_on = sun; in.step_n = steps;
        const auto old = evaluate(false, in), now = evaluate(true, in);
        equal_results(now, old);
        CHECK(now.operations.sunlight == 0);
        CHECK(now.operations.samples == requested_samples(steps));
        CHECK(now.operations.exponentials == 1);
        CHECK(old.operations.vector_scales - now.operations.vector_scales ==
              3 * (now.operations.samples - 1));
        CHECK(now.color.x > 0); CHECK(now.color.w < 1);
    }
}
TEST_CASE("varying CSM visibility keeps every sunlight call position and final shadowed scatter")
{
    for (float falloff : {0.0f, 0.05f}) for (float steps : {8.0f, 32.0f, 64.0f}) {
        Inputs in; in.height_falloff = falloff; in.step_n = steps;
        in.view_z_scale = 0.73f; in.gl_FragCoord = vec4(1.5f, 2.5f, 0, 0);
        const auto old = evaluate(false, in, true), now = evaluate(true, in, true);
        equal_results(now, old);
        CHECK(now.operations.sunlight == now.operations.samples);
        REQUIRE(now.operations.sunlight >= 4);
        CHECK(now.operations.sun_samples[0].visibility == 0);
        CHECK(now.operations.sun_samples[1].visibility == 1);
        CHECK(now.operations.sun_samples[2].visibility == 0.25f);
        const auto lit = evaluate(true, in, false);
        CHECK(now.color.x < lit.color.x);
        CHECK(now.color.w == lit.color.w);
    }
}
TEST_CASE("dense medium preserves early exit instead of skipping or finishing the march")
{
    for (float steps : {2.0f, 8.0f, 32.0f, 64.0f}) for (float sun : {0.0f, 4.0f}) {
        Inputs in; in.density = 2.0f; in.step_n = steps; in.sun_on = sun;
        const auto old = evaluate(false, in, true), now = evaluate(true, in, true);
        equal_results(now, old);
        CHECK(now.operations.samples == 1);
        CHECK(now.color.w < 0.005f);
        CHECK(now.operations.exponentials == 1);
        CHECK(old.operations.exponentials == 2);
    }
}
TEST_CASE("uniform medium preserves origin independence zero extinction and negative zero falloff")
{
    for (float origin : {-100.0f, 0.0f, 100.0f}) for (float falloff : {0.0f, -0.0f}) {
        Inputs in; in.height_origin = origin; in.height_falloff = falloff; in.density = 0;
        const auto old = evaluate(false, in, true), now = evaluate(true, in, true);
        equal_results(now, old);
        CHECK(now.color.x == 0); CHECK(now.color.y == 0); CHECK(now.color.z == 0);
        CHECK(now.color.w == 1); CHECK(now.operations.samples == 32);
        CHECK(now.operations.sunlight == 32); CHECK(now.operations.exponentials == 1);
    }
}

TEST_CASE("enabled density field updates the same local density for extinction and scattering")
{
    Inputs uniform;
    uniform.density = 0.000125f;
    Inputs noisy = uniform;
    noisy.noise_on = true;
    noisy.noise_multiplier = 2.0f;
    uniform.density *= noisy.noise_multiplier;
    const auto expected = evaluate(true, uniform);
    const auto actual = evaluate(true, noisy);
    equal_results(actual, expected);
    CHECK(actual.operations.noise == actual.operations.samples);
    CHECK(expected.operations.noise == 0);
}

TEST_CASE("cloud visibility dims directional fog only and skips fully CSM-blocked samples")
{
    Inputs sky;
    const auto clear = evaluate(true, sky, true);
    sky.cloud_shadow_on = true;
    sky.cloud_transmittance = 0.0f;
    const auto covered = evaluate(true, sky, true);
    CHECK(clear.color.x > covered.color.x);
    CHECK(clear.color.w == covered.color.w);
    CHECK(clear.operations.cloud == 0);
    CHECK(covered.operations.cloud > 0);
    CHECK(covered.operations.cloud < covered.operations.sunlight);

    sky.sun_on = 0.0f;
    const auto ambient_only = evaluate(true, sky, true);
    CHECK(covered.color.x == ambient_only.color.x);
    CHECK(covered.color.y == ambient_only.color.y);
    CHECK(covered.color.z == ambient_only.color.z);
    CHECK(ambient_only.operations.cloud == 0);
}
