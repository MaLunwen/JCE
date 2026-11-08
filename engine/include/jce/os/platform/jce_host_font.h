/* jce_host_font.h -- platform-owned installed-font selection. */
#ifndef JCE_HOST_FONT_H
#define JCE_HOST_FONT_H
#include <jce/os/core/jce_defs.h>
#include <stdbool.h>
#include <stddef.h>
JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_HOST_FONT_UI_LATIN = 0,
    JCE_HOST_FONT_UI_CJK = 1,
    JCE_HOST_FONT_UI_SYMBOLS = 2
} JceHostFontRole;

/* Latin prefers installed Ink Free Regular, then the platform system UI
 * Sans candidates. CJK uses separate language-aware candidates; Ink Free is not
 * a CJK fallback. Symbols selects a genuinely installed symbol face (Windows
 * Segoe UI Symbol); absence leaves the existing chain usable. No font bytes
 * are redistributed, and role selection does not certify every glyph.
 * Main-thread, startup/resource-change use: Linux may query fc-match with a
 * bounded wait. Never call on a per-frame draw path. False leaves dst empty,
 * including unsupported/locked-down hosts or an insufficient destination. */
JCE_API bool jce_host_default_font(JceHostFontRole role, char *dst, size_t cap);

/* Resolve an installed family or documented font filename stem. Rejects path
 * separators, control characters and quotes; this is not a host-file opener.
 * Ink Free/InkFree select its exact installed Regular face.
 * Segoe Print/SegoePrint/segoepr select Regular; Segoe Print Bold/
 * SegoePrint-Bold/segoeprb select Bold. Print rejects a substituted family or
 * style. Segoe UI Symbol/SegoeUISymbol/seguisym select that installed face.
 * Same thread/capacity/failure contract as jce_host_default_font. */
JCE_API bool jce_host_find_font(const char *family, char *dst, size_t cap);

JCE_EXTERN_C_END
#endif
