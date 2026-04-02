/*
 * jce_file_viewer.h  Multi-tab file viewer.
 */

#ifndef JCE_FILE_VIEWER_H
#define JCE_FILE_VIEWER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* File type detection. */
typedef enum {
    JCE_FILE_TYPE_UNKNOWN = 0,
    JCE_FILE_TYPE_CODE,
    JCE_FILE_TYPE_IMAGE,
    JCE_FILE_TYPE_MODEL,
} JceFileType;

/* Open a file in a new tab (or focus existing tab). */
void jce_file_viewer_open(const char *path);

/* Draw the file viewer content (for tab embedding). */
void jce_file_viewer_draw_content(void);

/* Close all tabs. */
void jce_file_viewer_close_all(void);

/* Detect file type by extension. */
JceFileType jce_file_viewer_detect_type(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* JCE_FILE_VIEWER_H */
