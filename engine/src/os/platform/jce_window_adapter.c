/* Window GPU identity only. Device creation remains with bgfx. */
#include "jce_window_adapter_internal.h"
#include <string.h>

const char *jce_window_adapter_reason_name(JceWindowAdapterReason reason)
{
    static const char *names[]={"match","invalid_argument","unsupported_platform",
        "display_unavailable","monitor_unavailable","dxgi_unavailable","factory_unavailable",
        "enumeration_failed","enumeration_limit","no_match","ambiguous","unrepresentable","backend_limit",
        "ccd_unavailable","ccd_query_failed","ccd_invalid_result","ccd_limit",
        "topology_changed","query_timeout","allocation_failed","ccd_unsupported_path",
        "ccd_source_ambiguous","dxgi_luid_ambiguous","backend_key_mismatch",
        "performance_unavailable","performance_no_hardware"};
    return (unsigned)reason < sizeof names/sizeof names[0] ? names[reason] : "unknown";
}

bool jce_window_observe_display_adapter_with_provider(JceWindow *window,
    const JceWindowAdapterProvider *provider,JceWindowAdapterObservation *out)
{
    JceWindowAdapterRecord records[JCE_WINDOW_ADAPTER_LIMIT];
    JceWindowDisplaySource source={0};
    const JceWindowAdapterRecord *selected=NULL;
    uint32_t count=0,candidate=UINT32_MAX,matches=0,duplicate=0,first_key=UINT32_MAX;
    bool complete=false;
    if (!out) return false;
    memset(out,0,sizeof *out);out->reason=JCE_WINDOW_ADAPTER_INVALID_ARGUMENT;
    if (!window || !provider || !provider->begin || !provider->adapter ||
        !provider->verify || !provider->finish) return false;
    bool begun=provider->begin(provider->user,window,&source,&out->reason);
    out->source_valid=source.valid;
    out->source_luid_low=source.luid_low;out->source_luid_high=source.luid_high;
    out->source_id=source.source_id;out->path_count=source.path_count;out->mode_count=source.mode_count;
    if (!begun) {
        if(out->reason==JCE_WINDOW_ADAPTER_MATCH) out->reason=JCE_WINDOW_ADAPTER_ENUMERATION_FAILED;
        goto done;
    }
    for(uint32_t i=0;i<JCE_WINDOW_ADAPTER_LIMIT;++i) {
        JceWindowAdapterRecord record={0};
        int status=provider->adapter(provider->user,i,&record);
        if(status==0){complete=true;break;}
        if(status!=1){out->reason=JCE_WINDOW_ADAPTER_ENUMERATION_FAILED;goto done;}
        records[count++]=record;
        if(record.luid_low==source.luid_low && record.luid_high==source.luid_high){candidate=i;++matches;}
        out->adapter_count=count;out->luid_match_count=matches;
    }
    if(!complete){out->reason=JCE_WINDOW_ADAPTER_ENUMERATION_LIMIT;goto done;}
    if(!provider->verify(provider->user,&source,&out->reason)) {
        if(out->reason==JCE_WINDOW_ADAPTER_MATCH) out->reason=JCE_WINDOW_ADAPTER_TOPOLOGY_CHANGED;
        goto done;
    }
    if(!matches){out->reason=JCE_WINDOW_ADAPTER_NO_MATCH;goto done;}
    if(matches!=1){out->reason=JCE_WINDOW_ADAPTER_DXGI_LUID_AMBIGUOUS;goto done;}
    selected=&records[candidate];
    if(selected->software || !selected->vendor_id || !selected->device_id ||
        selected->vendor_id>UINT16_MAX || selected->device_id>UINT16_MAX) {
        out->reason=JCE_WINDOW_ADAPTER_UNREPRESENTABLE;goto done;
    }
    for(uint32_t i=0;i<count;++i)
        if(records[i].vendor_id==selected->vendor_id && records[i].device_id==selected->device_id) {
            if(first_key==UINT32_MAX) first_key=i;
            ++duplicate;
        }
    out->key_match_count=duplicate;
    out->backend_expected_valid=true;out->backend_expected_index=first_key;
    out->backend_expected_luid_low=records[first_key].luid_low;
    out->backend_expected_luid_high=records[first_key].luid_high;
    /* The pinned bgfx classic DXGI owner keeps the first matching key. Distinct
     * later LUIDs are retained, not deduplicated. Only that exact first record
     * can represent the unique CCD source with this backend selection API. */
    if(first_key!=candidate){out->reason=JCE_WINDOW_ADAPTER_BACKEND_KEY_MISMATCH;goto done;}
    out->vendor_id=(uint16_t)selected->vendor_id;out->device_id=(uint16_t)selected->device_id;
    out->luid_low=selected->luid_low;out->luid_high=selected->luid_high;out->adapter_index=candidate;
    out->reason=JCE_WINDOW_ADAPTER_MATCH;
done:
    provider->finish(provider->user);
    return out->reason==JCE_WINDOW_ADAPTER_MATCH;
}

/* CCD declarations require a Windows 7 target. Older Windows retains the
 * renderer's ordinary adapter fallback without guessing a display identity. */
#if defined(_WIN32) && (!defined(WINVER) || WINVER >= 0x0601)
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
#if defined(__IDXGIFactory6_INTERFACE_DEFINED__)
#define JCE_WINDOW_HAS_DXGI_FACTORY6 1
#endif
#include "jce_window_internal.h"
#include <jce/os/platform/jce_library.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_timer.h>
#include <wchar.h>

typedef HRESULT (WINAPI *JceCreateDxgiFactory)(REFIID,void **);
typedef LONG (WINAPI *JceDisplayBufferSizes)(UINT32,UINT32 *,UINT32 *);
typedef LONG (WINAPI *JceDisplayQuery)(UINT32,UINT32 *,DISPLAYCONFIG_PATH_INFO *,
    UINT32 *,DISPLAYCONFIG_MODE_INFO *,DISPLAYCONFIG_TOPOLOGY_ID *);
typedef LONG (WINAPI *JceDisplayDeviceInfo)(DISPLAYCONFIG_DEVICE_INFO_HEADER *);
typedef struct JceWindowDisplaySnapshot {
    DISPLAYCONFIG_PATH_INFO *paths;
    DISPLAYCONFIG_MODE_INFO *modes;
    uint32_t path_count,mode_count,source_id;
    JceWindowDisplaySource source;
} JceWindowDisplaySnapshot;
typedef struct JceWindowDxgiContext {
    JceLibrary library,user_library;
    IDXGIFactory *factory;
    IDXGIAdapter *adapter;
    JceDisplayBufferSizes buffer_sizes;
    JceDisplayQuery query;
    JceDisplayDeviceInfo device_info;
    JceWindow *window;
    uintptr_t monitor;
    WCHAR device[CCHDEVICENAME];
    uint64_t started_ms;
    JceWindowDisplaySnapshot snapshots[2];
} JceWindowDxgiContext;
static bool display_deadline(JceWindowDxgiContext *ctx,JceWindowAdapterReason *reason)
{
    uint64_t now=jce_time_ticks_ms();
    if(now<ctx->started_ms || now-ctx->started_ms>JCE_WINDOW_ADAPTER_QUERY_MS) {
        *reason=JCE_WINDOW_ADAPTER_QUERY_TIMEOUT;return false;
    }
    return true;
}
static bool display_name_valid(const WCHAR *name,size_t capacity)
{
    if(!name[0]) return false;
    for(size_t i=1;i<capacity;++i) if(!name[i]) return true;
    return false;
}
static bool display_monitor(JceWindow *window,uintptr_t *monitor,WCHAR *device,JceWindowAdapterReason *reason)
{
    SDL_Window *native=jce_window_sdl(window);
    SDL_DisplayID display=native ? SDL_GetDisplayForWindow(native) : 0;
    if(!display){*reason=JCE_WINDOW_ADAPTER_DISPLAY_UNAVAILABLE;return false;}
    SDL_PropertiesID properties=SDL_GetDisplayProperties(display);
    *monitor=(uintptr_t)SDL_GetPointerProperty(properties,SDL_PROP_DISPLAY_WINDOWS_HMONITOR_POINTER,NULL);
    if(!*monitor){*reason=JCE_WINDOW_ADAPTER_MONITOR_UNAVAILABLE;return false;}
    MONITORINFOEXW info;memset(&info,0,sizeof info);info.cbSize=sizeof info;
    if(!GetMonitorInfoW((HMONITOR)*monitor,(MONITORINFO *)&info) || info.cbSize!=sizeof info ||
        !display_name_valid(info.szDevice,CCHDEVICENAME)) {
        *reason=JCE_WINDOW_ADAPTER_MONITOR_UNAVAILABLE;return false;
    }
    memcpy(device,info.szDevice,sizeof info.szDevice);return true;
}
static void display_snapshot_release(JceWindowDisplaySnapshot *snapshot)
{
    jce_free(snapshot->paths);jce_free(snapshot->modes);memset(snapshot,0,sizeof *snapshot);
}
/* Conventional display connectors only. Indirect/remote/unknown paths do not
 * establish a physical output identity. Enum values are the Windows CCD ABI. */
static bool display_physical_path(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology)
{
    int64_t value=(int32_t)technology;
    return (value>=0 && value<=6) || (value>=8 && value<=14) || value==18 ||
        value==(int64_t)INT32_MIN;
}
static bool display_source_ccd(JceWindowDxgiContext *ctx,JceWindowDisplaySnapshot *snapshot,
    JceWindowAdapterReason *reason)
{
    bool complete=false;
    for(uint32_t attempt=0;attempt<JCE_WINDOW_ADAPTER_QUERY_ATTEMPTS;++attempt) {
        UINT32 path_capacity=0,mode_capacity=0;
        display_snapshot_release(snapshot);
        if(!display_deadline(ctx,reason)) return false;
        LONG result=ctx->buffer_sizes(QDC_ONLY_ACTIVE_PATHS,&path_capacity,&mode_capacity);
        if(!display_deadline(ctx,reason)) return false;
        if(result!=ERROR_SUCCESS){*reason=JCE_WINDOW_ADAPTER_CCD_QUERY_FAILED;return false;}
        if(path_capacity>JCE_WINDOW_ADAPTER_PATH_LIMIT || mode_capacity>JCE_WINDOW_ADAPTER_MODE_LIMIT) {
            *reason=JCE_WINDOW_ADAPTER_CCD_LIMIT;return false;
        }
        size_t path_bytes=(path_capacity ? path_capacity : 1u)*sizeof *snapshot->paths;
        size_t mode_bytes=(mode_capacity ? mode_capacity : 1u)*sizeof *snapshot->modes;
        snapshot->paths=jce_malloc(path_bytes);snapshot->modes=jce_malloc(mode_bytes);
        if(!snapshot->paths || !snapshot->modes){*reason=JCE_WINDOW_ADAPTER_ALLOCATION_FAILED;return false;}
        memset(snapshot->paths,0,path_bytes);memset(snapshot->modes,0,mode_bytes);
        snapshot->path_count=path_capacity;snapshot->mode_count=mode_capacity;
        result=ctx->query(QDC_ONLY_ACTIVE_PATHS,&snapshot->path_count,snapshot->paths,
            &snapshot->mode_count,snapshot->modes,NULL);
        if(!display_deadline(ctx,reason)) return false;
        if(result==ERROR_INSUFFICIENT_BUFFER) continue;
        if(result!=ERROR_SUCCESS){*reason=JCE_WINDOW_ADAPTER_CCD_QUERY_FAILED;return false;}
        if(snapshot->path_count>path_capacity || snapshot->mode_count>mode_capacity) {
            *reason=JCE_WINDOW_ADAPTER_CCD_INVALID_RESULT;return false;
        }
        complete=true;break;
    }
    if(!complete){*reason=JCE_WINDOW_ADAPTER_TOPOLOGY_CHANGED;return false;}
    snapshot->source.path_count=snapshot->path_count;snapshot->source.mode_count=snapshot->mode_count;
    bool found=false;
    for(uint32_t i=0;i<snapshot->path_count;++i) {
        const DISPLAYCONFIG_PATH_INFO *path=&snapshot->paths[i];
        if(!(path->flags & DISPLAYCONFIG_PATH_ACTIVE) || !path->targetInfo.targetAvailable) {
            *reason=JCE_WINDOW_ADAPTER_TOPOLOGY_CHANGED;return false;
        }
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name;memset(&name,0,sizeof name);
        name.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;name.header.size=sizeof name;
        name.header.adapterId=path->sourceInfo.adapterId;name.header.id=path->sourceInfo.id;
        if(!display_deadline(ctx,reason)) return false;
        LONG result=ctx->device_info(&name.header);
        if(!display_deadline(ctx,reason)) return false;
        if(result!=ERROR_SUCCESS){*reason=JCE_WINDOW_ADAPTER_CCD_QUERY_FAILED;return false;}
        if(name.header.type!=DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME || name.header.size!=sizeof name ||
            name.header.id!=path->sourceInfo.id || name.header.adapterId.LowPart!=path->sourceInfo.adapterId.LowPart ||
            name.header.adapterId.HighPart!=path->sourceInfo.adapterId.HighPart ||
            !display_name_valid(name.viewGdiDeviceName,CCHDEVICENAME)) {
            *reason=JCE_WINDOW_ADAPTER_CCD_INVALID_RESULT;return false;
        }
        if(wcscmp(ctx->device,name.viewGdiDeviceName)) continue;
        if(!display_physical_path(path->targetInfo.outputTechnology)) {
            *reason=JCE_WINDOW_ADAPTER_CCD_UNSUPPORTED_PATH;return false;
        }
        JceWindowDisplaySource source={path->sourceInfo.adapterId.LowPart,path->sourceInfo.adapterId.HighPart,
            true,path->sourceInfo.id,snapshot->path_count,snapshot->mode_count};
        /* Clone paths can repeat the same source. Distinct source identities
         * sharing a GDI name are ambiguous, even when they share an adapter. */
        if(found && (source.luid_low!=snapshot->source.luid_low || source.luid_high!=snapshot->source.luid_high ||
            path->sourceInfo.id!=snapshot->source_id)) {
            *reason=JCE_WINDOW_ADAPTER_CCD_SOURCE_AMBIGUOUS;return false;
        }
        snapshot->source=source;snapshot->source_id=path->sourceInfo.id;found=true;
    }
    if(!found){*reason=JCE_WINDOW_ADAPTER_NO_MATCH;return false;}
    return true;
}
static bool open_classic_factory(JceWindowDxgiContext *ctx, JceWindowAdapterReason *reason)
{
    ctx->library=jce_library_open("dxgi.dll");
    if(!ctx->library){*reason=JCE_WINDOW_ADAPTER_DXGI_UNAVAILABLE;return false;}
    JceCreateDxgiFactory create=(JceCreateDxgiFactory)jce_library_symbol(ctx->library,"CreateDXGIFactory1");
    /* Pinned bgfx uses CreateDXGIFactory1(IID_IDXGIFactory), EnumAdapters and
     * GetDesc. Preserve that exact ordering/descriptor selection contract. */
    static const GUID factory_iid={0x7b7166ec,0x21c7,0x44ae,{0xb2,0x1a,0xc9,0xae,0x32,0x1a,0xe3,0x69}};
    if(!create || FAILED(create(&factory_iid,(void **)&ctx->factory)) || !ctx->factory) {
        *reason=JCE_WINDOW_ADAPTER_FACTORY_UNAVAILABLE;return false;
    }
    return true;
}

static bool display_begin(void *user,JceWindow *window,JceWindowDisplaySource *source,JceWindowAdapterReason *reason)
{
    JceWindowDxgiContext *ctx=user;ctx->window=window;ctx->started_ms=jce_time_ticks_ms();
    if(!display_monitor(window,&ctx->monitor,ctx->device,reason)) return false;
    ctx->user_library=jce_library_open("user32.dll");
    if(!ctx->user_library){*reason=JCE_WINDOW_ADAPTER_CCD_UNAVAILABLE;return false;}
    ctx->buffer_sizes=(JceDisplayBufferSizes)jce_library_symbol(ctx->user_library,"GetDisplayConfigBufferSizes");
    ctx->query=(JceDisplayQuery)jce_library_symbol(ctx->user_library,"QueryDisplayConfig");
    ctx->device_info=(JceDisplayDeviceInfo)jce_library_symbol(ctx->user_library,"DisplayConfigGetDeviceInfo");
    if(!ctx->buffer_sizes || !ctx->query || !ctx->device_info){*reason=JCE_WINDOW_ADAPTER_CCD_UNAVAILABLE;return false;}
    if(!open_classic_factory(ctx,reason)) return false;
    bool observed=display_source_ccd(ctx,&ctx->snapshots[0],reason);
    *source=ctx->snapshots[0].source;return observed;
}
static const GUID adapter1_iid={0x29038f61,0x3839,0x4626,{0x91,0xfd,0x08,0x68,0x79,0x01,0x1a,0x05}};

static int display_adapter(void *user,uint32_t index,JceWindowAdapterRecord *out)
{
    JceWindowDxgiContext *ctx=user;
    if(ctx->adapter){IDXGIAdapter_Release(ctx->adapter);ctx->adapter=NULL;}
    HRESULT result=IDXGIFactory_EnumAdapters(ctx->factory,index,&ctx->adapter);
    if(result==DXGI_ERROR_NOT_FOUND) return 0;
    if(FAILED(result) || !ctx->adapter) return -1;
    DXGI_ADAPTER_DESC desc;
    if(FAILED(IDXGIAdapter_GetDesc(ctx->adapter,&desc))) return -1;
    /* GetDesc1 is used only for the software flag, not a second key/order.
     * Refuse unavailable flags instead of inferring hardware from a label. */
    IDXGIAdapter1 *adapter1=NULL;
    result=IDXGIAdapter_QueryInterface(ctx->adapter,&adapter1_iid,(void **)&adapter1);
    if(FAILED(result) || !adapter1){if(adapter1)IDXGIAdapter1_Release(adapter1);return -1;}
    DXGI_ADAPTER_DESC1 flags;result=IDXGIAdapter1_GetDesc1(adapter1,&flags);
    IDXGIAdapter1_Release(adapter1);
    if(FAILED(result)) return -1;
    out->vendor_id=desc.VendorId;out->device_id=desc.DeviceId;
    out->luid_low=desc.AdapterLuid.LowPart;out->luid_high=desc.AdapterLuid.HighPart;
    out->software=(flags.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)!=0;
    return 1;
}
static bool display_verify(void *user,const JceWindowDisplaySource *source,JceWindowAdapterReason *reason)
{
    JceWindowDxgiContext *ctx=user;uintptr_t monitor=0;WCHAR device[CCHDEVICENAME];
    if(!display_deadline(ctx,reason)) return false;
    if(!display_monitor(ctx->window,&monitor,device,reason)) return false;
    if(monitor!=ctx->monitor || wcscmp(device,ctx->device)) {
        *reason=JCE_WINDOW_ADAPTER_TOPOLOGY_CHANGED;return false;
    }
    if(!display_source_ccd(ctx,&ctx->snapshots[1],reason)) return false;
    const JceWindowDisplaySnapshot *a=&ctx->snapshots[0],*b=&ctx->snapshots[1];
    if(source->luid_low!=b->source.luid_low || source->luid_high!=b->source.luid_high ||
        a->source_id!=b->source_id || a->path_count!=b->path_count || a->mode_count!=b->mode_count ||
        memcmp(a->paths,b->paths,a->path_count*sizeof *a->paths) ||
        memcmp(a->modes,b->modes,a->mode_count*sizeof *a->modes)) {
        *reason=JCE_WINDOW_ADAPTER_TOPOLOGY_CHANGED;return false;
    }
    return display_deadline(ctx,reason);
}
static void display_finish(void *user)
{
    JceWindowDxgiContext *ctx=user;
    if(ctx->adapter) IDXGIAdapter_Release(ctx->adapter);
    if(ctx->factory) IDXGIFactory_Release(ctx->factory);
    if(ctx->library) jce_library_close(ctx->library);
    if(ctx->user_library) jce_library_close(ctx->user_library);
    display_snapshot_release(&ctx->snapshots[0]);display_snapshot_release(&ctx->snapshots[1]);
    memset(ctx,0,sizeof *ctx);
}

#if defined(JCE_WINDOW_HAS_DXGI_FACTORY6)
typedef struct JceWindowPerformanceContext {
    JceWindowDxgiContext classic;
    IDXGIFactory6 *ranked_factory;
    JceWindowAdapterRecord ranked;
    uint32_t rank;
    bool classic_key_changed;
} JceWindowPerformanceContext;

static bool performance_rank(JceWindowPerformanceContext *ctx, JceWindowAdapterRecord *record,
                             uint32_t *rank, JceWindowAdapterReason *reason)
{
    for (uint32_t i = 0; i < JCE_WINDOW_ADAPTER_LIMIT; ++i) {
        if (!display_deadline(&ctx->classic, reason)) return false;
        IDXGIAdapter1 *adapter = NULL;
        HRESULT result = IDXGIFactory6_EnumAdapterByGpuPreference(ctx->ranked_factory, i,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &adapter1_iid, (void **)&adapter);
        DXGI_ADAPTER_DESC1 desc = {0};
        const bool found = SUCCEEDED(result) && adapter;
        HRESULT desc_result = found ? IDXGIAdapter1_GetDesc1(adapter, &desc) : E_FAIL;
        if (adapter) IDXGIAdapter1_Release(adapter);
        if (!display_deadline(&ctx->classic, reason)) return false;
        if (result == DXGI_ERROR_NOT_FOUND) {
            *reason = JCE_WINDOW_ADAPTER_PERFORMANCE_NO_HARDWARE;
            return false;
        }
        if (!found || FAILED(desc_result)) {
            *reason = JCE_WINDOW_ADAPTER_ENUMERATION_FAILED;
            return false;
        }
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        *record = (JceWindowAdapterRecord){desc.VendorId, desc.DeviceId,
            desc.AdapterLuid.LowPart, desc.AdapterLuid.HighPart, false};
        *rank = i;
        return true;
    }
    *reason = JCE_WINDOW_ADAPTER_ENUMERATION_LIMIT;
    return false;
}

static bool performance_begin(void *user, JceWindow *window, JceWindowDisplaySource *source,
                              JceWindowAdapterReason *reason)
{
    JceWindowPerformanceContext *ctx = user;
    (void)window;
    ctx->classic.started_ms = jce_time_ticks_ms();
    if (!open_classic_factory(&ctx->classic, reason) || !display_deadline(&ctx->classic, reason))
        return false;
    static const GUID factory6_iid = {
        0xc1b6694f, 0xff09, 0x44a9, {0xb0, 0x3c, 0x77, 0x90, 0x0a, 0x0a, 0x1d, 0x17}
    };
    HRESULT result = IDXGIFactory_QueryInterface(ctx->classic.factory, &factory6_iid,
        (void **)&ctx->ranked_factory);
    if (!display_deadline(&ctx->classic, reason)) return false;
    if (FAILED(result) || !ctx->ranked_factory) {
        *reason = JCE_WINDOW_ADAPTER_PERFORMANCE_UNAVAILABLE;
        return false;
    }
    if (!IDXGIFactory6_IsCurrent(ctx->ranked_factory)) {
        *reason = JCE_WINDOW_ADAPTER_TOPOLOGY_CHANGED;
        return false;
    }
    if (!performance_rank(ctx, &ctx->ranked, &ctx->rank, reason)) return false;
    *source = (JceWindowDisplaySource){ctx->ranked.luid_low, ctx->ranked.luid_high,
        true, ctx->rank, 0u, 0u};
    return true;
}

static int performance_classic_adapter(void *user, uint32_t index, JceWindowAdapterRecord *record)
{
    JceWindowPerformanceContext *ctx = user;
    int result = display_adapter(&ctx->classic, index, record);
    if (result == 1 && record->luid_low == ctx->ranked.luid_low &&
        record->luid_high == ctx->ranked.luid_high &&
        (record->vendor_id != ctx->ranked.vendor_id || record->device_id != ctx->ranked.device_id))
        ctx->classic_key_changed = true;
    return result;
}

static bool performance_verify(void *user, const JceWindowDisplaySource *source,
                               JceWindowAdapterReason *reason)
{
    JceWindowPerformanceContext *ctx = user;
    JceWindowAdapterRecord current;
    uint32_t rank;
    if (!performance_rank(ctx, &current, &rank, reason)) {
        if (*reason == JCE_WINDOW_ADAPTER_PERFORMANCE_NO_HARDWARE)
            *reason = JCE_WINDOW_ADAPTER_TOPOLOGY_CHANGED;
        return false;
    }
    const bool factory_current = IDXGIFactory6_IsCurrent(ctx->ranked_factory);
    if (!display_deadline(&ctx->classic, reason)) return false;
    if (!factory_current || ctx->classic_key_changed || rank != ctx->rank ||
        current.luid_low != source->luid_low || current.luid_high != source->luid_high ||
        current.vendor_id != ctx->ranked.vendor_id || current.device_id != ctx->ranked.device_id) {
        *reason = JCE_WINDOW_ADAPTER_TOPOLOGY_CHANGED;
        return false;
    }
    return true;
}

static void performance_finish(void *user)
{
    JceWindowPerformanceContext *ctx = user;
    if (ctx->ranked_factory) IDXGIFactory6_Release(ctx->ranked_factory);
    display_finish(&ctx->classic);
}
#endif
#endif

bool jce_window_observe_display_adapter(JceWindow *window,JceWindowAdapterObservation *out)
{
#if defined(_WIN32) && (!defined(WINVER) || WINVER >= 0x0601)
    JceWindowDxgiContext ctx={0};
    const JceWindowAdapterProvider provider={display_begin,display_adapter,display_verify,display_finish,&ctx};
    return jce_window_observe_display_adapter_with_provider(window,&provider,out);
#else
    if(out){
        memset(out,0,sizeof *out);
#if defined(_WIN32)
        out->reason=window ? JCE_WINDOW_ADAPTER_CCD_UNAVAILABLE : JCE_WINDOW_ADAPTER_INVALID_ARGUMENT;
#else
        out->reason=window ? JCE_WINDOW_ADAPTER_UNSUPPORTED_PLATFORM : JCE_WINDOW_ADAPTER_INVALID_ARGUMENT;
#endif
    }
    return false;
#endif
}

bool jce_window_observe_performance_adapter(JceWindow *window, JceWindowAdapterObservation *out)
{
#if defined(JCE_WINDOW_HAS_DXGI_FACTORY6)
    JceWindowPerformanceContext ctx = {0};
    const JceWindowAdapterProvider provider = {
        performance_begin, performance_classic_adapter, performance_verify, performance_finish, &ctx
    };
    return jce_window_observe_display_adapter_with_provider(window, &provider, out);
#else
    if (out) {
        memset(out, 0, sizeof *out);
#if defined(_WIN32)
        out->reason = window ? JCE_WINDOW_ADAPTER_PERFORMANCE_UNAVAILABLE
                             : JCE_WINDOW_ADAPTER_INVALID_ARGUMENT;
#else
        out->reason = window ? JCE_WINDOW_ADAPTER_UNSUPPORTED_PLATFORM
                             : JCE_WINDOW_ADAPTER_INVALID_ARGUMENT;
#endif
    }
    return false;
#endif
}
