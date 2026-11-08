/* Complete actual shader helpers with counted typed CPU math/texture ports.
 * No shader compiler, renderer, texture allocation, native window or GPU. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {
struct vec2 { float x, y; };
struct vec3 {
    float x, y, z;
    vec2 xy() const { return {x, y}; }
};
struct vec4 {
    float x, y, z, w, r, a;
    vec4(float x_, float y_, float z_, float w_)
        : x(x_), y(y_), z(z_), w(w_), r(x_), a(w_) {}
    vec4(vec3 v, float w_) : vec4(v.x, v.y, v.z, w_) {}
    vec3 xyz() const { return {x, y, z}; }
};
vec2 operator*(vec2 a, float b) { return {a.x*b, a.y*b}; }
vec2 operator+(vec2 a, float b) { return {a.x+b, a.y+b}; }
vec3 operator/(vec3 a, float b) { return {a.x/b, a.y/b, a.z/b}; }
struct Matrix { std::array<float,16> elements; };
struct Texture { unsigned slot; };
struct Counts { unsigned transforms = 0, textures = 0; };
Counts counts;
std::array<float,4> depths;
vec4 u_volfog_sun_color(1,1,1,4);
vec4 u_volfog_csm_splits(10,25,50,100);
vec4 u_volfog_csm_params(1.0f/2048,0.22f,0,0);
Matrix u_volfog_csm_vp[4];
Texture s_volfog_csm0{0}, s_volfog_csm1{1}, s_volfog_csm2{2}, s_volfog_csm3{3};

vec4 mul(const Matrix &matrix, vec4 value)
{
    ++counts.transforms;
    const float in[4] = {value.x,value.y,value.z,value.w};
    float out[4] = {};
    for (unsigned row=0;row<4;++row)
        for (unsigned column=0;column<4;++column)
            out[row] += matrix.elements[row*4+column]*in[column];
    return {out[0],out[1],out[2],out[3]};
}
vec4 texture2D(Texture texture, vec2 uv)
{
    ++counts.textures;
    const float d = depths[texture.slot] + 0.1f*uv.x - 0.05f*uv.y;
    return {d,d,d,d};
}
float clamp(float v, float low, float high)
{ return v < low ? low : v > high ? high : v; }
float max(float a, float b) { return std::max(a,b); }
float mix(float a, float b, float t) { return (1-t)*a+t*b; }
float smoothstep(float a, float b, float x)
{
    const float t=clamp((x-a)/(b-a),0,1);
    return t*t*(3-2*t);
}

namespace current {
#include JCE_TEST_VFOG_CURRENT
}
namespace reference {
#include JCE_TEST_VFOG_REFERENCE
}

void reset(float cascade_count=4, float blend=0.22f)
{
    counts={}; depths={0.30f,0.80f,0.40f,0.90f};
    u_volfog_sun_color=vec4(1,1,1,cascade_count);
    const float splits[4]={10,25,50,100};
    const unsigned n=static_cast<unsigned>(cascade_count);
    float padded[4]={};
    for (unsigned i=0;i<4;++i) padded[i]=n?splits[std::min(i,n-1)]:0;
    u_volfog_csm_splits=vec4(padded[0],padded[1],padded[2],padded[3]);
    u_volfog_csm_params=vec4(1.0f/2048,blend,0,0);
    for (auto &matrix:u_volfog_csm_vp) {
        matrix.elements={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    }
}
struct Result { float visibility; Counts operations; };
Result evaluate(bool optimized, vec3 position, float z)
{
    counts={};
    const float value=optimized ? current::volfog_sunlight(position,z)
                                : reference::volfog_sunlight(position,z);
    return {value,counts};
}
void equal_result(float actual,float expected)
{
    if(std::isnan(expected)) CHECK(std::isnan(actual));
    else CHECK(actual==expected);
}
}

TEST_CASE("finite complete helpers retain visibility across all cascade and fade boundaries")
{
    for (unsigned n=1;n<=4;++n) for(float blend:{0.0f,0.22f,0.35f}) {
        reset(static_cast<float>(n),blend);
        const float shadow_reach=u_volfog_csm_splits.w;
        for (float z:{0.0f,8.0f,10.0f,20.0f,25.0f,45.0f,50.0f,
                      shadow_reach*0.85f,std::nextafter(shadow_reach,0.0f),shadow_reach,
                      std::nextafter(shadow_reach,std::numeric_limits<float>::infinity()),shadow_reach*2})
            for(float x:{-0.9f,0.0f,0.9f}) for(float y:{-0.8f,0.0f,0.8f}) {
                const vec3 p{x,y,0.5f};
                const auto old=evaluate(false,p,z), now=evaluate(true,p,z);
                equal_result(now.visibility,old.visibility);
                if(z<shadow_reach) {
                    CHECK(now.operations.textures==old.operations.textures);
                    CHECK(now.operations.transforms==old.operations.transforms);
                }
            }
    }
}
TEST_CASE("clip selection retains distinct projection matrices and its final fallback")
{
    reset();
    for (unsigned i=0;i<4;++i) {
        const float n=static_cast<float>(i+1);
        u_volfog_csm_vp[i].elements={
            1,2,0,11*n, 0,1,-1,13*n, 2,0,1,17*n, 0.25f,-0.5f,0,n};
    }
    /* Known products for (2,-3,5,1), including non-unit homogeneous w.
     * This checks projection selection, not native compiler branch costs. */
    const vec4 expected[4]={{7,5,26,3},{18,18,43,4},{29,31,60,5},{40,44,77,6}};
    for (int cascade : {0,1,2,3,-1,4}) {
        const unsigned selected=cascade>=0 && cascade<3 ? static_cast<unsigned>(cascade) : 3;
        const auto a=current::volfog_shadow_clip(cascade,{2,-3,5});
        const auto b=reference::volfog_shadow_clip(cascade,{2,-3,5});
        CHECK(a.x==expected[selected].x); CHECK(a.y==expected[selected].y);
        CHECK(a.z==expected[selected].z); CHECK(a.w==expected[selected].w);
        CHECK(a.x==b.x); CHECK(a.y==b.y); CHECK(a.z==b.z); CHECK(a.w==b.w);
    }
}
TEST_CASE("fully faded valid samples skip all actual matrix and texture work")
{
    reset();
    for(float z:{100.0f,std::nextafter(100.0f,std::numeric_limits<float>::infinity()),200.0f}) {
        const auto old=evaluate(false,{0,0,0.5f},z), now=evaluate(true,{0,0,0.5f},z);
        CHECK(old.operations.textures>0); CHECK(old.operations.transforms>0);
        CHECK(now.visibility==1.0f); equal_result(now.visibility,old.visibility);
        CHECK(now.operations.textures==0); CHECK(now.operations.transforms==0);
    }
}
TEST_CASE("the fade band is retained rather than moved to its start")
{
    reset(); depths[3]=0.0f;
    for(float z:{85.0f,90.0f,95.0f,std::nextafter(100.0f,0.0f)}) {
        const auto old=evaluate(false,{0,0,0.5f},z), now=evaluate(true,{0,0,0.5f},z);
        equal_result(now.visibility,old.visibility);
        CHECK(now.operations.textures==old.operations.textures);
        CHECK(now.operations.textures>0);
        if(z<95.0f) CHECK(now.visibility<1.0f);
    }
}
TEST_CASE("cascade fallthrough and blending still use the actual clip and texture helpers")
{
    reset();
    u_volfog_csm_vp[0].elements[3]=10;
    u_volfog_csm_vp[1].elements[3]=10;
    for(float z:{1.0f,9.0f,24.0f,48.0f}) {
        const auto old=evaluate(false,{0,0,0.5f},z), now=evaluate(true,{0,0,0.5f},z);
        equal_result(now.visibility,old.visibility);
        CHECK(now.operations.transforms==old.operations.transforms);
        CHECK(now.operations.textures==old.operations.textures);
        CHECK(now.operations.transforms>=1);
    }
}
TEST_CASE("sun disabled and invalid reach keep their previous control paths")
{
    reset(0);
    const auto disabled=evaluate(true,{0,0,0.5f},200);
    CHECK(disabled.visibility==1); CHECK(disabled.operations.textures==0);
    const float nan=std::numeric_limits<float>::quiet_NaN();
    for(float shadow_reach:{0.0f,-1.0f,nan,std::numeric_limits<float>::infinity(),
                  std::numeric_limits<float>::denorm_min()}) {
        reset();u_volfog_csm_splits=vec4(0,0,0,shadow_reach);
        const auto old=evaluate(false,{0,0,0.5f},200), now=evaluate(true,{0,0,0.5f},200);
        equal_result(now.visibility,old.visibility);
        CHECK(now.operations.transforms==old.operations.transforms);
        CHECK(now.operations.textures==old.operations.textures);
        CHECK(now.operations.transforms>0);
    }
}
TEST_CASE("NaN view depth does not become a known lit shortcut")
{
    reset();
    const float nan=std::numeric_limits<float>::quiet_NaN();
    const auto old=evaluate(false,{0,0,0.5f},nan), now=evaluate(true,{0,0,0.5f},nan);
    equal_result(now.visibility,old.visibility);
    CHECK(now.operations.transforms==old.operations.transforms);
    CHECK(now.operations.textures==old.operations.textures);
    CHECK(now.operations.transforms>0);
}
