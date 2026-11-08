/* Private snapshot of the one current Emscripten context; not GPU caps. */
#ifndef JCE_WEBGL_CONTEXT_INTERNAL_H
#define JCE_WEBGL_CONTEXT_INTERNAL_H
#include <stdbool.h>
#include <stdint.h>
typedef struct JceWebGlContextSnapshot {
    uint32_t context_id, epoch_begin, epoch_end, webgl_major;
    char api[192], shading[192], renderer[192];
} JceWebGlContextSnapshot;
typedef bool (*JceWebGlContextRead)(JceWebGlContextSnapshot *snapshot);
bool jce_webgl_context_snapshot_internal(JceWebGlContextSnapshot *snapshot);
void jce_webgl_context_cache_reset_internal(void);
#endif
