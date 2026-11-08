/* Private renderer CPU instance uploads. Main-thread reservations; committed
 * tokens may be bound by encoders in the same submission epoch. No GPU fence
 * or physical memory guarantee is implied by the logical capacity budget. */
#ifndef JCE_INSTANCE_UPLOAD_H
#define JCE_INSTANCE_UPLOAD_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define JCE_INSTANCE_UPLOAD_BUDGET_BYTES (128u * 1048576u)
#define JCE_INSTANCE_UPLOAD_RETIRED_MAX 32u
/* Fixed opt-in metadata budget, independent of backend release delay. This
 * accommodates known common multi-view workloads, not every workload. Pool
 * saturation reports unavailable while normal upload ownership stays valid. */
#define JCE_INSTANCE_STAGE_TRACE_LEASE_MAX 256u
#define JCE_INSTANCE_UPLOAD_INVALID UINT16_MAX

typedef struct JceInstanceTransient {
    uint8_t *data;
    uint32_t size, offset, num;
    uint16_t stride, handle;
} JceInstanceTransient;

/* Immutable identity: local serials are scoped by renderer context generation.
 * Offset for a transient token is relative to its own slice; transient_offset
 * additionally names the backend pool slice. No GPU completion is implied. */
typedef struct JceInstanceUploadIdentity {
    uint64_t generation, serial, epoch_serial, allocation_serial;
    uint32_t epoch, requested, count, actual, offset, capacity, transient_offset;
    uint32_t transient_size, transient_num;
    uint16_t transient_stride;
    uint16_t stride, handle;
    bool dynamic;
    uint32_t fallback_reason;
} JceInstanceUploadIdentity;
typedef struct JceInstanceStageTrace JceInstanceStageTrace;
typedef struct JceInstanceStageLease JceInstanceStageLease;

typedef struct JceInstanceUploadProvider {
    uint32_t (*available)(void *, uint32_t, uint16_t);
    bool (*allocate_transient)(void *, uint32_t, uint16_t, JceInstanceTransient *);
    uint16_t (*create_dynamic)(void *, uint32_t, uint16_t);
    void (*destroy_dynamic)(void *, uint16_t);
    void *(*allocate_staging)(void *, uint32_t);
    void (*free_staging)(void *, void *);
    /* On true, staging ownership transfers to the backend; on false it stays
     * with the owner. The callback must not consume it when returning false. */
    bool (*update_dynamic)(void *, uint16_t, uint32_t, void *, uint32_t);
    void (*bind_dynamic)(void *, void *encoder, uint16_t, uint32_t, uint32_t);
    void (*bind_transient)(void *, void *encoder, const JceInstanceTransient *, uint32_t);
    void (*warn_partial)(void *, uint32_t, uint32_t, uint32_t, uint16_t);
    /* Optional trace-only extension. On true, provider transfers the lease
     * before any release callback; on false, lease and staging stay with owner.
     * NULL lease (trace off/metadata OOM) must not change normal upload outcome. */
    bool (*update_dynamic_traced)(void *, uint16_t, uint32_t, void *, uint32_t,
                                   JceInstanceStageLease *);
    void *(*allocate_trace_metadata)(void *, size_t);
    void (*free_trace_metadata)(void *, void *);
} JceInstanceUploadProvider;

typedef enum JceInstanceUploadError {
    JCE_INSTANCE_ERROR_NONE, JCE_INSTANCE_ERROR_REQUEST, JCE_INSTANCE_ERROR_PENDING,
    JCE_INSTANCE_ERROR_CALLER_LIMIT, JCE_INSTANCE_ERROR_AGGREGATE_LIMIT,
    JCE_INSTANCE_ERROR_STAGING, JCE_INSTANCE_ERROR_CREATE, JCE_INSTANCE_ERROR_TRANSIENT,
    JCE_INSTANCE_ERROR_TOKEN, JCE_INSTANCE_ERROR_UPDATE
} JceInstanceUploadError;
typedef struct JceInstanceUploadOwner JceInstanceUploadOwner;
typedef struct JceInstanceUpload {
    JceInstanceUploadOwner *owner;
    JceInstanceTransient transient;
    float *data;
    uint64_t generation, serial, epoch_serial, allocation_serial;
    uint32_t epoch, count, offset, capacity, actual, requested, fallback_reason;
    uint16_t stride, handle;
    bool dynamic, candidate, committed;
} JceInstanceUpload;

typedef struct JceInstanceUploadLane {
    uint32_t capacity, cursor;
    uint16_t handle, stride;
    JceInstanceUploadIdentity identity;
} JceInstanceUploadLane;
typedef struct JceInstanceUploadRetired {
    uint32_t bytes, epoch;
    uint16_t handle;
    JceInstanceUploadIdentity identity;
} JceInstanceUploadRetired;

struct JceInstanceUploadOwner {
    const JceInstanceUploadProvider *provider;
    void *user;
    JceInstanceUploadLane lanes[3];
    JceInstanceUploadRetired retired[JCE_INSTANCE_UPLOAD_RETIRED_MAX];
    JceInstanceUpload pending;
    JceInstanceStageTrace *stage_trace;
    uint64_t stage_unavailable_total, stage_unavailable_reason[6];
    uint64_t generation, serial, epoch_serial;
    uint32_t epoch, owned_bytes, retired_count, warned_epoch, transient_reserve_bytes;
    JceInstanceUploadError last_error;
    bool initialized, epoch_valid, warned, pending_valid;
};

/* context_generation comes from the existing renderer device lifetime owner.
 * It must be nonzero and distinct after destroy/reinitialization. */
void jce_instance_upload_owner_init(JceInstanceUploadOwner *, const JceInstanceUploadProvider *, void *, uint32_t transient_reserve_bytes, uint64_t context_generation);
void jce_instance_upload_owner_destroy(JceInstanceUploadOwner *);
/* The epoch is the renderer's actual submission ordinal, not bgfx/readback
 * frame readiness. Collection requests bgfx's existing deferred destruction
 * after submission; it does not certify hardware completion. */
void jce_instance_upload_owner_advance(JceInstanceUploadOwner *, uint32_t epoch);
/* Supports the qualified 32/64/80-byte shader families. Always rechecks actual
 * transient allocation count and byte size. Allocation discrepancies retry
 * full dynamic admission first; partial admission is explicit as a last resort.
 * An already allocated transient slice cannot be reclaimed until frame edge.
 * caller_budget_mb bounds admission and candidate capacity, clamped to 128.
 * A smaller caller budget trims an unused lane safely; slices already committed
 * in this epoch retain their shared logical charge until submission. This is
 * shared renderer residency, not a private per-particle physical allocation. */
bool jce_instance_upload_begin(JceInstanceUploadOwner *, uint32_t epoch,
                                uint32_t requested, uint16_t stride,
                                uint32_t caller_budget_mb, JceInstanceUpload *);
/* Commit transfers staging exactly once and publishes the cursor/candidate.
 * Commit is main-thread only; actual may be smaller than reserved count. */
bool jce_instance_upload_commit(JceInstanceUploadOwner *, JceInstanceUpload *, uint32_t actual);
bool jce_instance_upload_bind(JceInstanceUploadOwner *, JceInstanceUpload *, uint32_t actual);
/* Read-only binding of an already committed same-epoch token. */
bool jce_instance_upload_bind_encoder(const JceInstanceUploadOwner *, const JceInstanceUpload *,
                                       uint32_t actual, void *encoder);
void jce_instance_upload_discard(JceInstanceUploadOwner *, JceInstanceUpload *);

/* CPU staging trace leases contain no owner pointer. The copied allocator user
 * must remain valid until all leases release (production uses NULL). Metadata
 * OOM emits unavailable and never changes upload acceptance. Gauge covers only
 * instrumented CPU staging, not all heap bytes, GPU residency or completion.
 * Unavailable metadata, origin capture gaps, or core-ring loss preclude a
 * complete resource-correlation claim even if event delivery is complete. */
JceInstanceStageLease *jce_instance_upload_trace_prepare(JceInstanceUploadOwner *,
    const JceInstanceUploadIdentity *, uint32_t bytes);
void jce_instance_upload_trace_transfer(JceInstanceStageLease *);
void jce_instance_upload_trace_release(JceInstanceStageLease *);
void jce_instance_upload_trace_cancel(JceInstanceStageLease *);

typedef struct JceRenderer JceRenderer;
JceInstanceUploadOwner *jce_renderer_instance_upload_owner(const JceRenderer *);
uint32_t jce_renderer_instance_upload_epoch(void);
#endif
