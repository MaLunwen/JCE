#include "jce_rml_font_bytes.h"
#include <jce/os/core/jce_alloc.h>
#include <stdint.h>
struct JceRmlFontBuffer {
    JceRmlFontBuffer *next;
    void *bytes;
    size_t size;
    JceRmlFontReleaseFn release;
};
JceRmlFontBytesResult jce_rml_font_bytes_admit(JceRmlFontBytes *owner,
    void *bytes, size_t size, JceRmlFontReleaseFn release,
    JceRmlFontAdmitFn admit, void *user)
{
    if (!release) release = jce_free;
    if (!owner || !bytes || !size || !admit || owner->bytes > SIZE_MAX - size) {
        if (bytes) release(bytes);
        return JCE_RML_FONT_BYTES_INVALID;
    }
    JceRmlFontBuffer *record = (JceRmlFontBuffer *)jce_malloc(sizeof(*record));
    if (!record) {
        release(bytes);
        return JCE_RML_FONT_BYTES_REGISTRY_OOM;
    }
    record->next = owner->head; record->bytes = bytes;
    record->size = size; record->release = release;
    owner->head = record; owner->count++; owner->bytes += size;
    if (!admit(bytes, size, user)) {
        owner->head = record->next; owner->count--; owner->bytes -= size;
        release(bytes); jce_free(record);
        return JCE_RML_FONT_BYTES_REJECTED;
    }
    return JCE_RML_FONT_BYTES_OK;
}
void jce_rml_font_bytes_clear(JceRmlFontBytes *owner)
{
    if (!owner) return;
    while (owner->head) {
        JceRmlFontBuffer *record = owner->head;
        owner->head = record->next;
        record->release(record->bytes); jce_free(record);
    }
    owner->count = 0; owner->bytes = 0;
}
