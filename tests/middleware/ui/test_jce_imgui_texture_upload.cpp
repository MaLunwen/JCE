/* Exact private production atlas owner with counted GPU boundary ports.
 * No device/Noop/pixels/physical GPU completion is claimed by these controls. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#ifndef JCE_TEST_IMGUI_TEXTURE_UPLOAD_HEADER
#define JCE_TEST_IMGUI_TEXTURE_UPLOAD_HEADER "middleware/ui/jce_imgui_texture_upload_internal.h"
#endif
#include JCE_TEST_IMGUI_TEXTURE_UPLOAD_HEADER
#include <array>
#include <cstring>

namespace {
enum TextureStatus { Ready=0, Destroyed=1, WantCreate=2, WantUpdates=3, WantDestroy=4 };
const JceImGuiTextureCodes codes={Ready,WantCreate,WantUpdates,WantDestroy,Destroyed,0};
struct Rect {int x=0,y=0,w=0,h=0;};
struct Texture {
    int UniqueID=42,Format=0,Width=4,Height=4,BytesPerPixel=4,UnusedFrames=1;
    TextureStatus Status=WantCreate;
    void *BackendUserData=nullptr;
    uint64_t TexID=0;
    unsigned char *Pixels=nullptr;
    Rect UpdateRect;
    unsigned ready_calls=0;
    uint64_t GetTexID() const {return TexID;}
    void SetTexID(uint64_t id){TexID=id;}
    void SetStatus(TextureStatus status){Status=status;if(status==Ready)++ready_calls;}
    void *GetPixels(){return Pixels;}
    void *GetPixelsAt(int x,int y){return Pixels+(x+y*Width)*BytesPerPixel;}
};
struct Reference {
    Texture *_TexData=nullptr;
    uint64_t _TexID=0;
    mutable unsigned gets=0;
    uint64_t GetTexID() const {++gets;return _TexData?_TexData->TexID:_TexID;}
};
struct Memory {uint8_t *data;uint32_t size;};
struct Gpu {
    uint32_t dimension=16,creates=0,copies=0,allocations=0,uploads=0,destroys=0,warnings=0,recoveries=0;
    uint32_t invalid_uploads=0,invalid_destroys=0,last_bytes=0,last_x=0,last_y=0,last_w=0,last_h=0;
    uint16_t next_handle=0,last_upload=UINT16_MAX;
    bool fail_create=false,fail_copy=false,fail_alloc=false;
    std::array<bool,32> live{};
    std::array<unsigned,32> destroyed{};
    std::array<uint8_t,4096> staging{};
    std::array<uint8_t,4096> delivered{};
    Memory memory{staging.data(),0};
    Texture *observed=nullptr;
    TextureStatus upload_status=Destroyed;
    uint64_t upload_id=0;
    const char *last_reason=nullptr;
    uint32_t max_dimension() const {return dimension;}
    uint16_t create(uint16_t width,uint16_t height){
        ++creates;CHECK(width>0);CHECK(height>0);if(fail_create)return UINT16_MAX;
        const uint16_t h=next_handle++;REQUIRE(h<live.size());CHECK_FALSE(live[h]);live[h]=true;return h;
    }
    const Memory *copy(const void *pixels,uint32_t bytes){
        ++copies;if(fail_copy)return nullptr;REQUIRE(pixels);REQUIRE(bytes<=staging.size());
        memory.size=bytes;std::memcpy(memory.data,pixels,bytes);return &memory;
    }
    const Memory *alloc(uint32_t bytes){
        ++allocations;if(fail_alloc)return nullptr;REQUIRE(bytes<=staging.size());memory.size=bytes;return &memory;
    }
    void upload(uint16_t handle,uint16_t x,uint16_t y,uint16_t width,uint16_t height,const Memory *mem){
        ++uploads;if(handle>=live.size()||!live[handle])++invalid_uploads;
        REQUIRE(mem);CHECK(mem==&memory);REQUIRE(mem->size<=delivered.size());
        last_upload=handle;last_x=x;last_y=y;last_w=width;last_h=height;last_bytes=mem->size;
        std::memcpy(delivered.data(),mem->data,mem->size);
        if(observed){upload_status=observed->Status;upload_id=observed->TexID;}
    }
    void destroy(uint16_t handle){
        ++destroys;if(handle>=live.size()||!live[handle]){++invalid_destroys;return;}
        ++destroyed[handle];live[handle]=false;
    }
    void warning(const char *,const char *reason,int owner,int,int width,int height,uint64_t){
        ++warnings;last_reason=reason;CHECK(owner==42);CHECK((width!=0||height!=0));
    }
    void recovered(const char *,int owner){++recoveries;CHECK(owner==42);}
};
struct Session {
    std::array<uint8_t,64> pixels{};
    Texture tex;
    Gpu gpu;
    Session(){
        for(size_t i=0;i<pixels.size();++i)pixels[i]=uint8_t(i);
        tex.Pixels=pixels.data();gpu.observed=&tex;
        jce_trace_set_enabled(false);jce_trace_reset();jce_trace_task_restore_id(777);jce_trace_set_enabled(true);
    }
    ~Session(){jce_trace_set_enabled(false);jce_trace_task_restore_id(0);}
    void balanced() const {
        JceTraceStats stats={};jce_trace_get_stats(&stats);CHECK(stats.active_work_items==0);
        CHECK(stats.work_items_started==stats.work_items_completed);CHECK(jce_trace_task_current_id()==777);
        CHECK(gpu.invalid_uploads==0);CHECK(gpu.invalid_destroys==0);
    }
};
bool counter(uint64_t value,const char *name){
    JceTraceCursor cursor={};jce_trace_cursor_init(&cursor);JceTraceEvent events[256];
    const uint32_t count=jce_trace_read(&cursor,events,256);
    for(uint32_t i=0;i<count;++i)if(events[i].type==JCE_TRACE_EVENT_COUNTER&&
        !std::strcmp(events[i].name,name)&&events[i].value_u64==value)return true;
    return false;
}
unsigned terminal(const char *name,JceTraceTaskState state){
    JceTraceCursor cursor={};jce_trace_cursor_init(&cursor);JceTraceEvent events[256];
    const uint32_t count=jce_trace_read(&cursor,events,256);unsigned found=0;
    for(uint32_t i=0;i<count;++i)if(events[i].type==JCE_TRACE_EVENT_TASK_END&&
        !std::strcmp(events[i].name,name)&&events[i].value_u32[0]==uint32_t(state))++found;
    return found;
}
}

TEST_CASE("atlas create publishes only after upload and index zero remains owned") {
    Session s;
    REQUIRE(jce_imgui_texture_update(&s.tex,s.gpu,codes));CHECK(s.tex.Status==Ready);CHECK(s.tex.TexID==1);
    CHECK(s.gpu.upload_status==WantCreate);CHECK(s.gpu.upload_id==0);CHECK(s.gpu.last_bytes==64);
    CHECK(s.gpu.delivered[63]==63);CHECK(s.gpu.uploads==1);CHECK(s.tex.ready_calls==1);
    CHECK(counter(64,"ui.atlas.staging.accepted.bytes"));CHECK(counter(64,"ui.atlas.logical.request.bytes"));
    REQUIRE(jce_imgui_texture_destroy(&s.tex,s.gpu,codes));CHECK(s.gpu.destroyed[0]==1);CHECK(s.tex.TexID==0);
    REQUIRE(jce_imgui_texture_destroy(&s.tex,s.gpu,codes));CHECK(s.gpu.destroyed[0]==1);CHECK(s.gpu.destroys==1);
    CHECK(terminal("ui.atlas.create",JCE_TRACE_TASK_SUCCEEDED)==1);s.balanced();
}
TEST_CASE("invalid create stays pending retries without warnings and never uploads") {
    Session s;s.gpu.fail_create=true;
    for(unsigned i=0;i<3;++i){CHECK_FALSE(jce_imgui_texture_update(&s.tex,s.gpu,codes));CHECK(s.tex.Status==WantCreate);CHECK(s.tex.TexID==0);}
    CHECK(s.gpu.creates==3);CHECK(s.gpu.copies==0);CHECK(s.gpu.uploads==0);CHECK(s.tex.ready_calls==0);
    CHECK(s.gpu.warnings==1);CHECK(std::strcmp(s.gpu.last_reason?s.gpu.last_reason:"","texture-create")==0);
    CHECK(terminal("ui.atlas.create",JCE_TRACE_TASK_FAILED)==3);
    s.gpu.fail_create=false;REQUIRE(jce_imgui_texture_update(&s.tex,s.gpu,codes));CHECK(s.gpu.recoveries==1);
    CHECK_FALSE(s.tex.BackendUserData);CHECK(s.tex.Status==Ready);s.balanced();
}
TEST_CASE("copy OOM rolls new owner back once preserving old until successful replacement") {
    Session s;s.tex.TexID=8;s.gpu.live[7]=true;s.gpu.next_handle=8;s.gpu.fail_copy=true;
    for(unsigned i=0;i<2;++i){REQUIRE_FALSE(jce_imgui_texture_update(&s.tex,s.gpu,codes));CHECK(s.tex.TexID==8);CHECK(s.tex.Status==WantCreate);}
    CHECK(s.gpu.destroyed[8]==1);CHECK(s.gpu.destroyed[9]==1);CHECK(s.gpu.destroyed[7]==0);CHECK(s.gpu.live[7]);
    CHECK(s.gpu.warnings==1);CHECK(s.gpu.uploads==0);CHECK(s.tex.ready_calls==0);
    s.gpu.fail_copy=false;REQUIRE(jce_imgui_texture_update(&s.tex,s.gpu,codes));CHECK(s.tex.TexID==11);
    CHECK(s.gpu.upload_id==8);CHECK(s.gpu.upload_status==WantCreate);CHECK(s.gpu.destroyed[7]==1);CHECK(s.gpu.live[10]);
    CHECK(s.gpu.recoveries==1);s.balanced();
}
TEST_CASE("update allocation failure preserves ready resource and packed rows retry correctly") {
    Session s;s.tex.Status=WantUpdates;s.tex.TexID=3;s.gpu.live[2]=true;s.tex.UpdateRect={1,1,2,2};s.gpu.fail_alloc=true;
    for(unsigned i=0;i<2;++i)REQUIRE_FALSE(jce_imgui_texture_update(&s.tex,s.gpu,codes));
    CHECK(s.tex.Status==WantUpdates);CHECK(s.tex.TexID==3);CHECK(s.gpu.live[2]);CHECK(s.gpu.destroys==0);CHECK(s.gpu.uploads==0);
    Reference ref{&s.tex,0};CHECK(jce_imgui_texture_draw_handle(ref,codes)==2);CHECK(ref.gets==1);CHECK(s.gpu.warnings==1);
    s.gpu.fail_alloc=false;REQUIRE(jce_imgui_texture_update(&s.tex,s.gpu,codes));CHECK(s.tex.Status==Ready);
    CHECK(s.gpu.last_x==1);CHECK(s.gpu.last_y==1);CHECK(s.gpu.last_w==2);CHECK(s.gpu.last_h==2);CHECK(s.gpu.last_bytes==16);
    for(unsigned i=0;i<8;++i){CHECK(s.gpu.delivered[i]==s.pixels[20+i]);CHECK(s.gpu.delivered[8+i]==s.pixels[36+i]);}
    CHECK(counter(16,"ui.atlas.staging.accepted.bytes"));CHECK(terminal("ui.atlas.update",JCE_TRACE_TASK_FAILED)==2);
    s.tex.Status=WantUpdates;s.tex.UpdateRect={0,0,0,4};REQUIRE(jce_imgui_texture_update(&s.tex,s.gpu,codes));
    CHECK(s.gpu.uploads==1);CHECK(s.gpu.allocations==3);CHECK(s.gpu.recoveries==1);s.balanced();
}
TEST_CASE("format caps dimensions overflow pixels and invalid rectangles fail before GPU dispatch") {
    Session s;
    auto rejected=[&](){REQUIRE_FALSE(jce_imgui_texture_update(&s.tex,s.gpu,codes));CHECK(s.tex.Status==WantCreate);CHECK(s.tex.ready_calls==0);};
    s.tex.Format=1;rejected();s.tex.Format=0;s.tex.BytesPerPixel=1;rejected();s.tex.BytesPerPixel=4;
    s.tex.Width=0;rejected();s.tex.Width=65536;rejected();s.tex.Width=17;rejected();
    s.gpu.dimension=65535;s.tex.Width=32768;s.tex.Height=32768;rejected();s.tex.Width=4;s.tex.Height=4;
    s.tex.Pixels=nullptr;rejected();s.tex.Pixels=s.pixels.data();
    CHECK(s.gpu.creates==0);CHECK(s.gpu.copies==0);CHECK(s.gpu.allocations==0);
    s.tex.Status=WantUpdates;s.tex.TexID=1;s.gpu.live[0]=true;
    const Rect invalid[]={{-1,0,1,1},{0,-1,1,1},{3,0,2,1},{0,3,1,2},{0,0,-1,1}};
    for(const Rect &r:invalid){s.tex.UpdateRect=r;REQUIRE_FALSE(jce_imgui_texture_update(&s.tex,s.gpu,codes));CHECK(s.tex.Status==WantUpdates);}
    CHECK(s.gpu.allocations==0);CHECK(s.gpu.uploads==0);CHECK(s.gpu.destroys==0);
    s.tex.TexID=65536;s.tex.UpdateRect={0,0,1,1};REQUIRE_FALSE(jce_imgui_texture_update(&s.tex,s.gpu,codes));
    CHECK(s.gpu.allocations==0);s.balanced();
}
TEST_CASE("destroy waits unused then releases once with cold teardown legal") {
    Session s;s.tex.Status=WantDestroy;s.tex.TexID=6;s.tex.UnusedFrames=0;s.gpu.live[5]=true;
    REQUIRE_FALSE(jce_imgui_texture_update(&s.tex,s.gpu,codes));JceTraceStats before={};jce_trace_get_stats(&before);CHECK(before.recorded_events==0);
    s.tex.UnusedFrames=1;REQUIRE(jce_imgui_texture_update(&s.tex,s.gpu,codes));CHECK(s.gpu.destroyed[5]==1);CHECK(s.tex.Status==Destroyed);
    REQUIRE(jce_imgui_texture_destroy(&s.tex,s.gpu,codes));CHECK(s.gpu.destroys==1);CHECK(s.tex.TexID==0);s.balanced();
}
TEST_CASE("draw admission rejects pending create destroy and malformed IDs before getter") {
    Session s;s.tex.TexID=8;
    Reference ref{&s.tex,0};CHECK(jce_imgui_texture_draw_handle(ref,codes)==UINT16_MAX);CHECK(ref.gets==0);
    s.tex.Status=WantDestroy;CHECK(jce_imgui_texture_draw_handle(ref,codes)==UINT16_MAX);CHECK(ref.gets==0);
    s.tex.Status=Ready;CHECK(jce_imgui_texture_draw_handle(ref,codes)==7);CHECK(ref.gets==1);
    s.tex.TexID=0;CHECK(jce_imgui_texture_draw_handle(ref,codes)==UINT16_MAX);
    Reference raw{nullptr,65536};CHECK(jce_imgui_texture_draw_handle(raw,codes)==UINT16_MAX);
    raw._TexID=UINT64_MAX;CHECK(jce_imgui_texture_draw_handle(raw,codes)==UINT16_MAX);
    raw._TexID=65535;CHECK(jce_imgui_texture_draw_handle(raw,codes)==65534);
    CHECK(jce_imgui_texture_encode(UINT16_MAX)==0);CHECK(jce_imgui_texture_decode(1)==0);s.balanced();
}
TEST_CASE("trace off and idle atlas produce zero events preserve caller identity") {
    Session s;jce_trace_set_enabled(false);jce_trace_reset();jce_trace_task_restore_id(777);
    REQUIRE(jce_imgui_texture_update(&s.tex,s.gpu,codes));
    for(unsigned i=0;i<100;++i)REQUIRE(jce_imgui_texture_update(&s.tex,s.gpu,codes));
    JceTraceStats stats={};jce_trace_get_stats(&stats);CHECK(stats.recorded_events==0);CHECK(s.gpu.uploads==1);CHECK(s.gpu.creates==1);
    jce_trace_set_enabled(true);jce_trace_reset();jce_trace_task_restore_id(777);
    for(unsigned i=0;i<100;++i)REQUIRE(jce_imgui_texture_update(&s.tex,s.gpu,codes));
    jce_trace_get_stats(&stats);CHECK(stats.recorded_events==0);s.balanced();
}
