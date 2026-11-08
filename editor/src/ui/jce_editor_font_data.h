/* Private font-source ownership; ImGui 1.92 retains input for atlas lifetime. */
#ifndef JCE_EDITOR_FONT_DATA_H
#define JCE_EDITOR_FONT_DATA_H

#include "core/jce_editor_alloc.h"
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <climits>
#include <cstdint>
#include <cstring>

/* Host IO and ED/ImGui default allocation share tracked mimalloc ownership.
 * Reject sizes before ImGui's signed-int interface and pinned >100-byte
 * admission precondition; consume raw on all paths. */
static inline void *ed_font_adopt_host_buffer(void *raw, uint64_t size)
{
    if (!raw || size <= 100 || size > INT_MAX) {
        if (raw) jce_free(raw);
        return nullptr;
    }
    /* No second allocation: both host IO and ED/ImGui default frees use
     * the same tracked mimalloc heap. The sentinel byte stays owned too. */
    return raw;
}

static inline void *ed_font_read_host(const char *path, size_t *out_size)
{
    if (out_size) *out_size = 0;
    uint64_t size = 0;
    void *raw = jce_fs_host_read_all(path, &size);
    void *data = ed_font_adopt_host_buffer(raw, size);
    if (data && out_size) *out_size = static_cast<size_t>(size);
    return data;
}

/* The callback must be the owning AddFont operation
 * (FontDataOwnedByAtlas=true): invocation consumes data on EVERY return path.
 * ImGui 1.92.6 registers the source before initialization and its NULL rollback
 * already frees owned input. Successful input lives until Clear/destruction.
 * The caller relinquishes ownership at invocation; NULL is admission failure,
 * not a request for caller cleanup. Typed ports must model this same contract. */
template <typename AddFont>
static inline auto ed_font_transfer(void *data, int size, AddFont add)
    -> decltype(add(data, size))
{
    return add(data, size);
}

/* Only recoverable user errors from the owning admission call are allowed to
 * return NULL. Ordinary/internal ImGui assertions stay enabled. The caller
 * passes its real IO flag; restore its exact previous value on every return.
 * This is not validation of arbitrary truncated or malicious font tables. */
template <typename AddFont>
static inline auto ed_font_transfer_recoverable(void *data, int size,
                                               bool &assert_enabled, AddFont add)
    -> decltype(add(data, size))
{
    struct RestoreAssert {
        bool &enabled;
        bool previous;
        ~RestoreAssert() { enabled = previous; }
    } restore{assert_enabled, assert_enabled};
    assert_enabled = false;
    return ed_font_transfer(data, size, add);
}
#endif
