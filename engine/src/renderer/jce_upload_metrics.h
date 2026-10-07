/* Renderer-owned resource payload accounting; API-owner thread only. */
#ifndef JCE_UPLOAD_METRICS_H
#define JCE_UPLOAD_METRICS_H
#include <jce/renderer/jce_renderer.h>
typedef enum JceUploadPayloadKind {
    JCE_UPLOAD_STATIC_VERTEX,
    JCE_UPLOAD_STATIC_INDEX,
    JCE_UPLOAD_TEXTURE_CREATE,
    JCE_UPLOAD_TEXTURE_UPDATE
} JceUploadPayloadKind;
void jce_upload_metrics_reset(JceSubmittedPayloadStats *stats, uint64_t generation);
void jce_upload_metrics_add(JceSubmittedPayloadStats *stats,
    JceUploadPayloadKind kind, uint64_t bytes, bool admitted);
/* State is owned by the existing renderer device context, not this helper. */
typedef struct JceUploadMetricsBridge {
    JceSubmittedPayloadStats *active;
    uint64_t generation;
} JceUploadMetricsBridge;
bool jce_upload_metrics_bind(JceUploadMetricsBridge *bridge,
    JceSubmittedPayloadStats *stats);
void jce_upload_metrics_unbind(JceUploadMetricsBridge *bridge,
    JceSubmittedPayloadStats *stats);
void jce_upload_metrics_record(JceUploadPayloadKind kind, uint64_t bytes,
    bool admitted);
#endif
