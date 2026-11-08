/* CPU dependency control for the complete actual stock point-light loop.
 * Actual direct BRDF math and radiance accumulation remain intact. Only shadow
 * samplers have typed ports; no shader, device, GPU pixel or timing is tested. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {
struct vec3 {
    float x = 0, y = 0, z = 0;
    vec3(float x_ = 0, float y_ = 0, float z_ = 0) : x(x_), y(y_), z(z_) {}
    vec3 &operator+=(vec3 other)
    { x += other.x; y += other.y; z += other.z; return *this; }
    vec3 &operator*=(float scalar)
    { x *= scalar; y *= scalar; z *= scalar; return *this; }
};
struct vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    vec4(float x_ = 0, float y_ = 0, float z_ = 0, float w_ = 0)
        : x(x_), y(y_), z(z_), w(w_) {}
    vec4(vec3 xyz_, float w_) : vec4(xyz_.x, xyz_.y, xyz_.z, w_) {}
    vec3 xyz() const { return {x,y,z}; }
    float &component(unsigned index)
    {
        switch (index) {
        case 0: return x;
        case 1: return y;
        case 2: return z;
        default: return w;
        }
    }
};
vec3 operator+(vec3 a, vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
vec3 operator-(vec3 a, vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
vec3 operator*(vec3 a, vec3 b) { return {a.x*b.x,a.y*b.y,a.z*b.z}; }
vec3 operator*(vec3 v, float scalar) { return {v.x*scalar,v.y*scalar,v.z*scalar}; }
vec3 operator*(float scalar, vec3 v) { return v*scalar; }
vec3 operator/(vec3 v, float scalar) { return {v.x/scalar,v.y/scalar,v.z/scalar}; }
float dot(vec3 a, vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
float length(vec3 v) { return std::sqrt(dot(v,v)); }
vec3 normalize(vec3 v) { return v/length(v); }
vec3 vec3_splat(float scalar) { return {scalar,scalar,scalar}; }
float pow(float value, float exponent) { return std::pow(value,exponent); }
using std::clamp;
using std::max;

namespace actual_brdf {
#include JCE_TEST_PBR_POINT_COMMON
}
#undef PI

void equal_vector(vec3 a, vec3 b)
{
    CHECK(std::isfinite(a.x)); CHECK(std::isfinite(a.y)); CHECK(std::isfinite(a.z));
    CHECK(std::isfinite(b.x)); CHECK(std::isfinite(b.y)); CHECK(std::isfinite(b.z));
    CHECK(a.x==b.x); CHECK(a.y==b.y); CHECK(a.z==b.z);
}
struct ShadowCall { int point, slot; bool cube; };
struct RadianceCall { int point; vec3 value; };
struct Counts {
    std::vector<int> brdf;
    std::vector<ShadowCall> shadow;
    std::vector<RadianceCall> radiance;
};
struct PointUniforms {
    std::array<vec4,32> words;
    std::vector<int> consumed;
    int point = -1;
    vec4 operator[](int word)
    {
        consumed.push_back(word); point=word/2;
        return words.at(static_cast<unsigned>(word));
    }
};
struct Inputs {
    vec4 u_lightCounts, u_normalScale, u_lookWrap;
    PointUniforms u_pointLights;
    std::array<vec4,4> u_pointShadowSlot, u_pointCubeBaseSlot;
    std::array<int,16> local_slots, cube_slots;
    std::array<float,64> local_visibility, cube_visibility;
    vec3 v_worldpos{0,0,0}, N{0,0,1}, V{0,0,1};
    vec3 F0{0.04f,0.08f,0.12f}, albedo{0.7f,0.4f,0.2f}, Lo;
    float metallic = 0.25f, roughness = 0.5f;
    Counts counts;

    Inputs()
    {
        local_slots.fill(-1); cube_slots.fill(-1);
        u_pointShadowSlot.fill(vec4(-1,-1,-1,-1));
        u_pointCubeBaseSlot.fill(vec4(-1,-1,-1,-1));
        for (unsigned i=0;i<64;++i) {
            local_visibility[i]=0.25f+float(i)*0.005f;
            cube_visibility[i]=0.5f+float(i)*0.005f;
        }
    }
    void point(unsigned index, float distance, float radius, vec3 color={1,0.5f,0.25f},
               float intensity=2, int local=-1, int cube=-1)
    {
        u_pointLights.words.at(index*2)={v_worldpos+vec3(0,0,distance),radius};
        u_pointLights.words.at(index*2+1)={color,intensity};
        local_slots.at(index)=local; cube_slots.at(index)=cube;
        u_pointShadowSlot.at(index/4).component(index%4)=float(local);
        u_pointCubeBaseSlot.at(index/4).component(index%4)=float(cube);
    }
    float shadow(int slot, bool cube, vec3 position, vec3 normal, vec3 light, float ndot)
    {
        const int index=u_pointLights.point;
        counts.shadow.push_back({index,slot,cube});
        equal_vector(position,v_worldpos); equal_vector(normal,N);
        equal_vector(light,u_pointLights.words.at(static_cast<unsigned>(index)*2).xyz());
        CHECK(std::isfinite(ndot)); CHECK(ndot>=0); CHECK(ndot<=1);
        if (slot<0) return 1;
        return (cube?cube_visibility:local_visibility).at(static_cast<unsigned>(slot));
    }
    float sampleLocalShadow(int slot, vec3 position, vec3 normal, vec3 light, float ndot)
    { return shadow(slot,false,position,normal,light,ndot); }
    float samplePointCubeShadow(int slot, vec3 position, vec3 normal, vec3 light, float ndot)
    { return shadow(slot,true,position,normal,light,ndot); }
    vec3 cookTorranceBRDFWrap(vec3 normal, vec3 view, vec3 light, vec3 f0,
                             vec3 base, float metal, float rough, float wrap)
    {
        counts.brdf.push_back(u_pointLights.point);
        return actual_brdf::cookTorranceBRDFWrap(normal,view,light,f0,base,metal,rough,wrap);
    }
    void jce_test_radiance(int index, vec3 radiance)
    { counts.radiance.push_back({index,radiance}); }
};
struct Current : Inputs {
#include JCE_TEST_PBR_POINT_CURRENT
};
struct Reference : Inputs {
#include JCE_TEST_PBR_POINT_REFERENCE
};
struct Result { vec3 total; Counts counts; std::vector<int> consumed; };
template<class Program> Result evaluate(const Inputs &in)
{
    Program program;
    static_cast<Inputs &>(program)=in;
    program.counts={}; program.u_pointLights.consumed.clear();
    program.run();
    return {program.Lo,program.counts,program.u_pointLights.consumed};
}
std::vector<int> indices(unsigned count)
{
    std::vector<int> result;
    for (unsigned i=0;i<count;++i) result.push_back(static_cast<int>(i));
    return result;
}
void equal_semantics(const Result &now, const Result &old, unsigned count)
{
    equal_vector(now.total,old.total);
    REQUIRE(old.counts.radiance.size()==count);
    for (unsigned i=0;i<count;++i) {
        REQUIRE(old.counts.radiance[i].point==static_cast<int>(i));
        const auto found=std::find_if(now.counts.radiance.begin(),now.counts.radiance.end(),
            [i](const RadianceCall &call) { return call.point==static_cast<int>(i); });
        if (found==now.counts.radiance.end()) equal_vector(old.counts.radiance[i].value,{});
        else equal_vector(found->value,old.counts.radiance[i].value);
    }
}
void check_work(const Result &result, const Inputs &in, unsigned count,
                const std::vector<int> &retained)
{
    std::vector<int> words;
    for (unsigned i=0;i<count;++i)
        for (int offset:{0,0,1,1}) words.push_back(static_cast<int>(i)*2+offset);
    CHECK(result.consumed==words);
    CHECK(result.counts.brdf==retained);
    REQUIRE(result.counts.radiance.size()==retained.size());
    for (unsigned i=0;i<retained.size();++i)
        CHECK(result.counts.radiance[i].point==retained[i]);
    const bool receive=in.u_normalScale.w<0.5f;
    REQUIRE(result.counts.shadow.size()==(receive?retained.size():0u));
    if (receive) for (unsigned i=0;i<retained.size();++i) {
        const int index=retained[i];
        const bool cube=in.cube_slots.at(static_cast<unsigned>(index))>=0;
        CHECK(result.counts.shadow[i].point==index);
        CHECK(result.counts.shadow[i].cube==cube);
        CHECK(result.counts.shadow[i].slot==(cube?in.cube_slots:in.local_slots).at(
            static_cast<unsigned>(index)));
    }
}
void compare(const Inputs &in, unsigned count, const std::vector<int> &retained)
{
    const auto now=evaluate<Current>(in), old=evaluate<Reference>(in);
    equal_semantics(now,old,count);
    check_work(now,in,count,retained);
    check_work(old,in,count,indices(count));
}
}

TEST_CASE("actual point loop keeps empty inside exact-radius and outside boundaries")
{
    Inputs empty;
    compare(empty,0,{});
    for (float receive:{0.0f,1.0f}) for (float distance:{0.25f,1.0f,2.0f}) {
        CAPTURE(receive);
        CAPTURE(distance);
        Inputs in; in.u_lightCounts.y=1; in.u_normalScale.w=receive;
        in.Lo={0.125f,0.25f,0.5f};
        in.point(0,distance,1,{1,0.5f,0.25f},2,17,-1);
        compare(in,1,distance<1?std::vector<int>{0}:std::vector<int>{});
    }
}
TEST_CASE("actual point loop preserves packed indices through all sixteen mixed slots")
{
    const float just_inside=std::nextafter(1.0f,0.0f);
    for (unsigned count=0;count<=16;++count) for (float receive:{0.0f,1.0f}) {
        CAPTURE(count);
        CAPTURE(receive);
        Inputs in; in.u_lightCounts.y=float(count); in.u_normalScale.w=receive;
        std::vector<int> retained;
        for (unsigned i=0;i<count;++i) {
            const std::array<float,4> distances{2,0.5f,1,just_inside};
            in.point(i,distances[i%4],1,{1+float(i),0.5f,-0.25f},1+float(i),
                     16+static_cast<int>(i),i%3==0?static_cast<int>(i)*2:-1);
            if (i%4==1 || i%4==3) retained.push_back(static_cast<int>(i));
        }
        compare(in,count,retained);
    }
}
TEST_CASE("actual positive attenuation just inside radius survives without an epsilon cutoff")
{
    Inputs baseline;
    equal_vector(actual_brdf::cookTorranceBRDF(baseline.N,baseline.V,baseline.N,
        baseline.F0,baseline.albedo,baseline.metallic,baseline.roughness),
        actual_brdf::cookTorranceBRDFWrap(baseline.N,baseline.V,baseline.N,
        baseline.F0,baseline.albedo,baseline.metallic,baseline.roughness,0));
    for (float wrap:{0.0f,0.35f,1.0f}) for (float metal:{0.0f,0.5f,1.0f})
        for (float rough:{0.1f,0.5f,1.0f}) {
            CAPTURE(wrap);
            CAPTURE(metal);
            CAPTURE(rough);
            Inputs in; in.u_lightCounts.y=1; in.u_normalScale.w=1;
            in.u_lookWrap.x=wrap; in.metallic=metal; in.roughness=rough;
            in.point(0,std::nextafter(1.0f,0.0f),1);
            compare(in,1,{0});
            const auto now=evaluate<Current>(in);
            CHECK(now.total.x>0); CHECK(now.total.y>0); CHECK(now.total.z>0);
            REQUIRE(now.counts.radiance.size()==1);
            CHECK(now.counts.radiance[0].value.x>0);
            CHECK(now.counts.radiance[0].value.x<0.000001f);
        }
}
TEST_CASE("actual nonzero attenuation retains finite colors signed zeros and negative RGB")
{
    for (vec3 color:{vec3(1,0.5f,0.25f),vec3(-1,0.5f,-0.25f),
                     vec3(0.0f,-0.0f,1),vec3(-0.0f,0.0f,-0.0f)})
        for (float intensity:{2.0f,0.0f,-0.0f}) for (float receive:{0.0f,1.0f}) {
            CAPTURE(color.x);
            CAPTURE(color.y);
            CAPTURE(color.z);
            CAPTURE(intensity);
            CAPTURE(receive);
            Inputs in; in.u_lightCounts.y=1; in.u_normalScale.w=receive;
            in.point(0,0.5f,1,color,intensity,21,7);
            compare(in,1,{0});
            const auto now=evaluate<Current>(in);
            if (color.x<0 && intensity>0) CHECK(now.total.x<0);
            if (color.z>0 && intensity>0) CHECK(now.total.z>0);
        }
}
TEST_CASE("actual unshadowed point slots keep inside contribution and avoid zero-attenuation work")
{
    for (bool outside:{false,true}) {
        Inputs in; in.u_lightCounts.y=3;
        const std::array<float,3> radii{9,4,1};
        for (unsigned i=0;i<radii.size();++i)
            in.point(i,outside?radii[i]*2:radii[i]*0.5f,radii[i]);
        compare(in,3,outside?std::vector<int>{}:std::vector<int>{0,1,2});
    }
}
