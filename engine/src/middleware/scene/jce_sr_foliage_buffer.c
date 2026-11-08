/* Static foliage roots-buffer policy and transactional ownership. */
#include "jce_sr_foliage_buffer.h"

bool sr_foliage_persist_enabled(const char *value, bool compute_cull)
{
    return compute_cull || !value || !value[0] || value[0] != '0';
}

void sr_foliage_buffer_destroy(uint16_t *handle, uint32_t *resident_hash,
    uint32_t *resident_count, bool *resident_compute_read,
    const JceSrFoliageBufferProvider *provider)
{
    if (*resident_count && *handle != UINT16_MAX)
        provider->destroy(*handle, provider->user);
    *handle = UINT16_MAX;
    *resident_hash = 0u;
    *resident_count = 0u;
    *resident_compute_read = false;
}

bool sr_foliage_buffer_ensure(uint16_t *handle, uint32_t *resident_hash,
    uint32_t *resident_count, bool *resident_compute_read, const void *roots,
    uint32_t count, uint32_t hash, bool compute_read,
    const JceSrFoliageBufferProvider *provider)
{
    if (!count || !roots)
        return false;
    if (*handle != UINT16_MAX && *resident_count == count &&
        *resident_hash == hash && *resident_compute_read == compute_read)
        return true;

    const uint16_t candidate = provider->create(count, compute_read, provider->user);
    if (candidate == UINT16_MAX)
        return false;
    if (!provider->upload(candidate, roots, count, provider->user)) {
        provider->destroy(candidate, provider->user);
        return false;
    }

    /* The prior owner remains live until CPU upload acceptance. On failure it
     * is still owned, but ensure returns false and callers must not bind it as
     * the requested identity. Each replacement temporarily owns at most two. */
    const uint16_t previous = *handle;
    const bool had_previous = *resident_count && previous != UINT16_MAX;
    *handle = candidate;
    *resident_hash = hash;
    *resident_count = count;
    *resident_compute_read = compute_read;
    if (had_previous)
        provider->destroy(previous, provider->user);
    return true;
}
