/* Private owner-thread offscreen lifetime coordination. */
#ifndef JCE_OFFSCREEN_RETIREMENT_INTERNAL_H
#define JCE_OFFSCREEN_RETIREMENT_INTERNAL_H
#include <stdbool.h>
#include <stdint.h>
typedef struct JceRenderer JceRenderer;
typedef struct JceOffscreenTarget JceOffscreenTarget;
JceOffscreenTarget **jce_renderer_offscreen_head(const JceRenderer *renderer);
/* Read-only session policy, resolved once after successful device admission.
 * Offscreen wrappers copy it; their steady path never reads the environment. */
bool jce_renderer_offscreen_diag_enabled(const JceRenderer *renderer);
uint32_t jce_renderer_offscreen_submission_index(void);
uint32_t jce_renderer_submit_frame(const JceRenderer *renderer);
void jce_offscreen_retirement_collect(const JceRenderer *renderer, uint32_t frame);
void jce_offscreen_retirement_shutdown(const JceRenderer *renderer);
enum { JCE_OFFSCREEN_RETIRE_DELAY = 8 };
static inline bool jce_offscreen_retirement_due(uint32_t now, uint32_t retired)
{
    return (uint32_t)(now - retired) >= JCE_OFFSCREEN_RETIRE_DELAY;
}
#endif
