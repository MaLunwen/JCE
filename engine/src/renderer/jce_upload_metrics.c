#include "jce_upload_metrics.h"
#include <string.h>

void jce_upload_metrics_reset(JceSubmittedPayloadStats *stats, uint64_t generation)
{
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    stats->context_generation = generation;
    stats->valid = generation != 0;
}

void jce_upload_metrics_add(JceSubmittedPayloadStats *stats,
    JceUploadPayloadKind kind, uint64_t bytes, bool admitted)
{
    uint64_t *counter;
    if (!stats || !stats->valid || !admitted || !bytes) return;
    switch (kind) {
    case JCE_UPLOAD_STATIC_VERTEX: counter = &stats->static_vertex_bytes; break;
    case JCE_UPLOAD_STATIC_INDEX: counter = &stats->static_index_bytes; break;
    case JCE_UPLOAD_TEXTURE_CREATE: counter = &stats->texture_create_bytes; break;
    case JCE_UPLOAD_TEXTURE_UPDATE: counter = &stats->texture_update_bytes; break;
    default: return;
    }
    if (bytes > UINT64_MAX - *counter) {
        *counter = UINT64_MAX;
        stats->saturated = true;
    } else {
        *counter += bytes;
    }
}

bool jce_upload_metrics_bind(JceUploadMetricsBridge *bridge,
    JceSubmittedPayloadStats *stats)
{
    if (!bridge || !stats || bridge->active || bridge->generation == UINT64_MAX)
        return false;
    jce_upload_metrics_reset(stats, ++bridge->generation);
    bridge->active = stats;
    return true;
}

void jce_upload_metrics_unbind(JceUploadMetricsBridge *bridge,
    JceSubmittedPayloadStats *stats)
{
    if (!bridge || !stats || bridge->active != stats) return;
    bridge->active = NULL;
    stats->valid = false;
}
