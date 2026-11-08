/* Private startup observation: no SDL/COM types or public SDK ABI. */
#ifndef JCE_WINDOW_ADAPTER_INTERNAL_H
#define JCE_WINDOW_ADAPTER_INTERNAL_H
#include <jce/os/platform/jce_window.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define JCE_WINDOW_ADAPTER_LIMIT 16u
#define JCE_WINDOW_ADAPTER_PATH_LIMIT 1024u
#define JCE_WINDOW_ADAPTER_MODE_LIMIT 4096u
#define JCE_WINDOW_ADAPTER_QUERY_ATTEMPTS 4u
#define JCE_WINDOW_ADAPTER_QUERY_MS 250u
typedef enum JceWindowAdapterReason {
    JCE_WINDOW_ADAPTER_MATCH=0,
    JCE_WINDOW_ADAPTER_INVALID_ARGUMENT,
    JCE_WINDOW_ADAPTER_UNSUPPORTED_PLATFORM,
    JCE_WINDOW_ADAPTER_DISPLAY_UNAVAILABLE,
    JCE_WINDOW_ADAPTER_MONITOR_UNAVAILABLE,
    JCE_WINDOW_ADAPTER_DXGI_UNAVAILABLE,
    JCE_WINDOW_ADAPTER_FACTORY_UNAVAILABLE,
    JCE_WINDOW_ADAPTER_ENUMERATION_FAILED,
    JCE_WINDOW_ADAPTER_ENUMERATION_LIMIT,
    JCE_WINDOW_ADAPTER_NO_MATCH,
    JCE_WINDOW_ADAPTER_AMBIGUOUS,
    JCE_WINDOW_ADAPTER_UNREPRESENTABLE,
    JCE_WINDOW_ADAPTER_BACKEND_LIMIT,
    JCE_WINDOW_ADAPTER_CCD_UNAVAILABLE,
    JCE_WINDOW_ADAPTER_CCD_QUERY_FAILED,
    JCE_WINDOW_ADAPTER_CCD_INVALID_RESULT,
    JCE_WINDOW_ADAPTER_CCD_LIMIT,
    JCE_WINDOW_ADAPTER_TOPOLOGY_CHANGED,
    JCE_WINDOW_ADAPTER_QUERY_TIMEOUT,
    JCE_WINDOW_ADAPTER_ALLOCATION_FAILED,
    JCE_WINDOW_ADAPTER_CCD_UNSUPPORTED_PATH,
    JCE_WINDOW_ADAPTER_CCD_SOURCE_AMBIGUOUS,
    JCE_WINDOW_ADAPTER_DXGI_LUID_AMBIGUOUS,
    JCE_WINDOW_ADAPTER_BACKEND_KEY_MISMATCH,
    JCE_WINDOW_ADAPTER_PERFORMANCE_UNAVAILABLE,
    JCE_WINDOW_ADAPTER_PERFORMANCE_NO_HARDWARE
} JceWindowAdapterReason;
typedef struct JceWindowAdapterObservation {
    uint16_t vendor_id,device_id;
    uint32_t luid_low;
    int32_t luid_high;
    uint32_t adapter_index;
    JceWindowAdapterReason reason;
    /* Observed CCD source or ranked DXGI adapter identity. On the ranked
     * path source_id is the preference index and CCD counts are zero. */
    bool source_valid;
    uint32_t source_luid_low;
    int32_t source_luid_high;
    uint32_t source_id,path_count,mode_count;
    uint32_t adapter_count,luid_match_count,key_match_count;
    /* Pinned bgfx first-key-match prediction, never actual GPU ownership. */
    bool backend_expected_valid;
    uint32_t backend_expected_luid_low;
    int32_t backend_expected_luid_high;
    uint32_t backend_expected_index;
} JceWindowAdapterObservation;
typedef struct JceWindowAdapterRecord {
    uint32_t vendor_id,device_id,luid_low;
    int32_t luid_high;
    bool software;
} JceWindowAdapterRecord;
typedef struct JceWindowDisplaySource {
    uint32_t luid_low;
    int32_t luid_high;
    bool valid;
    uint32_t source_id,path_count,mode_count;
} JceWindowDisplaySource;
/* begin acquires startup-only resources; finish is always called, even after
 * failed begin. Enumerators return 1=record, 0=end, -1=failure. No global cache. */
typedef struct JceWindowAdapterProvider {
    bool (*begin)(void *user,JceWindow *window,JceWindowDisplaySource *source,JceWindowAdapterReason *reason);
    int (*adapter)(void *user,uint32_t index,JceWindowAdapterRecord *record);
    bool (*verify)(void *user,const JceWindowDisplaySource *source,JceWindowAdapterReason *reason);
    void (*finish)(void *user);
    void *user;
} JceWindowAdapterProvider;
bool jce_window_observe_display_adapter(JceWindow *window,JceWindowAdapterObservation *out);
bool jce_window_observe_performance_adapter(JceWindow *window, JceWindowAdapterObservation *out);
#if JCE_PLATFORM_WINDOWS && defined(JCE_WINXP_LEGACY) && JCE_WINXP_LEGACY
bool jce_window_legacy_d3d11_available(void);
bool jce_window_legacy_d3d12_available(void);
/* Returns true only when the admitted device reports the software flag. */
bool jce_window_legacy_d3d11_is_software(void *native_device);
#endif
bool jce_window_observe_display_adapter_with_provider(JceWindow *window,
    const JceWindowAdapterProvider *provider,JceWindowAdapterObservation *out);
const char *jce_window_adapter_reason_name(JceWindowAdapterReason reason);
#ifdef __cplusplus
}
#endif
#endif
