#ifndef JCE_OGG_OPUS_H
#define JCE_OGG_OPUS_H
#include <jce/os/core/jce_read_source.h>
typedef struct JceOggOpus JceOggOpus;
/* Retains the source. One decoder/cursor per consumer. */
JceOggOpus *jce_ogg_opus_open(JceReadSource *source);
void jce_ogg_opus_close(JceOggOpus *decoder);
uint32_t jce_ogg_opus_read(JceOggOpus *decoder,int16_t *out,uint32_t frames);
bool jce_ogg_opus_seek(JceOggOpus *decoder,uint64_t frame);
uint32_t jce_ogg_opus_channels(const JceOggOpus *decoder);
uint64_t jce_ogg_opus_length(const JceOggOpus *decoder);
uint64_t jce_ogg_opus_cursor(const JceOggOpus *decoder);
#endif
