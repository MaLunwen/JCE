#include "jce_adts_file.h"
#include "middleware/video/jce_aac_decode.h"
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_read_source.h>
#include <string.h>

struct JceAdtsFile {
    JceReadSource *source;
    JceAacDecoder *aac;
    uint64_t *offsets;
    uint32_t count, capacity, packet, channels, rate, frame_size;
    uint64_t skip;
    uint8_t encoded[8192];
    uint32_t encoded_pos, encoded_size;
    int16_t pcm[2048u*8u];
    uint32_t pcm_pos, pcm_frames;
};

static uint32_t header_size(const uint8_t *h)
{
    uint32_t bytes;
    if (h[0]!=255u || (h[1]&0xf6u)!=0xf0u || (h[2]&0x3cu)==0x3cu)
        return 0u;
    bytes=((uint32_t)(h[3]&3u)<<11) | (uint32_t)h[4]<<3 | h[5]>>5;
    /* Multiple raw-data blocks need a separate timing index. Reject them. */
    if ((h[6]&3u) || bytes<((h[1]&1u)?7u:9u)) return 0u;
    return bytes;
}

static bool next_pcm(JceAdtsFile *file)
{
    for (;;) {
        uint32_t samples=0u;
        if (jce_aac_adts_decode(file->aac,file->pcm,2048u*8u,&samples)) {
            uint32_t channels=jce_aac_decoder_get_channels(file->aac);
            uint32_t rate=jce_aac_decoder_get_samplerate(file->aac);
            if (!channels || channels>8u || !rate ||
                (file->channels && (file->channels!=channels || file->rate!=rate)))
                return false;
            file->channels=channels; file->rate=rate;
            file->frame_size=jce_aac_decoder_get_frame_size(file->aac);
            file->pcm_pos=0u; file->pcm_frames=samples/channels;
            return file->pcm_frames>0u;
        }
        if (file->encoded_pos==file->encoded_size) {
            uint8_t h[7];
            uint64_t offset;
            if (file->packet>=file->count) return false;
            offset=file->offsets[file->packet++];
            if (jce_read_source_read_at(file->source,offset,h,7u)!=7u) return false;
            file->encoded_size=header_size(h); file->encoded_pos=0u;
            if (!file->encoded_size || jce_read_source_read_at(file->source,offset,
                file->encoded,file->encoded_size)!=file->encoded_size) return false;
        }
        {
            uint32_t consumed=jce_aac_adts_feed(file->aac,file->encoded+file->encoded_pos,
                                                file->encoded_size-file->encoded_pos);
            if (!consumed) return false;
            file->encoded_pos+=consumed;
        }
    }
}

JceAdtsFile *jce_adts_file_open(const char *path)
{
    JceAdtsFile *file=JCE_CALLOC(1u,sizeof(*file));
    uint64_t pos=0u,size;
    if (!file) return NULL;
    file->source=jce_read_source_open_file(path);
    size=jce_read_source_size(file->source);
    if (!file->source || !size) goto fail;
    /* Raw ADTS has no duration table. Scan headers, retaining only offsets. */
    while (pos<size) {
        uint8_t h[7];
        uint32_t bytes;
        if (jce_read_source_read_at(file->source,pos,h,7u)!=7u ||
            !(bytes=header_size(h)) || bytes>size-pos || file->count>=1000000u)
            goto fail;
        if (file->count==file->capacity) {
            uint32_t capacity=file->capacity?file->capacity*2u:256u;
            uint64_t *grown;
            if (capacity>1000000u) capacity=1000000u;
            grown=JCE_REALLOC(file->offsets,(size_t)capacity*sizeof(*grown));
            if (!grown) goto fail;
            file->offsets=grown; file->capacity=capacity;
        }
        file->offsets[file->count++]=pos; pos+=bytes;
    }
    file->aac=jce_aac_decoder_open_adts();
    if (!file->aac || !next_pcm(file)) goto fail;
    return file;
fail:
    jce_adts_file_close(file);
    return NULL;
}

void jce_adts_file_close(JceAdtsFile *file)
{
    if (!file) return;
    jce_aac_decoder_close(file->aac);
    jce_read_source_close(file->source);
    JCE_FREE(file->offsets); JCE_FREE(file);
}

uint32_t jce_adts_file_read(JceAdtsFile *file,int16_t *out,uint32_t frames)
{
    uint32_t done=0u;
    if (!file || !out) return 0u;
    while (done<frames) {
        uint32_t count;
        if (file->pcm_pos==file->pcm_frames && !next_pcm(file)) break;
        count=file->pcm_frames-file->pcm_pos;
        if (file->skip) {
            uint32_t skip=file->skip<count?(uint32_t)file->skip:count;
            file->pcm_pos+=skip; file->skip-=skip;
            continue;
        }
        if (count>frames-done) count=frames-done;
        memcpy(out+(size_t)done*file->channels,file->pcm+(size_t)file->pcm_pos*file->channels,
               (size_t)count*file->channels*sizeof(int16_t));
        done+=count; file->pcm_pos+=count;
    }
    return done;
}

bool jce_adts_file_seek(JceAdtsFile *file,uint64_t frame)
{
    uint64_t length;
    uint32_t packet;
    if (!file || !file->frame_size) return false;
    length=(uint64_t)file->count*file->frame_size;
    if (frame>length) frame=length;
    packet=(uint32_t)(frame/file->frame_size);
    /* Decode two preceding access units to restore AAC overlap state. */
    packet=packet>2u?packet-2u:0u;
    jce_aac_decoder_close(file->aac);
    file->aac=jce_aac_decoder_open_adts();
    file->packet=packet;
    file->encoded_pos=file->encoded_size=0u;
    file->pcm_pos=file->pcm_frames=0u;
    file->skip=frame-(uint64_t)packet*file->frame_size;
    return file->aac!=NULL;
}

void jce_adts_file_format(const JceAdtsFile *file,uint32_t *channels,
                          uint32_t *rate,uint64_t *frames)
{
    if (!file) return;
    if (channels) *channels=file->channels;
    if (rate) *rate=file->rate;
    if (frames) *frames=(uint64_t)file->count*file->frame_size;
}
