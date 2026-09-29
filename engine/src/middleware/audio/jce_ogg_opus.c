#include "jce_ogg_opus.h"
#include "os/core/jce_memory.h"
#include <ogg/ogg.h>
#include <opus.h>
#include <string.h>

struct JceOggOpus {
    JceReadSource *source;
    ogg_sync_state sync;
    ogg_stream_state stream;
    OpusDecoder *opus;
    bool stream_ready;
    int serial;
    uint32_t channels, pre_skip, skip;
    uint64_t offset, cursor, length;
    int16_t pcm[5760u*2u];
    uint32_t pcm_pos, pcm_frames;
};

static bool next_packet(JceOggOpus *decoder,ogg_packet *packet)
{
    for (;;) {
        ogg_page page;
        int result;
        if (decoder->stream_ready) {
            result=ogg_stream_packetout(&decoder->stream,packet);
            if (result==1) return packet->bytes>0 && packet->bytes<=1024*1024;
            if (result<0) return false;
        }
        result=ogg_sync_pageout(&decoder->sync,&page);
        if (result<0) continue; /* CRC/resynchronization remains in libogg. */
        if (result==1) {
            if (!decoder->stream_ready) {
                decoder->serial=ogg_page_serialno(&page);
                if (ogg_stream_init(&decoder->stream,decoder->serial)) return false;
                decoder->stream_ready=true;
            }
            /* Chained logical streams require a new format; fail closed. */
            if (ogg_page_serialno(&page)!=decoder->serial ||
                decoder->stream.body_fill>1024*1024 ||
                ogg_stream_pagein(&decoder->stream,&page)) return false;
            continue;
        }
        {
            char *buffer=ogg_sync_buffer(&decoder->sync,4096);
            size_t got;
            if (!buffer) return false;
            got=jce_read_source_read_at(decoder->source,decoder->offset,buffer,4096u);
            if (!got) return false;
            decoder->offset+=got;
            if (ogg_sync_wrote(&decoder->sync,(long)got)) return false;
        }
    }
}

static bool read_headers(JceOggOpus *decoder)
{
    ogg_packet packet;
    int error=0,gain;
    if (!next_packet(decoder,&packet) || packet.bytes<19 ||
        memcmp(packet.packet,"OpusHead",8u) || packet.packet[8]>15u ||
        packet.packet[18]!=0u) return false;
    decoder->channels=packet.packet[9];
    decoder->pre_skip=(uint32_t)packet.packet[10] | (uint32_t)packet.packet[11]<<8;
    decoder->skip=decoder->pre_skip;
    gain=(int)(int16_t)((uint16_t)packet.packet[16] | (uint16_t)packet.packet[17]<<8);
    if (!decoder->channels || decoder->channels>2u) return false;
    decoder->opus=opus_decoder_create(48000,(int)decoder->channels,&error);
    if (!decoder->opus || error!=OPUS_OK) return false;
    if (opus_decoder_ctl(decoder->opus,OPUS_SET_GAIN(gain))!=OPUS_OK) return false;
    return next_packet(decoder,&packet) && packet.bytes>=8 &&
        memcmp(packet.packet,"OpusTags",8u)==0;
}

static uint64_t tail_length(JceOggOpus *decoder)
{
    ogg_sync_state sync;
    ogg_page page;
    uint64_t size=jce_read_source_size(decoder->source),offset;
    size_t bytes=(size_t)(size<65536u?size:65536u);
    ogg_int64_t granule=-1;
    char *buffer;
    if (ogg_sync_init(&sync)) return 0u;
    buffer=ogg_sync_buffer(&sync,(long)bytes);
    offset=size-bytes;
    if (buffer && jce_read_source_read_at(decoder->source,offset,buffer,bytes)==bytes &&
        !ogg_sync_wrote(&sync,(long)bytes)) {
        for (;;) {
            int result=ogg_sync_pageout(&sync,&page);
            if (!result) break;
            if (result>0 && ogg_page_eos(&page) &&
                ogg_page_serialno(&page)==decoder->serial) granule=ogg_page_granulepos(&page);
        }
    }
    ogg_sync_clear(&sync);
    return granule>(ogg_int64_t)decoder->pre_skip ? (uint64_t)granule-decoder->pre_skip : 0u;
}

JceOggOpus *jce_ogg_opus_open(JceReadSource *source)
{
    JceOggOpus *decoder;
    uint8_t magic[4];
    if (jce_read_source_read_at(source,0u,magic,4u)!=4u ||
        memcmp(magic,"OggS",4u)) return NULL;
    decoder=JCE_CALLOC(1u,sizeof(*decoder));
    if (!decoder) return NULL;
    decoder->source=jce_read_source_acquire(source);
    if (ogg_sync_init(&decoder->sync) || !read_headers(decoder)) {
        jce_ogg_opus_close(decoder);
        return NULL;
    }
    decoder->length=tail_length(decoder);
    if (!decoder->length) { jce_ogg_opus_close(decoder); return NULL; }
    return decoder;
}

void jce_ogg_opus_close(JceOggOpus *decoder)
{
    if (!decoder) return;
    if (decoder->stream_ready) ogg_stream_clear(&decoder->stream);
    ogg_sync_clear(&decoder->sync);
    if (decoder->opus) opus_decoder_destroy(decoder->opus);
    jce_read_source_close(decoder->source);
    JCE_FREE(decoder);
}

uint32_t jce_ogg_opus_read(JceOggOpus *decoder,int16_t *out,uint32_t frames)
{
    uint32_t done=0u;
    if (!decoder || !out) return 0u;
    while (done<frames && decoder->cursor<decoder->length) {
        uint32_t count;
        if (decoder->pcm_pos==decoder->pcm_frames) {
            ogg_packet packet;
            int decoded;
            if (!next_packet(decoder,&packet)) break;
            decoded=opus_decode(decoder->opus,packet.packet,(opus_int32)packet.bytes,
                                decoder->pcm,5760,0);
            if (decoded<=0) break;
            decoder->pcm_frames=(uint32_t)decoded;
            decoder->pcm_pos=decoder->skip<decoder->pcm_frames?decoder->skip:decoder->pcm_frames;
            decoder->skip-=decoder->pcm_pos;
            if (decoder->pcm_pos==decoder->pcm_frames) continue;
        }
        count=decoder->pcm_frames-decoder->pcm_pos;
        if (count>frames-done) count=frames-done;
        if (count>decoder->length-decoder->cursor) count=(uint32_t)(decoder->length-decoder->cursor);
        memcpy(out+(size_t)done*decoder->channels,
               decoder->pcm+(size_t)decoder->pcm_pos*decoder->channels,
               (size_t)count*decoder->channels*sizeof(int16_t));
        decoder->pcm_pos+=count; decoder->cursor+=count; done+=count;
    }
    return done;
}

bool jce_ogg_opus_seek(JceOggOpus *decoder,uint64_t frame)
{
    int16_t discard[1024u*2u];
    if (!decoder) return false;
    if (frame>decoder->length) frame=decoder->length;
    if (frame<decoder->cursor) {
        ogg_stream_clear(&decoder->stream); decoder->stream_ready=false;
        ogg_sync_reset(&decoder->sync);
        opus_decoder_destroy(decoder->opus); decoder->opus=NULL;
        decoder->offset=decoder->cursor=0u;
        decoder->pcm_pos=decoder->pcm_frames=0u;
        if (!read_headers(decoder)) return false;
    }
    while (decoder->cursor<frame) {
        uint32_t count=frame-decoder->cursor<1024u?(uint32_t)(frame-decoder->cursor):1024u;
        if (jce_ogg_opus_read(decoder,discard,count)!=count) return false;
    }
    return true;
}

uint32_t jce_ogg_opus_channels(const JceOggOpus *decoder) { return decoder?decoder->channels:0u; }
uint64_t jce_ogg_opus_length(const JceOggOpus *decoder) { return decoder?decoder->length:0u; }
uint64_t jce_ogg_opus_cursor(const JceOggOpus *decoder) { return decoder?decoder->cursor:0u; }
