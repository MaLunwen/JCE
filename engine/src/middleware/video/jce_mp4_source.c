#include "jce_mp4_source.h"
#include "os/core/jce_memory.h"

#define MOOV_MAX_BYTES (32u * 1024u * 1024u)
#define TABLE_MAX_ENTRIES 1000000u
#define BOX(a,b,c,d) ((uint32_t)(a)<<24 | (uint32_t)(b)<<16 | \
                      (uint32_t)(c)<<8 | (uint32_t)(d))

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 |
           (uint32_t)p[2]<<8 | p[3];
}

static uint64_t u64(const uint8_t *p)
{
    return (uint64_t)u32(p)<<32 | u32(p+4);
}

bool jce_mp4_composition_index(const uint64_t *dts, uint32_t count,
                               const uint8_t *ctts, size_t bytes, uint64_t *pts)
{
    uint32_t runs, run, sample = 0u;
    int64_t minimum = INT64_MAX;
    if (!dts || !pts || !ctts || !count || count > TABLE_MAX_ENTRIES ||
        bytes < 8u || ctts[0] > 1u) return false;
    runs = u32(ctts + 4u);
    if (runs > (bytes - 8u) / 8u) return false;
    for (run = 0u; run < runs; ++run) {
        const uint8_t *entry = ctts + 8u + (size_t)run * 8u;
        uint32_t n = u32(entry), k, raw = u32(entry + 4u);
        int64_t offset = ctts[0] ? (int64_t)(int32_t)raw : (int64_t)raw;
        if (n > count - sample) return false;
        for (k = 0u; k < n; ++k, ++sample) {
            int64_t tick;
            if (dts[sample] > (uint64_t)INT64_MAX - UINT32_MAX) return false;
            tick = (int64_t)dts[sample] + offset;
            pts[sample] = (uint64_t)tick;
            if (tick < minimum) minimum = tick;
        }
    }
    if (sample != count) return false;
    for (sample = 0u; sample < count; ++sample) {
        int64_t tick = (int64_t)pts[sample];
        if (minimum < 0 && tick > INT64_MAX + minimum) return false;
        pts[sample] = (uint64_t)(tick - minimum);
    }
    return true;
}

bool jce_mp4_source_box(JceReadSource *source, uint64_t offset,
                        uint32_t *type, uint64_t *body, uint64_t *end)
{
    uint8_t header[16];
    uint64_t total = jce_read_source_size(source), size;
    unsigned bytes = 8u;
    if (offset > total || total-offset < 8u ||
        jce_read_source_read_at(source,offset,header,8u) != 8u) return false;
    size = u32(header);
    if (size == 1u) {
        bytes = 16u;
        if (total-offset < bytes ||
            jce_read_source_read_at(source,offset+8u,header+8u,8u) != 8u)
            return false;
        size = u64(header+8u);
    } else if (!size) size = total-offset;
    if (size < bytes || size > total-offset) return false;
    *type = u32(header+4u);
    *body = offset+bytes;
    *end = offset+size;
    return true;
}

/* Validate counts BEFORE upstream allocations. Payloads remain immutable. */
static bool validate(const uint8_t *data, size_t begin, size_t end,
                     unsigned depth, uint64_t *entries, unsigned *tracks)
{
    size_t pos = begin;
    if (depth > 8u) return false;
    while (pos < end) {
        uint64_t bytes;
        size_t header = 8u, body, next, count_offset = 0u;
        unsigned stride = 0u;
        uint32_t type, count;
        if (end-pos < 8u) return false;
        bytes = u32(data+pos);
        type = u32(data+pos+4u);
        if (bytes == 1u) {
            if (end-pos < 16u) return false;
            header = 16u;
            bytes = u64(data+pos+8u);
        } else if (!bytes) bytes = end-pos;
        if (bytes < header || bytes > end-pos) return false;
        body = pos+header; next = pos+(size_t)bytes;
        if (type == BOX('m','o','o','v') || type == BOX('t','r','a','k') ||
            type == BOX('m','d','i','a') || type == BOX('m','i','n','f') ||
            type == BOX('s','t','b','l') || type == BOX('m','v','e','x')) {
            if (type == BOX('t','r','a','k') && ++*tracks > 32u) return false;
            if (!validate(data,body,next,depth+1u,entries,tracks)) return false;
        }
        if (type == BOX('s','t','t','s') || type == BOX('c','t','t','s')) stride=8u;
        else if (type == BOX('s','t','s','c')) stride=12u;
        else if (type == BOX('s','t','c','o') || type == BOX('s','t','s','s')) stride=4u;
        else if (type == BOX('c','o','6','4')) stride=8u;
        if (stride) count_offset=4u;
        if (type == BOX('s','t','s','z')) {
            if (next-body < 12u) return false;
            count_offset=8u;
            stride=u32(data+body+4u) ? 0u : 4u;
        }
        if (count_offset) {
            if (next-body < count_offset+4u) return false;
            count=u32(data+body+count_offset);
            if (count > TABLE_MAX_ENTRIES ||
                (stride && count > (next-body-count_offset-4u)/stride)) return false;
            if (type == BOX('s','t','t','s') || type == BOX('c','t','t','s')) {
                uint64_t samples=0u;
                uint32_t i;
                for (i=0u;i<count;++i) {
                    samples+=u32(data+body+8u+(size_t)i*8u);
                    if (samples>TABLE_MAX_ENTRIES) return false;
                }
            }
            *entries += count;
            if (*entries > 2u*TABLE_MAX_ENTRIES) return false;
        }
        pos=next;
    }
    return pos==end;
}

uint8_t *jce_mp4_source_moov(JceReadSource *source, uint64_t *offset,
                            size_t *size)
{
    uint64_t pos=0u, body, end, entries=0u;
    unsigned tracks=0u;
    uint32_t type;
    while (pos < jce_read_source_size(source)) {
        uint8_t *data;
        if (!jce_mp4_source_box(source,pos,&type,&body,&end)) return NULL;
        if (type != BOX('m','o','o','v')) { pos=end; continue; }
        if (end-pos > MOOV_MAX_BYTES) return NULL;
        *size=(size_t)(end-pos); *offset=pos;
        data=JCE_MALLOC(*size);
        if (!data) return NULL;
        if (jce_read_source_read_at(source,pos,data,*size) != *size ||
            !validate(data,0u,*size,0u,&entries,&tracks)) {
            JCE_FREE(data);
            return NULL;
        }
        return data;
    }
    return NULL;
}
