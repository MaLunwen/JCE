/* Private retention owner for RmlUi's borrowed font spans. No font parser. */
#ifndef JCE_RML_FONT_BYTES_H
#define JCE_RML_FONT_BYTES_H
#include <stdbool.h>
#include <stddef.h>
#include <jce/os/core/jce_defs.h>
JCE_EXTERN_C_BEGIN
typedef void (*JceRmlFontReleaseFn)(void *bytes);
typedef bool (*JceRmlFontAdmitFn)(const void *bytes, size_t size, void *user);
typedef struct JceRmlFontBuffer JceRmlFontBuffer;
typedef struct JceRmlFontBytes {
    JceRmlFontBuffer *head;
    size_t count;
    size_t bytes;
} JceRmlFontBytes;
typedef enum JceRmlFontBytesResult {
    JCE_RML_FONT_BYTES_OK,
    JCE_RML_FONT_BYTES_INVALID,
    JCE_RML_FONT_BYTES_REGISTRY_OOM,
    JCE_RML_FONT_BYTES_REJECTED
} JceRmlFontBytesResult;
/* Takes source ownership on every path. Allocate and register retention before
 * the real font engine is allowed to borrow. A rejected face is retired once. */
JceRmlFontBytesResult jce_rml_font_bytes_admit(JceRmlFontBytes *owner,
    void *bytes, size_t size, JceRmlFontReleaseFn release,
    JceRmlFontAdmitFn admit, void *user);
/* Caller MUST have completed Rml::Shutdown before clearing accepted spans. */
void jce_rml_font_bytes_clear(JceRmlFontBytes *owner);
JCE_EXTERN_C_END
#endif
