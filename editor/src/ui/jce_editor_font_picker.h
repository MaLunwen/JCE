/* Shared editor font picker; host discovery and font ownership stay in style/platform. */
#ifndef JCE_EDITOR_FONT_PICKER_H
#define JCE_EDITOR_FONT_PICKER_H
#include "jce_editor_style.h"
#include <cstddef>
bool jce_editor_draw_font_picker(const char *label, char *path, size_t capacity,
    const JceFontEntry *entries, int count, const char *automatic_label);
#endif
