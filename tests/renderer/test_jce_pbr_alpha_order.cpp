/* Complete actual PBR main prefix through alpha admission. Only the complete
 * shadow dependency is ported; no GPU, shader compiler or texture is created. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {
struct vec2 {
    float x = 0, y = 0;
    vec2(float x_ = 0, float y_ = 0) : x(x_), y(y_) {}
};
struct vec3 {
    float x = 0, y = 0, z = 0;
    vec3(float x_ = 0, float y_ = 0, float z_ = 0) : x(x_), y(y_), z(z_) {}
    vec3(vec2 xy, float z_) : vec3(xy.x, xy.y, z_) {}
    vec3 &operator*=(vec3 other)
    { x *= other.x; y *= other.y; z *= other.z; return *this; }
};
struct vec4 {
    float x, y, z, w, a;
    vec4(float x_ = 0, float y_ = 0, float z_ = 0, float w_ = 0)
        : x(x_), y(y_), z(z_), w(w_), a(w_) {}
    vec4(vec3 rgb_, float alpha) : vec4(rgb_.x, rgb_.y, rgb_.z, alpha) {}
    vec2 xy() const { return {x, y}; }
    vec2 zw() const { return {z, w}; }
    vec3 rgb() const { return {x, y, z}; }
};
vec2 operator*(vec2 l, vec2 r) { return {l.x*r.x, l.y*r.y}; }
vec2 operator+(vec2 l, vec2 r) { return {l.x+r.x, l.y+r.y}; }
vec2 operator/(vec2 l, vec2 r) { return {l.x/r.x, l.y/r.y}; }
vec3 operator*(vec3 l, vec3 r) { return {l.x*r.x, l.y*r.y, l.z*r.z}; }
vec2 vec2_splat(float x) { return {x,x}; }
vec3 vec3_splat(float x) { return {x,x,x}; }
vec2 max(vec2 l, vec2 r) { return {std::max(l.x,r.x),std::max(l.y,r.y)}; }
vec3 max(vec3 l, vec3 r)
{ return {std::max(l.x,r.x),std::max(l.y,r.y),std::max(l.z,r.z)}; }
vec3 clamp(vec3 v, vec3 lo, vec3 hi)
{ return {std::clamp(v.x,lo.x,hi.x),std::clamp(v.y,lo.y,hi.y),std::clamp(v.z,lo.z,hi.z)}; }
vec3 pow(vec3 v, vec3 exponent)
{ return {std::pow(v.x,exponent.x),std::pow(v.y,exponent.y),std::pow(v.z,exponent.z)}; }
float fract(float x) { return x-std::floor(x); }
struct Texture { unsigned slot; };
struct Discard {};
struct Counts {
    unsigned graph = 0, texture = 0, array = 0, checker = 0, shadow = 0, lod = 0;
    vec2 sampled_uv, checker_uv, graph_uv, screen_uv;
    vec3 checker_position;
    float array_layer = -1;
    std::vector<char> events;
};
struct Inputs {
    vec2 v_texcoord0{0.25f,0.5f};
    vec3 v_worldpos{2,3,4}, v_normal{0,0,1}, v_localpos{-3,2,-1};
    vec4 u_uvTransform{2,3,0.125f,0.25f};
    vec4 u_viewRect{0,0,1280,800}, gl_FragCoord{3.5f,7.5f,0,0};
    vec4 u_normalScale{1,0,0,0}, u_baseColorFactor{1,1,1,1};
    vec4 u_emissiveFactor{0,0,0,1}, u_pbrParams{0,0,0,0.5f};
    vec4 u_iblParams{0,0,2,0}, v_tint{1,1,1,1};
    vec4 texture_color{0.25f,0.5f,0.75f,1}, graph_color{0.75f,0.25f,0.5f,1};
    vec3 checker_color{0.75f,0.25f,0.5f};
    Texture s_albedo{0};
    bool lod_discard = false, material_reached = false;
    vec4 gl_FragColor{-17,-19,-23,-29}, material_color;
    Counts counts;

    vec4 texture2D(Texture texture, vec2 uv)
    {
        CHECK(texture.slot == 0);
        ++counts.texture; counts.sampled_uv = uv; counts.events.push_back('T');
        return texture_color;
    }
    vec4 texture2DArray(Texture texture, vec3 uv)
    {
        CHECK(texture.slot == 0);
        ++counts.array; counts.sampled_uv = {uv.x,uv.y}; counts.array_layer = uv.z;
        counts.events.push_back('A'); return texture_color;
    }
    vec3 missing_texture_checker(vec3 position, vec2 uv)
    {
        ++counts.checker; counts.checker_uv = uv; counts.checker_position = position;
        counts.events.push_back('C'); return checker_color;
    }
    void jce_graph_material(vec2 uv, vec3, vec3, vec2 screen,
                            vec4 &base, vec3 &normal, float &metallic,
                            float &roughness, vec3 &emissive)
    {
        ++counts.graph; counts.graph_uv = uv; counts.screen_uv = screen;
        counts.events.push_back('G'); base = graph_color; normal = {0,0,1};
        metallic = 0.25f; roughness = 0.75f; emissive = {0,0,0};
    }
    bool jce_lod_fade_discard(vec2)
    { ++counts.lod; counts.events.push_back('L'); return lod_discard; }
    void jce_test_shadow_dependency()
    { ++counts.shadow; counts.events.push_back('S'); }
    void jce_test_material_result(vec3 albedo, float alpha)
    { material_reached = true; material_color = {albedo,alpha}; counts.events.push_back('R'); }
    [[noreturn]] void jce_test_discard()
    { counts.events.push_back('D'); throw Discard{}; }
};

#define discard jce_test_discard()
namespace regular {
struct Current : Inputs {
#include JCE_TEST_PBR_ALPHA_CURRENT
};
struct Reference : Inputs {
#include JCE_TEST_PBR_ALPHA_REFERENCE
};
}
#define JCE_INST_TINT
namespace tinted {
struct Current : Inputs {
#include JCE_TEST_PBR_ALPHA_CURRENT
};
struct Reference : Inputs {
#include JCE_TEST_PBR_ALPHA_REFERENCE
};
}
#undef JCE_INST_TINT
#define JCE_TEX_ARRAY
namespace arrayed {
struct Current : Inputs {
#include JCE_TEST_PBR_ALPHA_CURRENT
};
struct Reference : Inputs {
#include JCE_TEST_PBR_ALPHA_REFERENCE
};
}
#undef JCE_TEX_ARRAY
#define JCE_GRAPH_MATERIAL
namespace graph {
struct Current : Inputs {
#include JCE_TEST_PBR_ALPHA_CURRENT
};
struct Reference : Inputs {
#include JCE_TEST_PBR_ALPHA_REFERENCE
};
}
#define JCE_INST_TINT
namespace graph_tinted {
struct Current : Inputs {
#include JCE_TEST_PBR_ALPHA_CURRENT
};
struct Reference : Inputs {
#include JCE_TEST_PBR_ALPHA_REFERENCE
};
}
#undef JCE_INST_TINT
#define JCE_FADE_DITHER
namespace graph_faded {
struct Current : Inputs {
#include JCE_TEST_PBR_ALPHA_CURRENT
};
struct Reference : Inputs {
#include JCE_TEST_PBR_ALPHA_REFERENCE
};
}
#undef JCE_GRAPH_MATERIAL
namespace faded {
struct Current : Inputs {
#include JCE_TEST_PBR_ALPHA_CURRENT
};
struct Reference : Inputs {
#include JCE_TEST_PBR_ALPHA_REFERENCE
};
}
#undef JCE_FADE_DITHER
#undef discard

struct Result {
    bool discarded, material_reached;
    vec4 material, output;
    Counts counts;
};
template<class Program> Result evaluate(const Inputs &in)
{
    Program program;
    static_cast<Inputs &>(program) = in;
    program.counts = {}; program.material_reached = false;
    bool discarded = false;
    try { program.run(); } catch (const Discard &) { discarded = true; }
    return {discarded,program.material_reached,program.material_color,
            program.gl_FragColor,program.counts};
}
struct Variant {
    const char *name;
    Result (*current)(const Inputs &), (*reference)(const Inputs &);
    bool graph, tint, array, fade;
};
const std::array<Variant,7> variants{{
    {"regular",evaluate<regular::Current>,evaluate<regular::Reference>,false,false,false,false},
    {"tint",evaluate<tinted::Current>,evaluate<tinted::Reference>,false,true,false,false},
    {"array",evaluate<arrayed::Current>,evaluate<arrayed::Reference>,false,false,true,false},
    {"graph",evaluate<graph::Current>,evaluate<graph::Reference>,true,false,false,false},
    {"graph tint",evaluate<graph_tinted::Current>,evaluate<graph_tinted::Reference>,true,true,false,false},
    {"graph fade",evaluate<graph_faded::Current>,evaluate<graph_faded::Reference>,true,false,false,true},
    {"fade",evaluate<faded::Current>,evaluate<faded::Reference>,false,false,false,true}
}};
void equal_color(vec4 a, vec4 b)
{ CHECK(a.x==b.x); CHECK(a.y==b.y); CHECK(a.z==b.z); CHECK(a.w==b.w); }
void equal_semantics(const Result &now, const Result &old)
{
    CHECK(now.discarded==old.discarded);
    CHECK(now.material_reached==old.material_reached);
    if (!now.discarded) {
        if (now.material_reached) equal_color(now.material,old.material);
        else equal_color(now.output,old.output);
    }
    CHECK(now.counts.graph==old.counts.graph);
    CHECK(now.counts.texture==old.counts.texture);
    CHECK(now.counts.array==old.counts.array);
    CHECK(now.counts.checker==old.counts.checker);
    CHECK(now.counts.lod==old.counts.lod);
}
}

TEST_CASE("actual opaque mask and blend keep strict cutoff boundaries and alpha outputs")
{
    for (const auto &v:variants) for (float mode:{0.0f,1.0f,2.0f})
        for (float cutoff:{0.0f,0.5f,1.0f})
            for (float alpha:{0.0f,std::nextafter(0.5f,0.0f),0.5f,
                              std::nextafter(0.5f,1.0f),1.0f}) {
                CAPTURE(v.name,mode,cutoff,alpha);
                Inputs in; in.u_emissiveFactor.w=mode; in.u_pbrParams.w=cutoff;
                in.texture_color.a=alpha; in.graph_color.a=alpha;
                const auto now=v.current(in), old=v.reference(in);
                equal_semantics(now,old);
                const bool rejected=mode==1.0f && alpha<cutoff;
                CHECK(now.discarded==rejected);
                CHECK(now.counts.shadow==(rejected?0u:1u));
                CHECK(old.counts.shadow==1);
                if (!rejected) { CHECK(now.material_reached); CHECK(now.material.w==alpha); }
            }
}
TEST_CASE("actual factor graph and tint paths retain clamping alpha and texture-array coordinates")
{
    for (const auto &v:variants) {
        CAPTURE(v.name);
        Inputs in; in.u_emissiveFactor.w=0;
        in.texture_color={-0.25f,0.5f,1.25f,0.5f}; in.graph_color=in.texture_color;
        in.u_baseColorFactor={0.5f,0.25f,0.75f,0.5f};
        in.v_tint={0.25f,0.5f,1,1};
        if (v.tint) in.v_tint.a=0.5f;
        const auto now=v.current(in), old=v.reference(in);
        equal_semantics(now,old); REQUIRE(now.material_reached);
        vec4 expected=v.graph?vec4(0,0.5f,1,0.5f):vec4(0,0.125f,0.75f,0.25f);
        if (v.tint) expected=v.graph?vec4(0,0.25f,1,0.25f):vec4(0,0.0625f,0.75f,0.125f);
        equal_color(now.material,expected);
        CHECK(now.counts.graph==(v.graph?1u:0u));
        CHECK(now.counts.texture==(!v.graph&&!v.array?1u:0u));
        CHECK(now.counts.array==(v.array?1u:0u));
        const auto uv=v.graph?now.counts.graph_uv:now.counts.sampled_uv;
        CHECK(uv.x==0.625f); CHECK(uv.y==1.75f);
        if (v.array) CHECK(now.counts.array_layer==0.25f);
        if (v.graph) {
            CHECK(now.counts.screen_uv.x==in.gl_FragCoord.x/1280.0f);
            CHECK(now.counts.screen_uv.y==in.gl_FragCoord.y/800.0f);
        }
    }
}
TEST_CASE("checker fallback keeps raw mesh coordinates alpha one and existing factor rules")
{
    for (const auto &v:variants) {
        CAPTURE(v.name);
        Inputs in; in.u_normalScale.x=-1; in.u_emissiveFactor.w=0;
        in.texture_color.a=0; in.graph_color.a=0;
        in.u_baseColorFactor={0.5f,0.25f,0.75f,0.5f};
        const auto now=v.current(in), old=v.reference(in);
        equal_semantics(now,old); REQUIRE(now.material_reached);
        CHECK(now.counts.checker==1);
        CHECK(now.counts.checker_uv.x==in.v_texcoord0.x);
        CHECK(now.counts.checker_uv.y==in.v_texcoord0.y);
        CHECK(now.counts.checker_position.x==in.v_localpos.x);
        CHECK(now.counts.checker_position.y==in.v_localpos.y);
        CHECK(now.counts.checker_position.z==in.v_localpos.z);
        equal_color(now.material,v.graph?vec4(0.75f,0.25f,0.5f,1)
                                        :vec4(0.375f,0.0625f,0.375f,0.5f));
    }
}
TEST_CASE("view modes two and three still return gamma albedo before mask with no shadow dependency")
{
    for (const auto &v:variants) for (float view:{2.0f,3.0f}) {
        CAPTURE(v.name,view);
        Inputs in; in.u_normalScale.z=view; in.u_pbrParams.w=0.9f;
        in.texture_color={0.25f,0.5f,0.75f,0.25f}; in.graph_color=in.texture_color;
        auto now=v.current(in), old=v.reference(in);
        equal_semantics(now,old); CHECK_FALSE(now.discarded); CHECK_FALSE(now.material_reached);
        equal_color(now.output,{0.0625f,0.25f,0.5625f,0.25f});
        CHECK(now.counts.shadow==0); CHECK(old.counts.shadow==1); CHECK(now.counts.lod==1);
        in.lod_discard=true;
        now=v.current(in); old=v.reference(in); equal_semantics(now,old);
        CHECK(now.discarded); CHECK(now.counts.shadow==0); CHECK(now.counts.lod==1);
    }
}
TEST_CASE("actual fade dither remains before material fetch and graph evaluation remains before fade")
{
    for (const auto &v:variants) if (v.fade)
        for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x) {
            CAPTURE(v.name,x,y);
            Inputs in; in.u_emissiveFactor.w=0;
            in.gl_FragCoord={float(x)+0.5f,float(y)+0.5f,0,0};
            in.v_tint.a=0.35f;
            const auto positive=v.current(in), old_positive=v.reference(in);
            equal_semantics(positive,old_positive);
            in.v_tint.a=-0.35f;
            const auto negative=v.current(in), old_negative=v.reference(in);
            equal_semantics(negative,old_negative);
            CHECK(positive.discarded!=negative.discarded);
            CHECK(positive.counts.shadow+negative.counts.shadow==1);
            CHECK(positive.counts.texture+negative.counts.texture==(v.graph?0u:1u));
            for (const auto *result:{&positive,&negative}) {
                CHECK(result->counts.graph==(v.graph?1u:0u));
                CHECK(result->counts.shadow==(result->discarded?0u:1u));
                if (v.graph) {
                    REQUIRE_FALSE(result->counts.events.empty());
                    CHECK(result->counts.events.front()=='G');
                }
                if (result->discarded) {
                    CHECK(result->counts.texture==0); CHECK(result->counts.array==0);
                    CHECK_FALSE(result->material_reached);
                }
            }
        }
}
