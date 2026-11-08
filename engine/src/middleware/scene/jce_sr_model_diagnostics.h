/* Private model-cache admission diagnostics; no resource ownership. */
#ifndef JCE_SR_MODEL_DIAGNOSTICS_H
#define JCE_SR_MODEL_DIAGNOSTICS_H
#include <stdbool.h>
#include <stdint.h>
struct JceSceneRenderer;
typedef enum JceSrModelFailure {
    SR_MODEL_FAILURE_LOAD = 1,
    SR_MODEL_FAILURE_ENTRY_BUDGET,
    SR_MODEL_FAILURE_NO_ARCHIVE,
    SR_MODEL_FAILURE_JOB_ALLOCATION,
    SR_MODEL_FAILURE_TASK_ADMISSION,
    SR_MODEL_FAILURE_DECODE,
    SR_MODEL_FAILURE_UPLOAD,
    SR_MODEL_FAILURE_PATH_LENGTH,
    SR_MODEL_FAILURE_ENTRY_ALLOCATION
} JceSrModelFailure;
/* Fixed per-reason observations, not a model/path registry. */
enum { SR_MODEL_FAILURE_REASONS = SR_MODEL_FAILURE_ENTRY_ALLOCATION };
typedef struct SrModelFailureCounts {
    uint64_t total, pending;
    uint32_t first_entity, last_entity, first_path_hash, last_path_hash;
    bool saturated;
} SrModelFailureCounts;
typedef struct SrModelFailureIdentity {
    const void *scene;
    uint64_t context, cache_generation;
    uint32_t frame, capture_generation;
    bool bound;
} SrModelFailureIdentity;
typedef struct SrModelDiagnostics {
    SrModelFailureIdentity current, batch;
    SrModelFailureCounts reasons[SR_MODEL_FAILURE_REASONS];
    uint32_t detailed_reasons;
    bool batch_valid;
} SrModelDiagnostics;
typedef enum SrModelDiagnosticsFlush {
    SR_MODEL_DIAGNOSTICS_ROUND = 1,
    SR_MODEL_DIAGNOSTICS_IDENTITY,
    SR_MODEL_DIAGNOSTICS_RESET,
    SR_MODEL_DIAGNOSTICS_DESTROY,
    SR_MODEL_DIAGNOSTICS_CAPTURE
} SrModelDiagnosticsFlush;

void sr_model_cache_failure_hash(struct JceSceneRenderer *sr, uint32_t path_hash,
    uint32_t entity, JceSrModelFailure reason);
void sr_model_cache_diagnostics_bind(struct JceSceneRenderer *sr,
    const void *scene, uint64_t context, uint32_t backend_frame);
/* False retains unpublished counts. A caller retiring the owner must report it. */
bool sr_model_cache_diagnostics_flush(struct JceSceneRenderer *sr,
    SrModelDiagnosticsFlush reason);
void sr_model_cache_failure(struct JceSceneRenderer *sr, const char *path,
    uint32_t entity, JceSrModelFailure reason);
void sr_model_cache_diagnostics_reset(struct JceSceneRenderer *sr);
#endif
