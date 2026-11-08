/* Complete production observation owner with counted C99 SDL/CCD/COM/heap
 * ports. No OS window, display query, library load or graphics device runs. */
#include "unity.h"
#include "os/platform/jce_window_adapter_internal.h"
#include <string.h>
static unsigned char window_tag;
static JceWindow *window(void){return (JceWindow *)&window_tag;}
static struct {
    JceWindowAdapterRecord records[JCE_WINDOW_ADAPTER_LIMIT];
    JceWindowDisplaySource source;
    unsigned count,begins,adapters,verifies,finishes;
    bool begin_fail,adapter_fail,verify_fail;
    JceWindowAdapterReason begin_reason,verify_reason;
} port;
static bool begin(void *user,JceWindow *win,JceWindowDisplaySource *source,JceWindowAdapterReason *reason)
{TEST_ASSERT_EQUAL_PTR(&port,user);TEST_ASSERT_EQUAL_PTR(window(),win);++port.begins;
 *source=port.source;*reason=port.begin_reason;return !port.begin_fail;}
static int adapter(void *user,uint32_t index,JceWindowAdapterRecord *out)
{TEST_ASSERT_EQUAL_PTR(&port,user);++port.adapters;if(port.adapter_fail)return -1;
 if(index>=port.count)return 0;*out=port.records[index];return 1;}
static bool verify(void *user,const JceWindowDisplaySource *source,JceWindowAdapterReason *reason)
{TEST_ASSERT_EQUAL_PTR(&port,user);++port.verifies;TEST_ASSERT_EQUAL_HEX32(port.source.luid_low,source->luid_low);
 TEST_ASSERT_EQUAL_INT32(port.source.luid_high,source->luid_high);*reason=port.verify_reason;return !port.verify_fail;}
static void finish(void *user){TEST_ASSERT_EQUAL_PTR(&port,user);++port.finishes;}
static const JceWindowAdapterProvider provider={begin,adapter,verify,finish,&port};
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <dxgi1_2.h>
#if defined(__has_include)
#if __has_include(<dxgi1_6.h>)
#include <dxgi1_6.h>
#endif
#endif
#include "os/platform/jce_window_internal.h"
#include <jce/os/platform/jce_library.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_timer.h>
#include <wchar.h>
static IDXGIFactory factory;
static IDXGIAdapter adapters[JCE_WINDOW_ADAPTER_LIMIT];
static IDXGIAdapter1 flags_adapter;
static IDXGIOutput native_output;
static IDXGIFactoryVtbl factory_vtable;
static IDXGIAdapterVtbl adapter_vtable;
static IDXGIAdapter1Vtbl flags_vtable;
static IDXGIOutputVtbl output_vtable;
static unsigned char user_library_tag;
#if defined(__IDXGIFactory6_INTERFACE_DEFINED__) && (!defined(WINVER) || WINVER >= 0x0601)
static IDXGIFactory6 ranked_factory;
static IDXGIAdapter1 ranked_adapter;
static IDXGIFactory6Vtbl ranked_factory_vtable;
static IDXGIAdapter1Vtbl ranked_adapter_vtable;
static struct {
    uint32_t order[JCE_WINDOW_ADAPTER_LIMIT], count, index, next_first;
    unsigned queries, current, passes, enums, descs, factory_releases, adapter_releases;
    bool unavailable, partial_factory, adapter_live;
} rankport;

static HRESULT STDMETHODCALLTYPE query_factory6(IDXGIFactory *self, REFIID iid, void **out)
{
    static const GUID expected = {
        0xc1b6694f, 0xff09, 0x44a9, {0xb0, 0x3c, 0x77, 0x90, 0x0a, 0x0a, 0x1d, 0x17}
    };
    TEST_ASSERT_EQUAL_PTR(&factory, self);
    TEST_ASSERT_EQUAL_MEMORY(&expected, iid, sizeof expected);
    ++rankport.queries;
    *out = !rankport.unavailable || rankport.partial_factory ? &ranked_factory : NULL;
    return rankport.unavailable ? E_NOINTERFACE : S_OK;
}

static BOOL STDMETHODCALLTYPE ranked_factory_current(IDXGIFactory6 *self)
{
    TEST_ASSERT_EQUAL_PTR(&ranked_factory, self);
    ++rankport.current;
    return TRUE;
}

static HRESULT STDMETHODCALLTYPE enum_ranked_adapter(IDXGIFactory6 *self, UINT index,
    DXGI_GPU_PREFERENCE preference, REFIID iid, void **out)
{
    static const GUID expected = {
        0x29038f61, 0x3839, 0x4626, {0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05}
    };
    TEST_ASSERT_EQUAL_PTR(&ranked_factory, self);
    TEST_ASSERT_EQUAL_INT(DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, preference);
    TEST_ASSERT_EQUAL_MEMORY(&expected, iid, sizeof expected);
    TEST_ASSERT_FALSE(rankport.adapter_live);
    ++rankport.enums;
    if (index == 0) ++rankport.passes;
    *out = NULL;
    if (index >= rankport.count) return DXGI_ERROR_NOT_FOUND;
    rankport.index = index == 0 && rankport.passes == 2 && rankport.next_first != UINT32_MAX
        ? rankport.next_first : rankport.order[index];
    TEST_ASSERT_TRUE(rankport.index < port.count);
    rankport.adapter_live = true;
    *out = &ranked_adapter;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ranked_adapter_desc(IDXGIAdapter1 *self, DXGI_ADAPTER_DESC1 *out)
{
    TEST_ASSERT_EQUAL_PTR(&ranked_adapter, self);
    TEST_ASSERT_TRUE(rankport.adapter_live);
    ++rankport.descs;
    const JceWindowAdapterRecord *record = &port.records[rankport.index];
    memset(out, 0, sizeof *out);
    out->VendorId = record->vendor_id;
    out->DeviceId = record->device_id;
    out->AdapterLuid.LowPart = record->luid_low;
    out->AdapterLuid.HighPart = record->luid_high;
    out->Flags = record->software ? DXGI_ADAPTER_FLAG_SOFTWARE : 0;
    return S_OK;
}

static ULONG STDMETHODCALLTYPE release_ranked_factory(IDXGIFactory6 *self)
{
    TEST_ASSERT_EQUAL_PTR(&ranked_factory, self);
    TEST_ASSERT_FALSE(rankport.adapter_live);
    ++rankport.factory_releases;
    return 0;
}

static ULONG STDMETHODCALLTYPE release_ranked_adapter(IDXGIAdapter1 *self)
{
    TEST_ASSERT_EQUAL_PTR(&ranked_adapter, self);
    TEST_ASSERT_TRUE(rankport.adapter_live);
    rankport.adapter_live = false;
    ++rankport.adapter_releases;
    return 0;
}

static void setup_ranked_factory(void)
{
    memset(&rankport, 0, sizeof rankport);
    rankport.count = 2;
    rankport.order[1] = 1;
    rankport.next_first = UINT32_MAX;
    factory_vtable.QueryInterface = query_factory6;
    memset(&ranked_factory_vtable, 0, sizeof ranked_factory_vtable);
    ranked_factory_vtable.IsCurrent = ranked_factory_current;
    ranked_factory_vtable.EnumAdapterByGpuPreference = enum_ranked_adapter;
    ranked_factory_vtable.Release = release_ranked_factory;
    ranked_factory.lpVtbl = &ranked_factory_vtable;
    memset(&ranked_adapter_vtable, 0, sizeof ranked_adapter_vtable);
    ranked_adapter_vtable.GetDesc1 = ranked_adapter_desc;
    ranked_adapter_vtable.Release = release_ranked_adapter;
    ranked_adapter.lpVtbl = &ranked_adapter_vtable;
}
#endif
static struct {
    unsigned window,display,properties,monitor,monitor_info,open,symbol,create,close;
    unsigned factory_release,adapter_release,flags_release,flags_queries,flags_descs,output_release,output_calls,current,output_index;
    unsigned sizes,queries,names,allocs,frees,live,allocation_fail_at,name_bad;
    unsigned path_capacity,mode_capacity,path_count,mode_count,insufficient,change_query;
    unsigned change_monitor,change_name,clock_calls;
    uint64_t clock,clock_step;
    bool no_window,no_display,no_monitor,monitor_failure,monitor_unterminated,monitor_empty;
    bool no_library,no_user_library,no_symbol,no_ccd_symbol,factory_failure,partial_factory;
    bool desc_failure,flags_failure,partial_flags,flags_desc_failure,return_over_paths,return_over_modes,inactive,unavailable,ambiguous,clone;
    bool name_failure,no_matching_name,raw_change,backwards_clock;
    LONG sizes_result,query_result;
    HRESULT adapter_result;
    DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology;
    void *allocations[4];
} winport;
static ULONG STDMETHODCALLTYPE release_factory(IDXGIFactory *self)
{TEST_ASSERT_EQUAL_PTR(&factory,self);++winport.factory_release;return 0;}
static ULONG STDMETHODCALLTYPE release_adapter(IDXGIAdapter *self)
{TEST_ASSERT_TRUE(self>=adapters && self<adapters+JCE_WINDOW_ADAPTER_LIMIT);++winport.adapter_release;return 0;}
static ULONG STDMETHODCALLTYPE release_output(IDXGIOutput *self)
{TEST_ASSERT_EQUAL_PTR(&native_output,self);++winport.output_release;return 0;}
static HRESULT STDMETHODCALLTYPE enum_adapters(IDXGIFactory *self,UINT index,IDXGIAdapter **out)
{TEST_ASSERT_EQUAL_PTR(&factory,self);++port.adapters;*out=NULL;
 if(FAILED(winport.adapter_result))return winport.adapter_result;
 if(index>=port.count)return DXGI_ERROR_NOT_FOUND;
 winport.current=index;*out=&adapters[index];return S_OK;}
static HRESULT STDMETHODCALLTYPE adapter_desc(IDXGIAdapter *self,DXGI_ADAPTER_DESC *out)
{TEST_ASSERT_EQUAL_PTR(&adapters[winport.current],self);if(winport.desc_failure)return E_FAIL;
 memset(out,0,sizeof *out);JceWindowAdapterRecord *record=&port.records[winport.current];
 out->VendorId=record->vendor_id;out->DeviceId=record->device_id;
 out->AdapterLuid.LowPart=record->luid_low;out->AdapterLuid.HighPart=record->luid_high;return S_OK;}
static HRESULT STDMETHODCALLTYPE adapter_flags_query(IDXGIAdapter *self,REFIID iid,void **out)
{TEST_ASSERT_EQUAL_PTR(&adapters[winport.current],self);
 static const GUID expected={0x29038f61,0x3839,0x4626,{0x91,0xfd,0x08,0x68,0x79,0x01,0x1a,0x05}};
 TEST_ASSERT_EQUAL_MEMORY(&expected,iid,sizeof expected);
 ++winport.flags_queries;*out=winport.flags_failure && !winport.partial_flags ? NULL : &flags_adapter;
 return winport.flags_failure ? E_NOINTERFACE : S_OK;}
static HRESULT STDMETHODCALLTYPE adapter_flags_desc(IDXGIAdapter1 *self,DXGI_ADAPTER_DESC1 *out)
{TEST_ASSERT_EQUAL_PTR(&flags_adapter,self);++winport.flags_descs;if(winport.flags_desc_failure)return E_FAIL;
 memset(out,0,sizeof *out);out->Flags=port.records[winport.current].software ? DXGI_ADAPTER_FLAG_SOFTWARE : 0;
 /* Descriptor key is deliberately different; flags never replace GetDesc identity. */
 out->VendorId=0x9999;out->DeviceId=0x8888;out->AdapterLuid.LowPart=0x7777;return S_OK;}
static ULONG STDMETHODCALLTYPE release_flags(IDXGIAdapter1 *self)
{TEST_ASSERT_EQUAL_PTR(&flags_adapter,self);++winport.flags_release;return 0;}
/* Deliberately misleading NVIDIA output: an Optimus virtual output can expose
 * the same HMONITOR although CCD identifies Intel as the active source. */
static HRESULT STDMETHODCALLTYPE enum_outputs(IDXGIAdapter *self,UINT index,IDXGIOutput **out)
{TEST_ASSERT_EQUAL_PTR(&adapters[winport.current],self);++winport.output_calls;*out=NULL;
 if(index>0)return DXGI_ERROR_NOT_FOUND;winport.output_index=index;*out=&native_output;return S_OK;}
static HRESULT STDMETHODCALLTYPE output_desc(IDXGIOutput *self,DXGI_OUTPUT_DESC *out)
{TEST_ASSERT_EQUAL_PTR(&native_output,self);memset(out,0,sizeof *out);
 out->Monitor=(HMONITOR)(uintptr_t)(winport.current==0 ? 23 : 17);return S_OK;}
static HRESULT WINAPI create_factory(REFIID iid,void **out)
{++winport.create;static const GUID expected={0x7b7166ec,0x21c7,0x44ae,{0xb2,0x1a,0xc9,0xae,0x32,0x1a,0xe3,0x69}};
 TEST_ASSERT_EQUAL_MEMORY(&expected,iid,sizeof expected);*out=NULL;
 if(winport.factory_failure){if(winport.partial_factory)*out=&factory;return E_FAIL;}*out=&factory;return S_OK;}
static SDL_Window *fixture_window(JceWindow *win)
{TEST_ASSERT_EQUAL_PTR(window(),win);++winport.window;return winport.no_window ? NULL : (SDL_Window *)&window_tag;}
static SDL_DisplayID SDLCALL fixture_display(SDL_Window *win)
{TEST_ASSERT_EQUAL_PTR(&window_tag,win);++winport.display;return winport.no_display ? 0 : 7;}
static SDL_PropertiesID SDLCALL fixture_properties(SDL_DisplayID display)
{TEST_ASSERT_EQUAL_UINT(7,display);++winport.properties;return 8;}
static void *SDLCALL fixture_monitor(SDL_PropertiesID properties,const char *name,void *default_value)
{TEST_ASSERT_EQUAL_UINT(8,properties);TEST_ASSERT_EQUAL_STRING(SDL_PROP_DISPLAY_WINDOWS_HMONITOR_POINTER,name);
 TEST_ASSERT_NULL(default_value);++winport.monitor;return winport.no_monitor ? NULL : (void *)(uintptr_t)23;}
static BOOL WINAPI fixture_monitor_info(HMONITOR monitor,LPMONITORINFO info)
{TEST_ASSERT_EQUAL_PTR((void *)(uintptr_t)23,monitor);TEST_ASSERT_EQUAL_UINT(sizeof(MONITORINFOEXW),info->cbSize);
 ++winport.monitor_info;if(winport.monitor_failure)return FALSE;
 MONITORINFOEXW *extended=(MONITORINFOEXW *)info;
 if(winport.monitor_unterminated){for(unsigned i=0;i<CCHDEVICENAME;++i)extended->szDevice[i]=L'x';}
 else if(!winport.monitor_empty)wcscpy(extended->szDevice,winport.change_monitor==winport.monitor_info ? L"DISPLAY2" : L"DISPLAY1");
 return TRUE;}
static LONG WINAPI fixture_sizes(UINT32 flags,UINT32 *paths,UINT32 *modes)
{TEST_ASSERT_EQUAL_UINT32(QDC_ONLY_ACTIVE_PATHS,flags);++winport.sizes;
 *paths=winport.path_capacity;*modes=winport.mode_capacity;return winport.sizes_result;}
static LONG WINAPI fixture_query(UINT32 flags,UINT32 *paths,DISPLAYCONFIG_PATH_INFO *path_array,
    UINT32 *modes,DISPLAYCONFIG_MODE_INFO *mode_array,DISPLAYCONFIG_TOPOLOGY_ID *topology)
{TEST_ASSERT_EQUAL_UINT32(QDC_ONLY_ACTIVE_PATHS,flags);TEST_ASSERT_NULL(topology);
 TEST_ASSERT_NOT_NULL(path_array);TEST_ASSERT_NOT_NULL(mode_array);++winport.queries;
 TEST_ASSERT_EQUAL_UINT32(winport.path_capacity,*paths);TEST_ASSERT_EQUAL_UINT32(winport.mode_capacity,*modes);
 if(winport.queries<=winport.insufficient)return ERROR_INSUFFICIENT_BUFFER;
 if(winport.query_result!=ERROR_SUCCESS)return winport.query_result;
 TEST_ASSERT_TRUE(winport.path_count<=*paths);TEST_ASSERT_TRUE(winport.mode_count<=*modes);
 for(unsigned i=0;i<winport.path_count;++i){DISPLAYCONFIG_PATH_INFO *path=&path_array[i];
  memset(path,0,sizeof *path);path->flags=winport.inactive ? 0 : DISPLAYCONFIG_PATH_ACTIVE;
  path->sourceInfo.adapterId.LowPart=port.source.luid_low;path->sourceInfo.adapterId.HighPart=port.source.luid_high;
  if(winport.change_query==winport.queries && !winport.raw_change)++path->sourceInfo.adapterId.LowPart;
  path->sourceInfo.id=winport.clone ? 3 : 3+i;
  path->targetInfo.adapterId.LowPart=0x77777777;path->targetInfo.adapterId.HighPart=8;
  path->targetInfo.id=9+i;path->targetInfo.targetAvailable=!winport.unavailable;
  path->targetInfo.outputTechnology=winport.technology;
  path->targetInfo.refreshRate.Numerator=winport.change_query==winport.queries && winport.raw_change ? 75 : 60;
 }
 *paths=winport.return_over_paths ? winport.path_capacity+1 : winport.path_count;
 *modes=winport.return_over_modes ? winport.mode_capacity+1 : winport.mode_count;
 return ERROR_SUCCESS;}
static LONG WINAPI fixture_device_info(DISPLAYCONFIG_DEVICE_INFO_HEADER *header)
{++winport.names;TEST_ASSERT_EQUAL_UINT32(DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,header->type);
 TEST_ASSERT_EQUAL_UINT32(sizeof(DISPLAYCONFIG_SOURCE_DEVICE_NAME),header->size);
 TEST_ASSERT_TRUE(header->id>=3 && header->id<3+winport.path_count);
 if(winport.name_failure)return ERROR_INVALID_PARAMETER;
 DISPLAYCONFIG_SOURCE_DEVICE_NAME *name=(DISPLAYCONFIG_SOURCE_DEVICE_NAME *)header;
 const WCHAR *device=(header->id==3 || winport.ambiguous) && !winport.no_matching_name ? L"DISPLAY1" : L"DISPLAY2";
 if(winport.change_name==winport.names)device=L"DISPLAY2";
 wcscpy(name->viewGdiDeviceName,device);
 switch(winport.name_bad){case 1:header->size--;break;case 2:header->type=DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;break;
 case 3:header->id++;break;case 4:header->adapterId.LowPart++;break;case 5:header->adapterId.HighPart++;break;
 case 6:name->viewGdiDeviceName[0]=0;break;case 7:for(unsigned i=0;i<CCHDEVICENAME;++i)name->viewGdiDeviceName[i]=L'x';break;}
 return ERROR_SUCCESS;}
static JceLibrary JCE_CALL fixture_open(const char *name)
{++winport.open;if(!strcmp(name,"user32.dll"))return winport.no_user_library ? NULL : &user_library_tag;
 TEST_ASSERT_EQUAL_STRING("dxgi.dll",name);return winport.no_library ? NULL : &factory;}
static void *JCE_CALL fixture_symbol(JceLibrary lib,const char *name)
{++winport.symbol;if(lib==&user_library_tag){if(winport.no_ccd_symbol)return NULL;
  if(!strcmp(name,"GetDisplayConfigBufferSizes"))return (void *)fixture_sizes;
  if(!strcmp(name,"QueryDisplayConfig"))return (void *)fixture_query;
  TEST_ASSERT_EQUAL_STRING("DisplayConfigGetDeviceInfo",name);return (void *)fixture_device_info;}
 TEST_ASSERT_EQUAL_PTR(&factory,lib);TEST_ASSERT_EQUAL_STRING("CreateDXGIFactory1",name);
 return winport.no_symbol ? NULL : (void *)create_factory;}
static void JCE_CALL fixture_close(JceLibrary lib)
{TEST_ASSERT_TRUE(lib==&factory || lib==&user_library_tag);++winport.close;}
static void *fixture_malloc(size_t size)
{++winport.allocs;TEST_ASSERT_TRUE(size<=JCE_WINDOW_ADAPTER_MODE_LIMIT*sizeof(DISPLAYCONFIG_MODE_INFO));
 if(winport.allocs==winport.allocation_fail_at)return NULL;
 void *memory=jce_malloc(size);TEST_ASSERT_NOT_NULL(memory);
 for(unsigned i=0;i<4;++i)if(!winport.allocations[i]){winport.allocations[i]=memory;++winport.live;return memory;}
 TEST_FAIL_MESSAGE("CCD owner exceeded four live allocations");return NULL;}
static void fixture_free(void *memory)
{if(!memory)return;for(unsigned i=0;i<4;++i)if(winport.allocations[i]==memory){
 winport.allocations[i]=NULL;++winport.frees;--winport.live;jce_free(memory);return;}
 TEST_FAIL_MESSAGE("CCD owner released an unowned allocation");}
static uint64_t JCE_CALL fixture_clock(void)
{++winport.clock_calls;uint64_t value=winport.clock;winport.clock+=winport.clock_step;
 return winport.backwards_clock && winport.clock_calls>1 ? 999 : value;}
#define jce_window_sdl fixture_window
#define SDL_GetDisplayForWindow fixture_display
#define SDL_GetDisplayProperties fixture_properties
#define SDL_GetPointerProperty fixture_monitor
#define GetMonitorInfoW fixture_monitor_info
#define jce_library_open fixture_open
#define jce_library_symbol fixture_symbol
#define jce_library_close fixture_close
#define jce_malloc fixture_malloc
#define jce_free fixture_free
#define jce_time_ticks_ms fixture_clock
#endif
#ifndef JCE_WINDOW_ADAPTER_OWNER
#define JCE_WINDOW_ADAPTER_OWNER "os/platform/jce_window_adapter.c"
#endif
#include JCE_WINDOW_ADAPTER_OWNER
void setUp(void)
{
    memset(&port,0,sizeof port);port.count=2;port.source=(JceWindowDisplaySource){0xabcdef01,-5,true,3,1,1};
    port.records[0]=(JceWindowAdapterRecord){0x10de,0x2507,1,2,false};
    port.records[1]=(JceWindowAdapterRecord){0x8086,0x9a49,0xabcdef01,-5,false};
#if defined(_WIN32)
    memset(&winport,0,sizeof winport);winport.path_capacity=winport.path_count=1;
    winport.mode_capacity=winport.mode_count=1;winport.clock=1000;winport.clock_step=1;
    winport.technology=DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL;
    memset(&factory_vtable,0,sizeof factory_vtable);factory_vtable.Release=release_factory;factory_vtable.EnumAdapters=enum_adapters;
    memset(&adapter_vtable,0,sizeof adapter_vtable);adapter_vtable.Release=release_adapter;
    adapter_vtable.GetDesc=adapter_desc;adapter_vtable.EnumOutputs=enum_outputs;adapter_vtable.QueryInterface=adapter_flags_query;
    memset(&flags_vtable,0,sizeof flags_vtable);flags_vtable.Release=release_flags;flags_vtable.GetDesc1=adapter_flags_desc;
    flags_adapter.lpVtbl=&flags_vtable;
    memset(&output_vtable,0,sizeof output_vtable);output_vtable.Release=release_output;output_vtable.GetDesc=output_desc;
    factory.lpVtbl=&factory_vtable;native_output.lpVtbl=&output_vtable;
    for(unsigned i=0;i<JCE_WINDOW_ADAPTER_LIMIT;++i)adapters[i].lpVtbl=&adapter_vtable;
#if defined(JCE_WINDOW_HAS_DXGI_FACTORY6)
    setup_ranked_factory();
#endif
#endif
}
void tearDown(void)
{
#if defined(_WIN32)
    TEST_ASSERT_EQUAL_UINT(0,winport.live);
#endif
}
static void ccd_source_luid_selects_unique_full_identity_and_finishes_once(void)
{
    JceWindowAdapterObservation result;
    TEST_ASSERT_TRUE(jce_window_observe_display_adapter_with_provider(window(),&provider,&result));
    TEST_ASSERT_EQUAL_HEX16(0x8086,result.vendor_id);TEST_ASSERT_EQUAL_HEX16(0x9a49,result.device_id);
    TEST_ASSERT_EQUAL_UINT32(1,result.adapter_index);TEST_ASSERT_EQUAL_HEX32(0xabcdef01,result.luid_low);
    TEST_ASSERT_EQUAL_INT32(-5,result.luid_high);TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_MATCH,result.reason);
    TEST_ASSERT_TRUE(result.source_valid);TEST_ASSERT_EQUAL_HEX32(0xabcdef01,result.source_luid_low);
    TEST_ASSERT_EQUAL_INT32(-5,result.source_luid_high);TEST_ASSERT_EQUAL_UINT32(3,result.source_id);
    TEST_ASSERT_EQUAL_UINT32(1,result.path_count);TEST_ASSERT_EQUAL_UINT32(1,result.mode_count);
    TEST_ASSERT_EQUAL_UINT32(2,result.adapter_count);TEST_ASSERT_EQUAL_UINT32(1,result.luid_match_count);
    TEST_ASSERT_EQUAL_UINT32(1,result.key_match_count);
    TEST_ASSERT_EQUAL_UINT(1,port.begins);TEST_ASSERT_EQUAL_UINT(3,port.adapters);
    TEST_ASSERT_EQUAL_UINT(1,port.verifies);TEST_ASSERT_EQUAL_UINT(1,port.finishes);
}
static void absent_and_duplicate_luid_preserve_original_policy(void)
{
    JceWindowAdapterObservation result;port.source.luid_low=29;
    TEST_ASSERT_FALSE(jce_window_observe_display_adapter_with_provider(window(),&provider,&result));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_NO_MATCH,result.reason);TEST_ASSERT_EQUAL_UINT(0,result.vendor_id);
    port.source.luid_low=0xabcdef01;port.records[0].luid_low=port.source.luid_low;port.records[0].luid_high=port.source.luid_high;
    TEST_ASSERT_FALSE(jce_window_observe_display_adapter_with_provider(window(),&provider,&result));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_DXGI_LUID_AMBIGUOUS,result.reason);
    TEST_ASSERT_TRUE(result.source_valid);TEST_ASSERT_EQUAL_HEX32(port.source.luid_low,result.source_luid_low);
    TEST_ASSERT_EQUAL_UINT32(2,result.adapter_count);TEST_ASSERT_EQUAL_UINT32(2,result.luid_match_count);
    TEST_ASSERT_EQUAL_UINT32(0,result.key_match_count);TEST_ASSERT_EQUAL_UINT32(0,result.vendor_id);
    TEST_ASSERT_EQUAL_UINT(2,port.finishes);
}
static void duplicate_vendor_device_on_another_luid_is_not_selectable_by_bgfx(void)
{
    port.records[0].vendor_id=port.records[1].vendor_id;port.records[0].device_id=port.records[1].device_id;
    JceWindowAdapterObservation result;
    TEST_ASSERT_FALSE(jce_window_observe_display_adapter_with_provider(window(),&provider,&result));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_BACKEND_KEY_MISMATCH,result.reason);TEST_ASSERT_EQUAL_UINT(0,result.device_id);
    TEST_ASSERT_TRUE(result.source_valid);TEST_ASSERT_EQUAL_HEX32(port.source.luid_low,result.source_luid_low);
    TEST_ASSERT_EQUAL_UINT32(2,result.adapter_count);TEST_ASSERT_EQUAL_UINT32(1,result.luid_match_count);
    TEST_ASSERT_EQUAL_UINT32(2,result.key_match_count);
    TEST_ASSERT_TRUE(result.backend_expected_valid);TEST_ASSERT_EQUAL_UINT32(0,result.backend_expected_index);
    TEST_ASSERT_EQUAL_HEX32(port.records[0].luid_low,result.backend_expected_luid_low);
    port.records[0].software=true;
    TEST_ASSERT_FALSE(jce_window_observe_display_adapter_with_provider(window(),&provider,&result));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_BACKEND_KEY_MISMATCH,result.reason);
}
static void first_matching_key_is_the_ccd_source_despite_later_distinct_luid(void)
{
    port.count=3;port.records[2]=port.records[1];port.records[2].luid_low=0x29360;
    JceWindowAdapterObservation result;
    TEST_ASSERT_TRUE(jce_window_observe_display_adapter_with_provider(window(),&provider,&result));
    TEST_ASSERT_EQUAL_HEX16(0x8086,result.vendor_id);TEST_ASSERT_EQUAL_HEX32(port.source.luid_low,result.luid_low);
    TEST_ASSERT_EQUAL_UINT32(3,result.adapter_count);TEST_ASSERT_EQUAL_UINT32(1,result.luid_match_count);
    TEST_ASSERT_EQUAL_UINT32(2,result.key_match_count);TEST_ASSERT_TRUE(result.backend_expected_valid);
    TEST_ASSERT_EQUAL_UINT32(1,result.backend_expected_index);TEST_ASSERT_EQUAL_UINT32(1,result.adapter_index);
    TEST_ASSERT_EQUAL_HEX32(port.source.luid_low,result.backend_expected_luid_low);
    /* No-export ordering places the display adapter first instead. */
    JceWindowAdapterRecord swapped=port.records[0];port.records[0]=port.records[1];port.records[1]=swapped;
    TEST_ASSERT_TRUE(jce_window_observe_display_adapter_with_provider(window(),&provider,&result));
    TEST_ASSERT_EQUAL_UINT32(0,result.backend_expected_index);TEST_ASSERT_EQUAL_UINT32(0,result.adapter_index);
    TEST_ASSERT_EQUAL_UINT32(2,result.key_match_count);
}
static void software_zero_and_wide_keys_cannot_be_sent_to_bgfx(void)
{
    JceWindowAdapterObservation result;
    for(unsigned i=0;i<5;++i){
        port.records[1].software=(i==0);port.records[1].vendor_id=i==1 ? 0 : (i==2 ? 0x18086 : 0x8086);
        port.records[1].device_id=i==3 ? 0 : (i==4 ? 0x19a49 : 0x9a49);
        TEST_ASSERT_FALSE(jce_window_observe_display_adapter_with_provider(window(),&provider,&result));
        TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_UNREPRESENTABLE,result.reason);
    }
    TEST_ASSERT_EQUAL_UINT(5,port.finishes);
}
static void failed_begin_adapter_or_verify_always_finish_once(void)
{
    JceWindowAdapterObservation result;port.begin_fail=true;port.begin_reason=JCE_WINDOW_ADAPTER_FACTORY_UNAVAILABLE;
    TEST_ASSERT_FALSE(jce_window_observe_display_adapter_with_provider(window(),&provider,&result));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_FACTORY_UNAVAILABLE,result.reason);TEST_ASSERT_EQUAL_UINT(0,port.adapters);
    port.begin_reason=JCE_WINDOW_ADAPTER_MATCH;
    TEST_ASSERT_FALSE(jce_window_observe_display_adapter_with_provider(window(),&provider,&result));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_ENUMERATION_FAILED,result.reason);
    port.begin_fail=false;port.adapter_fail=true;
    TEST_ASSERT_FALSE(jce_window_observe_display_adapter_with_provider(window(),&provider,&result));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_ENUMERATION_FAILED,result.reason);
    port.adapter_fail=false;port.verify_fail=true;port.verify_reason=JCE_WINDOW_ADAPTER_QUERY_TIMEOUT;
    TEST_ASSERT_FALSE(jce_window_observe_display_adapter_with_provider(window(),&provider,&result));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_QUERY_TIMEOUT,result.reason);
    port.verify_reason=JCE_WINDOW_ADAPTER_MATCH;
    TEST_ASSERT_FALSE(jce_window_observe_display_adapter_with_provider(window(),&provider,&result));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_TOPOLOGY_CHANGED,result.reason);TEST_ASSERT_EQUAL_UINT(5,port.finishes);
}
static void finite_adapter_bounds_refuse_incomplete_observation(void)
{
    JceWindowAdapterObservation result;port.count=JCE_WINDOW_ADAPTER_LIMIT;
    TEST_ASSERT_FALSE(jce_window_observe_display_adapter_with_provider(window(),&provider,&result));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_ENUMERATION_LIMIT,result.reason);
    TEST_ASSERT_EQUAL_UINT(JCE_WINDOW_ADAPTER_LIMIT,port.adapters);TEST_ASSERT_EQUAL_UINT(0,port.verifies);
}
static void invalid_inputs_never_acquire_and_reason_names_are_bounded(void)
{
    JceWindowAdapterObservation result;JceWindowAdapterProvider incomplete=provider;incomplete.verify=NULL;
    TEST_ASSERT_FALSE(jce_window_observe_display_adapter_with_provider(NULL,&provider,&result));
    TEST_ASSERT_FALSE(jce_window_observe_display_adapter_with_provider(window(),NULL,&result));
    TEST_ASSERT_FALSE(jce_window_observe_display_adapter_with_provider(window(),&incomplete,&result));
    TEST_ASSERT_FALSE(jce_window_observe_display_adapter_with_provider(window(),&provider,NULL));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_INVALID_ARGUMENT,result.reason);
    TEST_ASSERT_EQUAL_UINT(0,port.begins);TEST_ASSERT_EQUAL_UINT(0,port.finishes);
    TEST_ASSERT_EQUAL_STRING("ccd_invalid_result",jce_window_adapter_reason_name(JCE_WINDOW_ADAPTER_CCD_INVALID_RESULT));
    TEST_ASSERT_EQUAL_STRING("unknown",jce_window_adapter_reason_name((JceWindowAdapterReason)-1));
}
#if defined(_WIN32)
static void native_failure(JceWindowAdapterReason reason)
{
    JceWindowAdapterObservation result;
    TEST_ASSERT_FALSE(jce_window_observe_display_adapter(window(),&result));
    TEST_ASSERT_EQUAL_INT(reason,result.reason);TEST_ASSERT_EQUAL_UINT(0,result.vendor_id);
    TEST_ASSERT_EQUAL_UINT(0,winport.live);
}
static void actual_windows_ccd_intel_overrules_misleading_nvidia_output_and_releases_once(void)
{
    JceWindowAdapterObservation result;TEST_ASSERT_TRUE(jce_window_observe_display_adapter(window(),&result));
    TEST_ASSERT_EQUAL_HEX16(0x8086,result.vendor_id);TEST_ASSERT_EQUAL_HEX16(0x9a49,result.device_id);
    TEST_ASSERT_EQUAL_HEX32(0xabcdef01,result.luid_low);TEST_ASSERT_EQUAL_INT32(-5,result.luid_high);
    TEST_ASSERT_TRUE(result.source_valid);TEST_ASSERT_EQUAL_HEX32(result.luid_low,result.source_luid_low);
    TEST_ASSERT_EQUAL_INT32(result.luid_high,result.source_luid_high);TEST_ASSERT_EQUAL_UINT32(3,result.source_id);
    TEST_ASSERT_EQUAL_UINT(2,winport.window);TEST_ASSERT_EQUAL_UINT(2,winport.display);
    TEST_ASSERT_EQUAL_UINT(2,winport.monitor_info);TEST_ASSERT_EQUAL_UINT(2,winport.open);
    TEST_ASSERT_EQUAL_UINT(4,winport.symbol);TEST_ASSERT_EQUAL_UINT(1,winport.create);
    TEST_ASSERT_EQUAL_UINT(2,winport.close);TEST_ASSERT_EQUAL_UINT(1,winport.factory_release);
    TEST_ASSERT_EQUAL_UINT(2,winport.adapter_release);TEST_ASSERT_EQUAL_UINT(0,winport.output_calls);
    TEST_ASSERT_EQUAL_UINT(2,winport.flags_queries);TEST_ASSERT_EQUAL_UINT(2,winport.flags_descs);
    TEST_ASSERT_EQUAL_UINT(2,winport.flags_release);
    TEST_ASSERT_EQUAL_UINT(0,winport.output_release);TEST_ASSERT_EQUAL_UINT(2,winport.sizes);
    TEST_ASSERT_EQUAL_UINT(2,winport.queries);TEST_ASSERT_EQUAL_UINT(2,winport.names);
    TEST_ASSERT_EQUAL_UINT(4,winport.allocs);TEST_ASSERT_EQUAL_UINT(4,winport.frees);
}
static void actual_windows_missing_display_monitor_and_ccd_api_refuse_without_fallback_guess(void)
{
    winport.no_window=true;native_failure(JCE_WINDOW_ADAPTER_DISPLAY_UNAVAILABLE);TEST_ASSERT_EQUAL_UINT(0,winport.open);
    winport.no_window=false;winport.no_monitor=true;native_failure(JCE_WINDOW_ADAPTER_MONITOR_UNAVAILABLE);
    winport.no_monitor=false;winport.monitor_failure=true;native_failure(JCE_WINDOW_ADAPTER_MONITOR_UNAVAILABLE);
    winport.monitor_failure=false;winport.monitor_unterminated=true;native_failure(JCE_WINDOW_ADAPTER_MONITOR_UNAVAILABLE);
    winport.monitor_unterminated=false;winport.monitor_empty=true;native_failure(JCE_WINDOW_ADAPTER_MONITOR_UNAVAILABLE);
    winport.monitor_empty=false;winport.no_user_library=true;native_failure(JCE_WINDOW_ADAPTER_CCD_UNAVAILABLE);
    winport.no_user_library=false;winport.no_ccd_symbol=true;native_failure(JCE_WINDOW_ADAPTER_CCD_UNAVAILABLE);
    TEST_ASSERT_EQUAL_UINT(1,winport.close);TEST_ASSERT_EQUAL_UINT(0,winport.create);
}
static void actual_windows_failed_dxgi_acquisition_releases_only_owned_resources(void)
{
    winport.no_library=true;native_failure(JCE_WINDOW_ADAPTER_DXGI_UNAVAILABLE);TEST_ASSERT_EQUAL_UINT(1,winport.close);
    winport.no_library=false;winport.no_symbol=true;native_failure(JCE_WINDOW_ADAPTER_FACTORY_UNAVAILABLE);
    TEST_ASSERT_EQUAL_UINT(3,winport.close);winport.no_symbol=false;winport.factory_failure=true;
    native_failure(JCE_WINDOW_ADAPTER_FACTORY_UNAVAILABLE);TEST_ASSERT_EQUAL_UINT(5,winport.close);
    TEST_ASSERT_EQUAL_UINT(0,winport.factory_release);winport.partial_factory=true;
    native_failure(JCE_WINDOW_ADAPTER_FACTORY_UNAVAILABLE);TEST_ASSERT_EQUAL_UINT(1,winport.factory_release);
    TEST_ASSERT_EQUAL_UINT(7,winport.close);TEST_ASSERT_EQUAL_UINT(0,winport.allocs);
}
static void actual_windows_failed_adapter_enumeration_or_description_cleans_ccd_storage(void)
{
    winport.desc_failure=true;native_failure(JCE_WINDOW_ADAPTER_ENUMERATION_FAILED);
    TEST_ASSERT_EQUAL_UINT(1,winport.adapter_release);TEST_ASSERT_EQUAL_UINT(2,winport.frees);
    winport.desc_failure=false;winport.adapter_result=E_FAIL;native_failure(JCE_WINDOW_ADAPTER_ENUMERATION_FAILED);
    TEST_ASSERT_EQUAL_UINT(1,winport.adapter_release);TEST_ASSERT_EQUAL_UINT(4,winport.frees);
    TEST_ASSERT_EQUAL_UINT(2,winport.factory_release);TEST_ASSERT_EQUAL_UINT(4,winport.close);
}
static void actual_windows_missing_or_failed_software_flags_release_all_acquired_interfaces(void)
{
    winport.flags_failure=true;native_failure(JCE_WINDOW_ADAPTER_ENUMERATION_FAILED);
    TEST_ASSERT_EQUAL_UINT(1,winport.adapter_release);TEST_ASSERT_EQUAL_UINT(0,winport.flags_release);
    winport.flags_failure=false;winport.flags_desc_failure=true;native_failure(JCE_WINDOW_ADAPTER_ENUMERATION_FAILED);
    TEST_ASSERT_EQUAL_UINT(2,winport.adapter_release);TEST_ASSERT_EQUAL_UINT(1,winport.flags_release);
    TEST_ASSERT_EQUAL_UINT(4,winport.frees);TEST_ASSERT_EQUAL_UINT(4,winport.close);
    winport.flags_desc_failure=false;winport.flags_failure=true;winport.partial_flags=true;
    native_failure(JCE_WINDOW_ADAPTER_ENUMERATION_FAILED);
    TEST_ASSERT_EQUAL_UINT(3,winport.adapter_release);TEST_ASSERT_EQUAL_UINT(2,winport.flags_release);
    TEST_ASSERT_EQUAL_UINT(6,winport.frees);TEST_ASSERT_EQUAL_UINT(6,winport.close);
}
static void actual_windows_duplicate_key_first_source_and_later_source_follow_backend_contract(void)
{
    port.count=3;port.records[2]=port.records[1];port.records[2].luid_low=0x29360;
    JceWindowAdapterObservation result;TEST_ASSERT_TRUE(jce_window_observe_display_adapter(window(),&result));
    TEST_ASSERT_TRUE(result.source_valid);TEST_ASSERT_EQUAL_UINT32(2,result.key_match_count);
    TEST_ASSERT_EQUAL_UINT32(1,result.backend_expected_index);TEST_ASSERT_EQUAL_UINT32(1,result.adapter_index);
    TEST_ASSERT_EQUAL_HEX32(port.source.luid_low,result.backend_expected_luid_low);
    port.source.luid_low=port.records[2].luid_low;
    TEST_ASSERT_FALSE(jce_window_observe_display_adapter(window(),&result));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_BACKEND_KEY_MISMATCH,result.reason);
    TEST_ASSERT_TRUE(result.source_valid);TEST_ASSERT_EQUAL_HEX32(0x29360,result.source_luid_low);
    TEST_ASSERT_EQUAL_UINT32(1,result.backend_expected_index);
    TEST_ASSERT_EQUAL_HEX32(port.records[1].luid_low,result.backend_expected_luid_low);
    TEST_ASSERT_EQUAL_UINT32(0,result.vendor_id);TEST_ASSERT_EQUAL_UINT32(0,result.device_id);
}
static void actual_windows_ccd_api_failures_counts_and_empty_paths_refuse(void)
{
    winport.sizes_result=ERROR_ACCESS_DENIED;native_failure(JCE_WINDOW_ADAPTER_CCD_QUERY_FAILED);
    TEST_ASSERT_EQUAL_UINT(0,winport.allocs);winport.sizes_result=0;winport.query_result=ERROR_NOT_SUPPORTED;
    native_failure(JCE_WINDOW_ADAPTER_CCD_QUERY_FAILED);winport.query_result=0;winport.return_over_paths=true;
    native_failure(JCE_WINDOW_ADAPTER_CCD_INVALID_RESULT);winport.return_over_paths=false;winport.return_over_modes=true;
    native_failure(JCE_WINDOW_ADAPTER_CCD_INVALID_RESULT);winport.return_over_modes=false;
    winport.path_capacity=winport.path_count=winport.mode_capacity=winport.mode_count=0;
    native_failure(JCE_WINDOW_ADAPTER_NO_MATCH);
}
static void actual_windows_ccd_capacity_bounds_refuse_before_allocation(void)
{
    winport.path_capacity=JCE_WINDOW_ADAPTER_PATH_LIMIT+1;native_failure(JCE_WINDOW_ADAPTER_CCD_LIMIT);
    winport.path_capacity=1;winport.mode_capacity=JCE_WINDOW_ADAPTER_MODE_LIMIT+1;native_failure(JCE_WINDOW_ADAPTER_CCD_LIMIT);
    TEST_ASSERT_EQUAL_UINT(0,winport.allocs);TEST_ASSERT_EQUAL_UINT(0,winport.queries);
}
static void actual_windows_ccd_insufficient_buffer_retries_are_finite_and_healthy_retry_succeeds(void)
{
    JceWindowAdapterObservation result;winport.insufficient=2;
    TEST_ASSERT_TRUE(jce_window_observe_display_adapter(window(),&result));
    TEST_ASSERT_EQUAL_UINT(4,winport.sizes);TEST_ASSERT_EQUAL_UINT(4,winport.queries);
    TEST_ASSERT_EQUAL_UINT(8,winport.allocs);TEST_ASSERT_EQUAL_UINT(8,winport.frees);
    winport.insufficient=winport.queries+JCE_WINDOW_ADAPTER_QUERY_ATTEMPTS;
    unsigned before=winport.queries;native_failure(JCE_WINDOW_ADAPTER_TOPOLOGY_CHANGED);
    TEST_ASSERT_EQUAL_UINT(JCE_WINDOW_ADAPTER_QUERY_ATTEMPTS,winport.queries-before);
}
static void actual_windows_allocation_failures_have_single_release_of_each_acquired_buffer(void)
{
    winport.allocation_fail_at=1;native_failure(JCE_WINDOW_ADAPTER_ALLOCATION_FAILED);
    TEST_ASSERT_EQUAL_UINT(2,winport.allocs);TEST_ASSERT_EQUAL_UINT(1,winport.frees);
    winport.allocation_fail_at=4;native_failure(JCE_WINDOW_ADAPTER_ALLOCATION_FAILED);
    TEST_ASSERT_EQUAL_UINT(4,winport.allocs);TEST_ASSERT_EQUAL_UINT(2,winport.frees);
}
static void actual_windows_name_packet_headers_and_strings_are_strict(void)
{
    for(unsigned bad=1;bad<=7;++bad){winport.name_bad=bad;native_failure(JCE_WINDOW_ADAPTER_CCD_INVALID_RESULT);}
    winport.name_bad=0;winport.name_failure=true;native_failure(JCE_WINDOW_ADAPTER_CCD_QUERY_FAILED);
    winport.name_failure=false;winport.no_matching_name=true;native_failure(JCE_WINDOW_ADAPTER_NO_MATCH);
    TEST_ASSERT_EQUAL_UINT(18,winport.frees);
}
static void actual_windows_distinct_named_sources_are_ambiguous_but_clone_source_is_unique(void)
{
    JceWindowAdapterObservation result;winport.path_capacity=winport.path_count=2;winport.ambiguous=true;
    TEST_ASSERT_FALSE(jce_window_observe_display_adapter(window(),&result));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_CCD_SOURCE_AMBIGUOUS,result.reason);
    TEST_ASSERT_TRUE(result.source_valid);TEST_ASSERT_EQUAL_HEX32(port.source.luid_low,result.source_luid_low);
    TEST_ASSERT_EQUAL_UINT32(3,result.source_id);TEST_ASSERT_EQUAL_UINT32(2,result.path_count);
    TEST_ASSERT_EQUAL_UINT32(0,result.adapter_count);TEST_ASSERT_EQUAL_UINT32(0,result.vendor_id);
    winport.ambiguous=false;winport.clone=true;
    TEST_ASSERT_TRUE(jce_window_observe_display_adapter(window(),&result));TEST_ASSERT_EQUAL_HEX16(0x8086,result.vendor_id);
    TEST_ASSERT_EQUAL_UINT(0,winport.output_calls);
}
static void actual_windows_inactive_unavailable_and_virtual_paths_never_claim_physical_match(void)
{
    winport.inactive=true;native_failure(JCE_WINDOW_ADAPTER_TOPOLOGY_CHANGED);winport.inactive=false;
    winport.unavailable=true;native_failure(JCE_WINDOW_ADAPTER_TOPOLOGY_CHANGED);winport.unavailable=false;
    const DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY values[]={DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST,
        DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED,DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL,
        DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER};
    for(unsigned i=0;i<sizeof values/sizeof values[0];++i){winport.technology=values[i];native_failure(JCE_WINDOW_ADAPTER_CCD_UNSUPPORTED_PATH);}
}
static void actual_windows_topology_source_path_and_window_changes_are_rejected(void)
{
    winport.change_query=2;native_failure(JCE_WINDOW_ADAPTER_TOPOLOGY_CHANGED);
    winport.change_query=winport.queries+2;winport.raw_change=true;native_failure(JCE_WINDOW_ADAPTER_TOPOLOGY_CHANGED);
    winport.change_query=0;winport.change_monitor=winport.monitor_info+2;native_failure(JCE_WINDOW_ADAPTER_TOPOLOGY_CHANGED);
    winport.change_monitor=0;winport.change_name=winport.names+2;native_failure(JCE_WINDOW_ADAPTER_NO_MATCH);
}
static void actual_windows_observed_deadline_and_clock_regression_refuse_further_work(void)
{
    winport.clock_step=JCE_WINDOW_ADAPTER_QUERY_MS+1;native_failure(JCE_WINDOW_ADAPTER_QUERY_TIMEOUT);
    TEST_ASSERT_EQUAL_UINT(0,winport.sizes);TEST_ASSERT_EQUAL_UINT(0,winport.allocs);
    winport.clock=1000;winport.clock_step=1;winport.clock_calls=0;winport.backwards_clock=true;
    native_failure(JCE_WINDOW_ADAPTER_QUERY_TIMEOUT);TEST_ASSERT_EQUAL_UINT(0,winport.sizes);
}
#endif

#if defined(JCE_WINDOW_HAS_DXGI_FACTORY6)
static void assert_performance_cleanup(unsigned ranked_descriptors, unsigned classic_adapters)
{
    TEST_ASSERT_EQUAL_UINT(1, rankport.queries);
    TEST_ASSERT_EQUAL_UINT(rankport.unavailable ? 0 : 1, rankport.factory_releases);
    TEST_ASSERT_EQUAL_UINT(ranked_descriptors, rankport.descs);
    TEST_ASSERT_EQUAL_UINT(ranked_descriptors, rankport.adapter_releases);
    TEST_ASSERT_EQUAL_UINT(classic_adapters, winport.adapter_release);
    TEST_ASSERT_EQUAL_UINT(classic_adapters, winport.flags_release);
    TEST_ASSERT_EQUAL_UINT(1, winport.factory_release);
    TEST_ASSERT_EQUAL_UINT(1, winport.close);
    TEST_ASSERT_EQUAL_UINT(0, winport.window);
    TEST_ASSERT_EQUAL_UINT(0, winport.queries);
    TEST_ASSERT_EQUAL_UINT(0, winport.allocs);
    TEST_ASSERT_FALSE(rankport.adapter_live);
}

static void actual_windows_performance_rank_uses_classic_index_for_non_display_gpu(void)
{
    JceWindowAdapterRecord discrete = port.records[0];
    port.records[0] = port.records[1];
    port.records[1] = discrete;
    rankport.order[0] = 1;
    rankport.order[1] = 0;
    JceWindowAdapterObservation result;
    TEST_ASSERT_TRUE(jce_window_observe_performance_adapter(window(), &result));
    TEST_ASSERT_EQUAL_HEX16(0x10de, result.vendor_id);
    TEST_ASSERT_EQUAL_HEX16(0x2507, result.device_id);
    TEST_ASSERT_TRUE(result.source_valid);
    TEST_ASSERT_EQUAL_HEX32(discrete.luid_low, result.source_luid_low);
    TEST_ASSERT_EQUAL_INT32(discrete.luid_high, result.source_luid_high);
    TEST_ASSERT_EQUAL_UINT32(0, result.source_id);
    TEST_ASSERT_EQUAL_UINT32(1, result.adapter_index);
    TEST_ASSERT_EQUAL_UINT32(0, result.path_count);
    TEST_ASSERT_EQUAL_UINT32(0, result.mode_count);
    TEST_ASSERT_EQUAL_UINT(2, rankport.current);
    TEST_ASSERT_EQUAL_UINT(2, rankport.passes);
    assert_performance_cleanup(2, 2);
}

static void actual_windows_performance_rank_skips_and_releases_software(void)
{
    port.count = 3;
    port.records[2] = (JceWindowAdapterRecord){0x1414, 0x008c, 7, 0, true};
    rankport.count = 3;
    rankport.order[0] = 2;
    rankport.order[1] = 0;
    rankport.order[2] = 1;
    JceWindowAdapterObservation result;
    TEST_ASSERT_TRUE(jce_window_observe_performance_adapter(window(), &result));
    TEST_ASSERT_EQUAL_HEX16(0x10de, result.vendor_id);
    TEST_ASSERT_EQUAL_UINT32(1, result.source_id);
    TEST_ASSERT_EQUAL_UINT32(0, result.adapter_index);
    TEST_ASSERT_EQUAL_UINT(4, rankport.enums);
    assert_performance_cleanup(4, 3);
}

static void actual_windows_absent_factory6_preserves_fallback_and_releases_classic_factory(void)
{
    rankport.unavailable = true;
    JceWindowAdapterObservation result;
    TEST_ASSERT_FALSE(jce_window_observe_performance_adapter(window(), &result));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_PERFORMANCE_UNAVAILABLE, result.reason);
    TEST_ASSERT_FALSE(result.source_valid);
    TEST_ASSERT_EQUAL_UINT(0, rankport.enums);
    TEST_ASSERT_EQUAL_UINT(0, rankport.factory_releases);
    assert_performance_cleanup(0, 0);

    rankport.partial_factory = true;
    TEST_ASSERT_FALSE(jce_window_observe_performance_adapter(window(), &result));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_PERFORMANCE_UNAVAILABLE, result.reason);
    TEST_ASSERT_EQUAL_UINT(2, rankport.queries);
    TEST_ASSERT_EQUAL_UINT(1, rankport.factory_releases);
    TEST_ASSERT_EQUAL_UINT(2, winport.factory_release);
    TEST_ASSERT_EQUAL_UINT(2, winport.close);
    TEST_ASSERT_EQUAL_UINT(0, rankport.enums);
}

static void actual_windows_performance_rank_refuses_unselectable_classic_key(void)
{
    port.records[1].vendor_id = port.records[0].vendor_id;
    port.records[1].device_id = port.records[0].device_id;
    rankport.order[0] = 1;
    rankport.order[1] = 0;
    JceWindowAdapterObservation result;
    TEST_ASSERT_FALSE(jce_window_observe_performance_adapter(window(), &result));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_BACKEND_KEY_MISMATCH, result.reason);
    TEST_ASSERT_EQUAL_HEX32(port.records[1].luid_low, result.source_luid_low);
    TEST_ASSERT_TRUE(result.backend_expected_valid);
    TEST_ASSERT_EQUAL_HEX32(port.records[0].luid_low, result.backend_expected_luid_low);
    TEST_ASSERT_EQUAL_UINT32(2, result.key_match_count);
    TEST_ASSERT_EQUAL_UINT(1, rankport.factory_releases);
    assert_performance_cleanup(2, 2);
}

static void actual_windows_performance_rank_change_refuses_stale_selection(void)
{
    rankport.next_first = 1;
    JceWindowAdapterObservation result;
    TEST_ASSERT_FALSE(jce_window_observe_performance_adapter(window(), &result));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_TOPOLOGY_CHANGED, result.reason);
    TEST_ASSERT_TRUE(result.source_valid);
    TEST_ASSERT_EQUAL_HEX32(port.records[0].luid_low, result.source_luid_low);
    TEST_ASSERT_FALSE(result.backend_expected_valid);
    TEST_ASSERT_EQUAL_UINT(2, rankport.passes);
    TEST_ASSERT_EQUAL_UINT(1, rankport.factory_releases);
    assert_performance_cleanup(2, 2);
}
#endif

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(ccd_source_luid_selects_unique_full_identity_and_finishes_once);
    RUN_TEST(absent_and_duplicate_luid_preserve_original_policy);
    RUN_TEST(duplicate_vendor_device_on_another_luid_is_not_selectable_by_bgfx);
    RUN_TEST(first_matching_key_is_the_ccd_source_despite_later_distinct_luid);
    RUN_TEST(software_zero_and_wide_keys_cannot_be_sent_to_bgfx);
    RUN_TEST(failed_begin_adapter_or_verify_always_finish_once);
    RUN_TEST(finite_adapter_bounds_refuse_incomplete_observation);
    RUN_TEST(invalid_inputs_never_acquire_and_reason_names_are_bounded);
#if defined(_WIN32)
    RUN_TEST(actual_windows_ccd_intel_overrules_misleading_nvidia_output_and_releases_once);
    RUN_TEST(actual_windows_missing_display_monitor_and_ccd_api_refuse_without_fallback_guess);
    RUN_TEST(actual_windows_failed_dxgi_acquisition_releases_only_owned_resources);
    RUN_TEST(actual_windows_failed_adapter_enumeration_or_description_cleans_ccd_storage);
    RUN_TEST(actual_windows_missing_or_failed_software_flags_release_all_acquired_interfaces);
    RUN_TEST(actual_windows_duplicate_key_first_source_and_later_source_follow_backend_contract);
    RUN_TEST(actual_windows_ccd_api_failures_counts_and_empty_paths_refuse);
    RUN_TEST(actual_windows_ccd_capacity_bounds_refuse_before_allocation);
    RUN_TEST(actual_windows_ccd_insufficient_buffer_retries_are_finite_and_healthy_retry_succeeds);
    RUN_TEST(actual_windows_allocation_failures_have_single_release_of_each_acquired_buffer);
    RUN_TEST(actual_windows_name_packet_headers_and_strings_are_strict);
    RUN_TEST(actual_windows_distinct_named_sources_are_ambiguous_but_clone_source_is_unique);
    RUN_TEST(actual_windows_inactive_unavailable_and_virtual_paths_never_claim_physical_match);
    RUN_TEST(actual_windows_topology_source_path_and_window_changes_are_rejected);
    RUN_TEST(actual_windows_observed_deadline_and_clock_regression_refuse_further_work);
#endif
#if defined(JCE_WINDOW_HAS_DXGI_FACTORY6)
    RUN_TEST(actual_windows_performance_rank_uses_classic_index_for_non_display_gpu);
    RUN_TEST(actual_windows_performance_rank_skips_and_releases_software);
    RUN_TEST(actual_windows_absent_factory6_preserves_fallback_and_releases_classic_factory);
    RUN_TEST(actual_windows_performance_rank_refuses_unselectable_classic_key);
    RUN_TEST(actual_windows_performance_rank_change_refuses_stale_selection);
#endif
    return UNITY_END();
}
