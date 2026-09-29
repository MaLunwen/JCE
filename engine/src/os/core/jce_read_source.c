/* File, memory and callback inputs share one bounded, synchronized cache. */
#include <jce/os/core/jce_read_source.h>
#include <jce/os/core/jce_thread.h>
#include "jce_memory.h"

#include <SDL3/SDL_iostream.h>
#include <string.h>

#define JCE_READ_CACHE_BYTES (64u * 1024u)

struct JceReadSource {
    JceReadSourceDesc desc;
    JceMutex *mutex;
    uint32_t refs;
    uint8_t *cache;
    uint64_t cache_offset;
    size_t cache_size;
    uint64_t bytes_read;
    uint64_t read_calls;
    uint64_t owned_bytes;
};

typedef struct {
    const uint8_t *data;
    size_t size;
    bool owned;
} MemoryInput;

JceReadSource *jce_read_source_create(const JceReadSourceDesc *desc)
{
    JceReadSource *source;
    if (!desc || !desc->read_at || desc->size > INT64_MAX) return NULL;
    source = JCE_CALLOC(1, sizeof(*source));
    if (!source) return NULL;
    source->mutex = jce_mutex_create();
    source->cache = JCE_MALLOC(JCE_READ_CACHE_BYTES);
    if (!source->mutex || !source->cache) {
        jce_mutex_destroy(source->mutex);
        JCE_FREE(source->cache);
        JCE_FREE(source);
        return NULL;
    }
    source->desc = *desc;
    source->refs = 1;
    source->owned_bytes = sizeof(*source) + JCE_READ_CACHE_BYTES;
    return source;
}

static size_t file_read_at(void *user, uint64_t offset, void *out, size_t size)
{
    SDL_IOStream *io = user;
    if (SDL_SeekIO(io, (Sint64)offset, SDL_IO_SEEK_SET) != (Sint64)offset)
        return 0;
    return SDL_ReadIO(io, out, size);
}

static void file_close(void *user) { SDL_CloseIO(user); }

JceReadSource *jce_read_source_open_file(const char *path)
{
    SDL_IOStream *io;
    Sint64 size;
    JceReadSourceDesc desc;
    JceReadSource *source;
    if (!path || !path[0]) return NULL;
    io = SDL_IOFromFile(path, "rb");
    if (!io) return NULL;
    size = SDL_GetIOSize(io);
    if (size < 0) { SDL_CloseIO(io); return NULL; }
    desc.size = (uint64_t)size;
    desc.read_at = file_read_at;
    desc.close = file_close;
    desc.user = io;
    source = jce_read_source_create(&desc);
    if (!source) SDL_CloseIO(io);
    return source;
}

static size_t memory_read_at(void *user, uint64_t offset, void *out, size_t size)
{
    MemoryInput *input = user;
    if (offset > input->size || size > input->size - offset) return 0;
    memcpy(out, input->data + (size_t)offset, size);
    return size;
}

static void memory_close(void *user)
{
    MemoryInput *input = user;
    if (input->owned) JCE_FREE((void *)input->data);
    JCE_FREE(input);
}

JceReadSource *jce_read_source_open_memory(const void *data, size_t size,
                                         bool copy)
{
    MemoryInput *input;
    JceReadSourceDesc desc;
    JceReadSource *source;
    if (!data || !size || (uint64_t)size > INT64_MAX) return NULL;
    input = JCE_CALLOC(1, sizeof(*input));
    if (!input) return NULL;
    input->size = size;
    input->data = data;
    if (copy) {
        void *owned = JCE_MALLOC(size);
        if (!owned) { JCE_FREE(input); return NULL; }
        memcpy(owned, data, size);
        input->data = owned;
        input->owned = true;
    }
    desc.size = size;
    desc.read_at = memory_read_at;
    desc.close = memory_close;
    desc.user = input;
    source = jce_read_source_create(&desc);
    if (!source) memory_close(input);
    else source->owned_bytes += sizeof(*input) + (copy ? size : 0);
    return source;
}

JceReadSource *jce_read_source_acquire(JceReadSource *source)
{
    if (!source) return NULL;
    jce_mutex_lock(source->mutex);
    if (source->refs == UINT32_MAX) {
        jce_mutex_unlock(source->mutex);
        return NULL;
    }
    ++source->refs;
    jce_mutex_unlock(source->mutex);
    return source;
}

void jce_read_source_close(JceReadSource *source)
{
    bool destroy;
    if (!source) return;
    jce_mutex_lock(source->mutex);
    destroy = --source->refs == 0;
    jce_mutex_unlock(source->mutex);
    if (!destroy) return;
    if (source->desc.close) source->desc.close(source->desc.user);
    jce_mutex_destroy(source->mutex);
    JCE_FREE(source->cache);
    JCE_FREE(source);
}

uint64_t jce_read_source_size(const JceReadSource *source)
{
    return source ? source->desc.size : 0;
}

size_t jce_read_source_read_at(JceReadSource *source, uint64_t offset,
                              void *out, size_t capacity)
{
    size_t done = 0;
    uint8_t *dst = out;
    if (!source || !out || offset >= source->desc.size) return 0;
    if ((uint64_t)capacity > source->desc.size - offset)
        capacity = (size_t)(source->desc.size - offset);
    jce_mutex_lock(source->mutex);
    while (done < capacity) {
        size_t cached, count;
        uint64_t pos = offset + done;
        if (pos < source->cache_offset ||
            pos - source->cache_offset >= source->cache_size) {
            uint64_t left = source->desc.size - pos;
            size_t want = left < JCE_READ_CACHE_BYTES
                        ? (size_t)left : JCE_READ_CACHE_BYTES;
            size_t got = source->desc.read_at(source->desc.user, pos,
                                              source->cache, want);
            ++source->read_calls;
            source->cache_size = 0;
            if (!got || got > want) break;
            source->bytes_read += got;
            source->cache_offset = pos;
            source->cache_size = got;
        }
        cached = (size_t)(pos - source->cache_offset);
        count = source->cache_size - cached;
        if (count > capacity - done) count = capacity - done;
        memcpy(dst + done, source->cache + cached, count);
        done += count;
    }
    jce_mutex_unlock(source->mutex);
    return done;
}

bool jce_read_source_get_stats(JceReadSource *source, JceReadSourceStats *out)
{
    if (!source || !out) return false;
    jce_mutex_lock(source->mutex);
    out->size = source->desc.size;
    out->bytes_read = source->bytes_read;
    out->read_calls = source->read_calls;
    out->owned_bytes = source->owned_bytes;
    jce_mutex_unlock(source->mutex);
    return true;
}
