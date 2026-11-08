/* Current Editor draw observations. CPU facts, never GPU completion. */
#ifndef JCE_EDITOR_FRAME_OBSERVATION_H
#define JCE_EDITOR_FRAME_OBSERVATION_H
#include <jce/application/jce_engine.h>
#include "jce_editor_viewport_trace.h"

class JceEditorFrameObservation {
    bool enabled_ = false;
    JceFrameViewportObservation value_{};
public:
    void begin(bool enabled) {
        enabled_ = enabled;
        if (!enabled_) return;
        value_ = {};
        value_.struct_size = sizeof(value_);
        value_.source_id = 1; /* Editor-assigned Game source, not an Engine ID. */
        value_.flags = JCE_FRAME_VIEWPORT_IMAGE_COUNT_VALID;
        value_.cpu_outcome = JCE_FRAME_VIEWPORT_CPU_SKIPPED;
        value_.refusal_reason = 1; /* No Game owner visited during this draw. */
    }
    bool enabled() const { return enabled_; }
    bool copy(JceFrameViewportObservation *out) const {
        if (!enabled_ || !out) return false;
        *out = value_;
        return true;
    }
    void policy(JceEditorViewportPolicyReason reason) {
        if (!enabled_) return;
        value_.cpu_outcome = JCE_FRAME_VIEWPORT_CPU_SKIPPED;
        value_.refusal_reason = 100u + static_cast<uint32_t>(reason);
    }
    void request(uint32_t width, uint32_t height) {
        if (!enabled_ || !width || !height) return;
        value_.flags |= JCE_FRAME_VIEWPORT_REQUESTED_SIZE_VALID;
        value_.requested_width = width;
        value_.requested_height = height;
    }
    void render_begin(uint32_t width, uint32_t height) {
        if (!enabled_) return;
        request(width, height);
        value_.cpu_outcome = JCE_FRAME_VIEWPORT_CPU_FAILED;
        value_.refusal_reason = 200u + static_cast<uint32_t>(JceEditorViewportRenderReason::Incomplete);
    }
    void refuse(JceEditorViewportRenderReason reason) {
        if (!enabled_) return;
        const bool failed = reason == JceEditorViewportRenderReason::TargetRefused ||
            reason == JceEditorViewportRenderReason::ResourcesRefused ||
            reason == JceEditorViewportRenderReason::Incomplete;
        value_.cpu_outcome = failed ? JCE_FRAME_VIEWPORT_CPU_FAILED : JCE_FRAME_VIEWPORT_CPU_SKIPPED;
        value_.refusal_reason = 200u + static_cast<uint32_t>(reason);
    }
    void allocation(uint32_t width, uint32_t height) {
        if (!enabled_ || !width || !height) return;
        value_.flags |= JCE_FRAME_VIEWPORT_ALLOCATED_SIZE_VALID;
        value_.allocated_width = width;
        value_.allocated_height = height;
    }
    void prepared(uint32_t width, uint32_t height) {
        if (!enabled_) return;
        value_.flags |= JCE_FRAME_VIEWPORT_PREPARE_SUCCEEDED;
        allocation(width, height);
    }
    void completed() {
        if (!enabled_) return;
        value_.cpu_outcome = JCE_FRAME_VIEWPORT_CPU_COMPLETED;
        value_.refusal_reason = 0;
    }
    void image(uint16_t texture, uint32_t allocated_width, uint32_t allocated_height,
               uint32_t display_width, uint32_t display_height) {
        if (!enabled_ || texture == UINT16_MAX) return;
        value_.flags |= JCE_FRAME_VIEWPORT_TEXTURE_VALID;
        value_.texture_index = texture;
        ++value_.image_count;
        allocation(allocated_width, allocated_height);
        /* The extent qualifies the current texture identity. */
        value_.flags &= ~JCE_FRAME_VIEWPORT_DISPLAY_SIZE_VALID;
        value_.display_width = 0;
        value_.display_height = 0;
        if (display_width && display_height) {
            value_.flags |= JCE_FRAME_VIEWPORT_DISPLAY_SIZE_VALID;
            value_.display_width = display_width;
            value_.display_height = display_height;
        }
    }
};
#endif
