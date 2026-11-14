/* Private CPU copy used when debug lines exceed the remaining transient space.
 * It preserves whole lines and samples the full queue, never its prefix. */
#ifndef JCE_DEBUG_LINE_SAMPLE_H
#define JCE_DEBUG_LINE_SAMPLE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline void jce_debug_line_sample(void *target, const void *source,
                                        uint32_t source_lines,
                                        uint32_t target_lines,
                                        size_t line_bytes)
{
    if (!target || !source || !source_lines || !target_lines || !line_bytes) return;
    if (target_lines >= source_lines) {
        memcpy(target, source, (size_t)source_lines * line_bytes);
        return;
    }
    for (uint32_t i = 0; i < target_lines; ++i) {
        /* Cover both ends when two or more complete lines fit. With only one
         * slot, use the middle instead of always favouring the first body. */
        const uint32_t at = target_lines == 1 ? source_lines / 2 :
            (uint32_t)((uint64_t)i * (source_lines - 1) / (target_lines - 1));
        memcpy((unsigned char *)target + (size_t)i * line_bytes,
               (const unsigned char *)source + (size_t)at * line_bytes,
               line_bytes);
    }
}

#endif
