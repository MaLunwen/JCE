/* Actual shader-source arithmetic/address controls. No device or GPU shader. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

struct vec2 {
    float x, y;
    vec2(float a = 0, float b = 0) : x(a), y(b) { }
};
static vec2 operator+(vec2 a, vec2 b) { return {a.x+b.x,a.y+b.y}; }
static vec2 operator-(vec2 a, vec2 b) { return {a.x-b.x,a.y-b.y}; }
static vec2 operator*(vec2 a, vec2 b) { return {a.x*b.x,a.y*b.y}; }
static vec2 operator/(vec2 a, vec2 b) { return {a.x/b.x,a.y/b.y}; }
static vec2 operator+(vec2 a, float b) { return a+vec2(b,b); }
static vec2 operator+(float a, vec2 b) { return b+a; }
static vec2 operator-(vec2 a, float b) { return a-vec2(b,b); }
static vec2 operator-(float a, vec2 b) { return vec2(a,a)-b; }
static vec2 operator*(vec2 a, float b) { return {a.x*b,a.y*b}; }
static vec2 operator*(float a, vec2 b) { return b*a; }
static vec2 floor(vec2 a) { return {std::floor(a.x),std::floor(a.y)}; }
static vec2 vec2_splat(float a) { return {a,a}; }
static vec2 clamp(vec2 v, vec2 lo, vec2 hi)
{
    return {std::clamp(v.x,lo.x,hi.x),std::clamp(v.y,lo.y,hi.y)};
}
static float floor(float a) { return std::floor(a); }
static float mod(float a, float b) { return std::fmod(a,b); }
static float min(float a, float b) { return std::min(a,b); }
struct vec4 { float x,y,z,w; };
static vec4 u_csmDynParams, u_shadowQuality;
using sampler2D = int;
static const sampler2D s_shadowMap = 5;

enum class Field { Lit, Dark, Vertical, Diagonal, Checker, Tile, Impulse, Equal };
static Field field;
static int atlas_size, tile_col, tile_row, edge_x, edge_y, impulse_x, impulse_y;
static bool tile_lit;
static std::vector<vec2> reads;
static bool crossed_tile;
struct depth_sample { float r; };
static depth_sample texture2D(sampler2D sampler, vec2 uv)
{
    CHECK(sampler == s_shadowMap);
    reads.push_back(uv);
    const int x = std::min(atlas_size-1,static_cast<int>(std::floor(uv.x*atlas_size)));
    const int y = std::min(atlas_size-1,static_cast<int>(std::floor(uv.y*atlas_size)));
    const int half = atlas_size/2;
    const bool own = x >= tile_col*half && x < (tile_col+1)*half &&
                     y >= tile_row*half && y < (tile_row+1)*half;
    crossed_tile |= !own;
    bool lit = true;
    switch(field) {
    case Field::Lit: lit=true; break;
    case Field::Dark: lit=false; break;
    case Field::Vertical: lit=x>=edge_x; break;
    case Field::Diagonal: lit=x+y>=edge_x+edge_y; break;
    case Field::Checker: lit=((x+y)&1)!=0; break;
    case Field::Tile: lit=own?tile_lit:!tile_lit; break;
    case Field::Impulse: lit=x==impulse_x && y==impulse_y; break;
    case Field::Equal: return {0.5f};
    }
    return {lit?1.0f:0.0f};
}
namespace actual {
#include JCE_TEST_CSM_DYNAMIC_OWNER
}
using Consumer = float(*)(float,int,vec2,float,float);
static const Consumer consumers[] = {actual::csm_dyn_min,actual::csm_dyn_pbr};

static void setup(int size=2048, int cascade=0, float quality=1)
{
    atlas_size=size;
    // Independent literal tile expectations for the two atlas origins.
    static const int columns[4]={0,1,0,1};
#if BGFX_SHADER_LANGUAGE_GLSL
    static const int rows[4]={1,1,0,0};
#else
    static const int rows[4]={0,0,1,1};
#endif
    tile_col=columns[cascade]; tile_row=rows[cascade];
    edge_x=tile_col*(size/2)+size/4;
    edge_y=tile_row*(size/2)+size/4;
    u_csmDynParams={2.0f,1.0f/size,1.0f,0};
    u_shadowQuality={quality,0,0,0};
    field=Field::Lit; reads.clear(); crossed_tile=false;
}
static float sample(Consumer consumer, vec2 uv, float s=1.0f,
                    int cascade=0, float z=0.5f, float bias=0.0f)
{
    reads.clear(); crossed_tile=false;
    const float v=consumer(s,cascade,uv,z,bias);
    CHECK(std::isfinite(v));
    CHECK(v>=-0.000001f); CHECK(v<=1.000001f);
    return v;
}
static bool close(float a,float b,float eps=0.000005f) { return std::abs(a-b)<=eps; }

TEST_CASE("Both dynamic consumers preserve disabled hard and nine-fetch policies")
{
    for(auto consumer:consumers) for(int size:{512,1024,2048,4096}) {
        for(int cascade=0;cascade<4;++cascade) for(float quality:{0.0f,1.0f,2.0f}) {
            setup(size,cascade,quality);
            u_csmDynParams.z=0;
            CHECK(sample(consumer,{0.37f,0.61f},0.37f,cascade)==0.37f);
            CHECK(reads.empty());
            u_csmDynParams.z=1;
            for(auto kind:{Field::Lit,Field::Dark,Field::Equal}) {
                field=kind;
                const float v=sample(consumer,{0.37f,0.61f},1,cascade);
                CHECK(close(v,kind==Field::Dark?0.0f:1.0f));
                CHECK(reads.size()==(quality<0.5f?1:9));
                CHECK_FALSE(crossed_tile);
            }
            // Strict depth comparison, including the original bias direction.
            field=Field::Equal;
            CHECK(close(sample(consumer,{0.37f,0.61f},1,cascade,0.51f,0),0));
            CHECK(close(sample(consumer,{0.37f,0.61f},1,cascade,0.51f,0.02f),1));
        }
    }
}

TEST_CASE("Nine comparisons vary continuously through subtexel and anchor crossings")
{
    for(auto consumer:consumers) for(int size:{512,1024,2048,4096}) {
        for(int cascade=0;cascade<4;++cascade) {
            setup(size,cascade);field=Field::Vertical;
            // Independently integrated tent response of a sharp half-plane.
            // A uniform point-PCF box has plateaus at 1/3 and 2/3 instead.
            float last=-1;
            for(int i=-128;i<=128;++i) {
                const float d=i/64.0f;
                const vec2 uv={0.5f+d/(size/2),0.43f};
                const float v=sample(consumer,uv,1,cascade);
                float expected=0;
                if(d>=1) expected=1;
                else if(d>=0) expected=1-0.5f*(1-d)*(1-d);
                else if(d>-1) expected=0.5f*(1+d)*(1+d);
                CHECK(close(v,expected,0.00015f));
                CHECK(v+0.000001f>=last);
                CHECK(reads.size()==9); CHECK_FALSE(crossed_tile);
                last=v;
            }
            for(auto kind:{Field::Vertical,Field::Diagonal,Field::Checker}) {
                field=kind;
                const float delta=0.001f/(size/2);
                const float before=sample(consumer,{0.5f-delta,0.5f-delta},1,cascade);
                const float after=sample(consumer,{0.5f+delta,0.5f+delta},1,cascade);
                CHECK(std::abs(after-before)<0.0045f);
            }
        }
    }
}

TEST_CASE("Every filtered read stays inside its cascade tile at boundaries")
{
    for(auto consumer:consumers) for(int size:{512,1024,2048,4096}) {
        for(int cascade=0;cascade<4;++cascade) for(float quality:{0.0f,1.0f,2.0f}) {
            setup(size,cascade,quality);field=Field::Tile;
            for(bool lit:{false,true}) {
                tile_lit=lit;
                for(float x:{0.0f,0.000001f,0.5f,0.999999f,1.0f}) {
                    for(float y:{0.0f,0.000001f,0.5f,0.999999f,1.0f}) {
                        CHECK(close(sample(consumer,{x,y},1,cascade),lit?1.0f:0.0f));
                        CHECK_FALSE(crossed_tile);
                    }
                }
            }
        }
    }
}

TEST_CASE("Mesh and common receiver routing agree and retain static min combination")
{
    for(int cascade=0;cascade<4;++cascade) for(float quality:{0.0f,1.0f,2.0f}) {
        setup(2048,cascade,quality);
        for(auto kind:{Field::Vertical,Field::Diagonal,Field::Checker}) {
            field=kind;
            for(int i=-11;i<=11;++i) for(int j=-7;j<=7;++j) {
                const vec2 uv={0.5f+i/2048.0f,0.5f+j/3072.0f};
                const float raw=sample(consumers[0],uv,1,cascade);
                CHECK(sample(consumers[1],uv,1,cascade)==raw);
                for(float s:{0.0f,0.25f,0.75f,1.0f}) for(auto consumer:consumers)
                    CHECK(close(sample(consumer,uv,s,cascade),std::min(s,raw)));
            }
        }
    }
}

TEST_CASE("Impulse coverage conserves mass centroid and a quarter-texel variance")
{
    // Recover actual weights via nine independent binary depth inputs; expected
    // moments are independent invariants, not a second copy of the kernel.
    setup(2048);field=Field::Impulse;
    const float inv=1.0f/atlas_size;
    const vec2 lo={tile_col*0.5f+inv*0.5f,tile_row*0.5f+inv*0.5f};
    const vec2 hi={tile_col*0.5f+0.5f-inv*0.5f,tile_row*0.5f+0.5f-inv*0.5f};
    for(float px:{-0.49f,-0.25f,0.0f,0.25f,0.49f}) {
        for(float py:{-0.49f,-0.25f,0.0f,0.25f,0.49f}) {
            const vec2 position={edge_x+0.5f+px,edge_y+0.5f+py};
            const vec2 uv=position*inv;
            double mass=0, mx=0, my=0, vx=0, vy=0;
            for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
                impulse_x=edge_x+x;impulse_y=edge_y+y;
                reads.clear();
                const float w=actual::csm_dynamic_pcf(s_shadowMap,uv,0.5f,0,
                                                      {inv,inv},lo,hi);
                CHECK(w>=0);CHECK(w<=1);CHECK(reads.size()==9);
                const double dx=(impulse_x+0.5)-position.x;
                const double dy=(impulse_y+0.5)-position.y;
                mass+=w;mx+=w*dx;my+=w*dy;vx+=w*dx*dx;vy+=w*dy*dy;
            }
            CHECK(std::abs(mass-1)<0.000001);
            CHECK(std::abs(mx)<0.000001);CHECK(std::abs(my)<0.000001);
            CHECK(std::abs(vx-0.25)<0.000001);CHECK(std::abs(vy-0.25)<0.000001);
        }
    }
}
