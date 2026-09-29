/* jce_test_file_util.h  Shared file helpers for the application test suite.
 *
 * Several runtime tests author a small text asset (a Lua script, a scene JSON,
 * a prefab) on disk and then load it through the REAL runtime.  The
 * open/assert/write/close helper for that was copy-pasted into five of them
 * under two different names (`write_file`, `write_script`).
 *
 * Kept deliberately small — this is a file helper, not a God fixture.  Tests
 * that write BINARY blobs with their own layout (e.g. the collider mesh in
 * test_jce_runtime_collider_resolve.c) keep their own writer: they are not the
 * same function.
 *
 * Include AFTER "unity.h" is available; this header pulls it in itself so the
 * assertion inside the helper resolves.
 */

#ifndef JCE_TEST_FILE_UTIL_H
#define JCE_TEST_FILE_UTIL_H

#include "unity.h"

#include <stdio.h>
#include <string.h>

/* Write `body` verbatim to `path`, failing the test if the file cannot be
 * opened.  Opened in binary mode so the bytes on disk are exactly `body` —
 * text mode would translate newlines on Windows and change file contents the
 * runtime then parses. */
static inline void jce_test_write_file(const char *path, const char *body)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fputs(body, f);
    fclose(f);
}

#endif /* JCE_TEST_FILE_UTIL_H */
