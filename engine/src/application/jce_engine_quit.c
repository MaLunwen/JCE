#include "jce_engine_quit.h"
#include <jce/os/core/jce_log.h>

static bool requested;
static bool invalid_reported;
static JceAppResult requested_result = JCE_APP_SUCCESS;

void jce_engine_request_quit_with_result(JceAppResult result)
{
    if (result != JCE_APP_SUCCESS && result != JCE_APP_FAILURE) {
        if (!invalid_reported) {
            LOG_WARN("engine_quit", "phase=request_rejected result=%d reason=nonterminal_or_invalid",
                     (int)result);
            invalid_reported = true;
        }
        return;
    }
    requested = true;
    if (result == JCE_APP_FAILURE) requested_result = JCE_APP_FAILURE;
}
void jce_engine_request_quit(void)
{
    jce_engine_request_quit_with_result(JCE_APP_SUCCESS);
}
bool jce_engine_quit_requested(void) { return requested; }
JceAppResult jce_engine_quit_resolve(JceAppResult result)
{
    if (result == JCE_APP_CONTINUE) return result;
    return requested && requested_result == JCE_APP_FAILURE ? JCE_APP_FAILURE : result;
}
void jce_engine_quit_end_session(void)
{
    requested = false;
    requested_result = JCE_APP_SUCCESS;
    invalid_reported = false;
}
