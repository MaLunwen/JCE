/* The complete production legacy observation owner; no SDL/COM/window call. */
#include "os/platform/jce_window_adapter_internal.h"
#include <stdio.h>
#include <jce/os/platform/jce_entropy.h>
#include <string.h>
#include <stdint.h>
#define CHECK(test) do { if (!(test)) { \
    printf("XP_WINDOW_CHECK_FAILED:%d\n", __LINE__); return 1; } } while (0)
int main(void)
{
    unsigned char entropy[16];
    CHECK(jce_host_random_bytes(entropy, sizeof entropy));
    JceWindow *window = (JceWindow *)(uintptr_t)1;
    JceWindowAdapterObservation output, expected;
    CHECK(!jce_window_observe_display_adapter(NULL, NULL));
    CHECK(!jce_window_observe_display_adapter(window, NULL));
    memset(&output, 0xa5, sizeof output);
    memset(&expected, 0, sizeof expected);
    expected.reason = JCE_WINDOW_ADAPTER_INVALID_ARGUMENT;
    CHECK(!jce_window_observe_display_adapter(NULL, &output));
    CHECK(memcmp(&output, &expected, sizeof output) == 0);
    memset(&output, 0xa5, sizeof output);
    expected.reason = JCE_WINDOW_ADAPTER_CCD_UNAVAILABLE;
    CHECK(!jce_window_observe_display_adapter(window, &output));
    CHECK(memcmp(&output, &expected, sizeof output) == 0);
    CHECK(strcmp(jce_window_adapter_reason_name(output.reason), "ccd_unavailable") == 0);
    puts("JCE_XP_WINDOW_ADAPTER_OK");
    return 0;
}
