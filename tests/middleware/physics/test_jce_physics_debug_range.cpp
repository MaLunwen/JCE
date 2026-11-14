/* Real Bullet world/BVH execution of the production query. No renderer,
 * dynamics step, Windows host or full SDK acceptance is claimed here. */
#include "../../../engine/src/middleware/physics/jce_physics_debug_query.hpp"
#include "../../../engine/src/middleware/physics/jce_physics_debug_range.hpp"
#include <BulletCollision/BroadphaseCollision/btDbvtBroadphase.h>
#include <BulletCollision/CollisionDispatch/btDefaultCollisionConfiguration.h>
#include <BulletCollision/CollisionDispatch/btCollisionDispatcher.h>
#include <BulletCollision/CollisionDispatch/btCollisionWorld.h>
#include <BulletCollision/CollisionShapes/btBoxShape.h>
#include <BulletCollision/CollisionShapes/btCapsuleShape.h>
#include <BulletCollision/CollisionShapes/btTriangleMesh.h>
#include <BulletCollision/CollisionShapes/btScaledBvhTriangleMeshShape.h>
#include <BulletCollision/CollisionShapes/btCompoundShape.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char *name) {
    ++checks; if (!ok) { ++failures; std::fprintf(stderr, "%s\n", name); }
}
struct Line { jce_vec3 a, b; };
std::vector<Line> lines;
void sink(jce_vec3 a, jce_vec3 b, uint32_t, void *) { lines.push_back({a,b}); }
struct World {
    btDefaultCollisionConfiguration configuration;
    btCollisionDispatcher dispatcher{&configuration};
    btDbvtBroadphase broadphase;
    btCollisionWorld world{&dispatcher,&broadphase,&configuration};
    std::vector<std::unique_ptr<btCollisionObject>> objects;
    btCollisionObject *add(btCollisionShape &shape, const btVector3 &at = btVector3(0,0,0)) {
        auto object = std::make_unique<btCollisionObject>();
        btTransform pose; pose.setIdentity(); pose.setOrigin(at);
        object->setWorldTransform(pose); object->setCollisionShape(&shape);
        world.addCollisionObject(object.get());
        objects.push_back(std::move(object)); return objects.back().get();
    }
    ~World() { for (auto &object : objects) world.removeCollisionObject(object.get()); }
};
JcePhysicsDebugQuery query(float radius = 30) {
    JcePhysicsDebugQuery q{}; q.struct_size = sizeof(q); q.flags = JCE_PHYS_DBG_WIREFRAME;
    q.bounds_min = jce_v3(-1.e9f,-1.e9f,-1.e9f); q.bounds_max = jce_v3(1.e9f,1.e9f,1.e9f);
    q.max_objects = 2048; q.max_shapes = 2048; q.max_mesh_parts = 2048;
    q.max_triangles = 8192; q.max_lines = 32768; q.range_radius = radius;
    q.keep_character = JceCharacterHandle{UINT32_MAX}; return q;
}
JcePhysicsDebugQueryStats run(World &w, const JcePhysicsDebugQuery &q,
                              const btCollisionObject *keep = nullptr) {
    JcePhysicsDebugQueryStats s{}; s.struct_size = sizeof(s); lines.clear();
    check(jce_bullet_debug_query_native(&w.world,&q,sink,nullptr,&s,keep), "native query accepts valid descriptor");
    check(s.lines_emitted == lines.size(), "reported line count equals real callback count"); return s;
}
void x_region(float low, float high, const char *name) {
    check(!lines.empty(), "expected native geometry is present");
    bool ok = true; for (const auto &line : lines)
        ok = ok && line.a.x >= low && line.a.x <= high && line.b.x >= low && line.b.x <= high;
    check(ok,name);
}
struct CountingMesh : btTriangleMesh {
    mutable uint32_t locks = 0;
    void getLockedReadOnlyVertexIndexBase(const unsigned char **v, int &nv, PHY_ScalarType &vt,
        int &vs, const unsigned char **i, int &is, int &nt, PHY_ScalarType &it, int part = 0) const override {
        ++locks; btTriangleMesh::getLockedReadOnlyVertexIndexBase(v,nv,vt,vs,i,is,nt,it,part);
    }
};
void triangle(btTriangleMesh &mesh, float x) {
    mesh.addTriangle(btVector3(x,0,0),btVector3(x,.4f,0),btVector3(x,0,.4f),false);
}
void descriptor_and_legacy() {
    static_assert(JCE_PHYSICS_DEBUG_QUERY_V1_SIZE == 52, "original binary prefix remains 52 bytes");
    static_assert(sizeof(JcePhysicsDebugQuery) == 76, "append-only current descriptor layout");
    btBoxShape box(btVector3(.5f,.5f,.5f)); World w; w.add(box);
    auto q = query();
    for (uint32_t size : {0u,51u,53u,75u,77u}) {
        q.struct_size = size; JcePhysicsDebugQueryStats s{};s.struct_size=sizeof(s);s.status=0x1234;lines.clear();
        check(!jce_bullet_debug_query_native(&w.world,&q,sink,nullptr,&s), "invalid descriptor size refuses");
        check(lines.empty() && s.status == 0x1234, "refusal has no callbacks or stats writes");
    }
    q=query();
    for (float radius : {-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        q.range_radius=radius;JcePhysicsDebugQueryStats s{};s.struct_size=sizeof(s);lines.clear();
        check(!jce_bullet_debug_query_native(&w.world,&q,sink,nullptr,&s) && lines.empty(), "invalid radius refuses");
    }
    q=query();q.range_center.x=std::numeric_limits<float>::quiet_NaN();
    JcePhysicsDebugQueryStats s{};s.struct_size=sizeof(s);
    check(!jce_bullet_debug_query_native(&w.world,&q,sink,nullptr,&s), "nonfinite camera refuses");
    q=query(0);q.struct_size=JCE_PHYSICS_DEBUG_QUERY_V1_SIZE;
    void *old = std::malloc(JCE_PHYSICS_DEBUG_QUERY_V1_SIZE);
    std::memcpy(old,&q,JCE_PHYSICS_DEBUG_QUERY_V1_SIZE);lines.clear();
    check(jce_bullet_debug_query_native(&w.world,static_cast<JcePhysicsDebugQuery *>(old),sink,nullptr,&s),
          "old 52-byte allocation executes without reading optional suffix");
    check(lines.size()==12 && s.shapes_admitted==1,"old prefix retains native box output");std::free(old);
}
void moving_camera_and_character() {
    btBoxShape box(btVector3(.5f,.5f,.5f));btCapsuleShape capsule(.3f,1.2f);World w;
    auto *near=w.add(box);w.add(box,btVector3(100,0,0));
    auto *player=w.add(capsule,btVector3(200,0,0));w.add(capsule,btVector3(400,0,0));
    auto q=query(3);auto s=run(w,q);check(s.shapes_admitted==1,"radius filters bodies before shape drawing");x_region(-1,1,"near box only");
    q.range_center.x=100;s=run(w,q);check(s.shapes_admitted==1,"moving camera selects different native body");x_region(99,101,"current camera region replaces previous region");
    btTransform moved; moved.setIdentity();moved.setOrigin(btVector3(102,0,0));near->setWorldTransform(moved);
    s=run(w,q);check(s.shapes_admitted==2,"latest native transform used without a cross-frame shape cache");
    q=query(3);q.range_flags=JCE_PHYS_QUERY_KEEP_CHARACTER;
    q.keep_character.idx=7;s=run(w,q,player);
    check(s.shapes_admitted==1 && !lines.empty(),"only exact live player survives outside camera range");
    x_region(199,201,"other distant capsule is not treated as player");
    q.max_objects=1;q.max_lines=12;s=run(w,q,player);
    check((s.status & JCE_PHYS_QUERY_LIMIT) && s.objects_visited==1,"player is prioritized under object/line budgets");x_region(199,201,"budget cannot give player share to terrain");
    q=query(3);s=run(w,q,player);check(lines.empty(),"keep pointer without opt-in never changes query");
    btCollisionObject detached;detached.setCollisionShape(&capsule);q.range_flags=JCE_PHYS_QUERY_KEEP_CHARACTER;
    s=run(w,q,&detached);check(lines.empty(),"object outside queried live world cannot be retained");
    q=query(0);s=run(w,q);check(s.shapes_admitted==4,"radius zero cancels only distance filtering");
}
void huge_bvh_and_limits() {
    CountingMesh mesh;const unsigned total=200003;
    for(unsigned i=0;i<total;++i)triangle(mesh,float(i)*2);
    btBvhTriangleMeshShape shape(&mesh,true);World w;w.add(shape);
    auto q=query(4);q.range_center.x=346842.f;q.max_triangles=64;
    mesh.locks=0;auto s=run(w,q);
    check(s.admitted_mesh_triangles>0 && s.admitted_mesh_triangles<100,"huge spanning mesh budgets local BVH candidates, not all 200003 faces");
    check(s.triangles_read==s.admitted_mesh_triangles && s.triangles_read<100,"nearby candidates consume actual indexed reads");
    check(mesh.locks==2,"one local part inspected/read; BVH traversal does not read triangle arrays");
    x_region(q.range_center.x-4.01f,q.range_center.x+4.01f,"only actual nearby geometry survives conservative BVH quantization");
    namespace r=jce_physics_debug_range;r::Candidates candidates;JcePhysicsDebugQueryStats metadata{};
    btTransform identity;identity.setIdentity();mesh.locks=0;
    r::collect(candidates,shape,0,identity,btVector3(1,1,1),q,metadata);
    check(candidates.node_visits<200 && candidates.count>0 && mesh.locks==0,"native escape-index traversal skips far subtrees before index/vertex work");
    q=query(1.e6f);q.max_triangles=2;run(w,q);auto two=lines;q.max_triangles=3;run(w,q);
    check(two.size()==6 && lines.size()==9 &&
          std::memcmp(two.data(),lines.data(),two.size()*sizeof(Line))==0,
          "fixed-range candidate sampling retains prefix when quota grows");
    q=query(4);q.range_center.x=146842.f;q.max_triangles=64;s=run(w,q);x_region(146830,146854,"moving camera queries fresh BVH region without stale candidates");
    q=query(0);q.max_triangles=31;s=run(w,q);check(s.admitted_mesh_triangles==total && s.triangles_read==31,"zero range uses original full-mesh bounded sampling");
    q=query(1.e6f);q.max_triangles=3;s=run(w,q);
    check((s.status & JCE_PHYS_QUERY_LIMIT) && s.admitted_mesh_triangles==16384 && s.triangles_read==3,
          "candidate overflow is explicitly partial while index budget stays bounded");
}
void crossing_and_scaled_mesh() {
    btTriangleMesh mesh;mesh.addTriangle(btVector3(-100,0,0),btVector3(100,0,0),btVector3(0,100,0),false);
    triangle(mesh,1000);btBvhTriangleMeshShape shape(&mesh,true);World w;w.add(shape);
    auto q=query(1);auto s=run(w,q);check(s.triangles_read==1 && lines.size()==3,"crossing triangle is kept whole even when vertices lie outside radius");
    btTriangleMesh points;triangle(points,2);triangle(points,100);points.setScaling(btVector3(2,3,4));
    btBvhTriangleMeshShape child(&points,true);btScaledBvhTriangleMeshShape scaled(&child,btVector3(-2,.5f,1));
    btCompoundShape compound;btTransform child_pose;child_pose.setIdentity();child_pose.setOrigin(btVector3(18,0,0));compound.addChildShape(child_pose,&scaled);
    World second;auto *object=second.add(compound,btVector3(2,0,0));q=query(1);q.range_center.x=12;
    s=run(second,q);check(s.triangles_read==1 && lines.size()==3,"scaled compound range admits local face only");x_region(11.99f,12.01f,"mesh and negative wrapper scale applied exactly once");
    btTransform rotated(btQuaternion(btVector3(0,0,1),SIMD_HALF_PI),btVector3(2,0,0));object->setWorldTransform(rotated);
    q.range_center=jce_v3(2,10,0);s=run(second,q);check(lines.size()==3,"rotated compound uses actual world BVH bounds");
    bool near=true;for(auto &line:lines)near=near&&std::fabs(line.a.y-10)<.01f&&std::fabs(line.b.y-10)<.01f;
    check(near,"rotated local face coordinates match world pose");
    btBvhTriangleMeshShape unquantized(&points,false);World third;third.add(unquantized);q=query(30);s=run(third,q);
    check((s.status & JCE_PHYS_QUERY_UNSUPPORTED) && lines.empty(),"nonquantized mesh explicitly refuses spatial path without full scan fallback");
    q.range_radius=0;s=run(third,q);check(s.triangles_read==2,"unquantized mesh retains legacy zero-range path");
}
} // namespace

extern "C" bool r16_native_old_port(void *world,const void *query,
    jce_debug_line_fn fn,void *ud,JcePhysicsDebugQueryStats *stats) {
    return jce_bullet_debug_query_native(static_cast<btCollisionWorld *>(world),
        static_cast<const JcePhysicsDebugQuery *>(query),fn,ud,stats);
}
#ifdef JCE_RANGE_SEPARATE_OLD_CALLER
extern "C" int r16_old_prefix_call(void *world);
#endif
int main() {
    descriptor_and_legacy();moving_camera_and_character();huge_bvh_and_limits();crossing_and_scaled_mesh();
#ifdef JCE_RANGE_SEPARATE_OLD_CALLER
    btBoxShape box(btVector3(1,1,1));World w;w.add(box);
    check(r16_old_prefix_call(&w.world)==0,"separately compiled exact R15 public header caller remains binary compatible");
#endif
    std::printf("physics debug native range: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
