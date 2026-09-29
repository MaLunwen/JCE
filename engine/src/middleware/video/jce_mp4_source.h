#ifndef JCE_MP4_SOURCE_H
#define JCE_MP4_SOURCE_H
#include <jce/os/core/jce_read_source.h>
#include <jce/middleware/video/jce_mp4_parser.h>

JCE_EXTERN_C_BEGIN
/* Decoder packet stamp, distinct from the public sample DTS seek index. */
bool jce_mp4_parser_video_presentation_time(const JceMp4Parser *parser,
                                            uint32_t index, uint64_t *out);

/* Expand immutable CTTS runs into normalized presentation times. */
bool jce_mp4_composition_index(const uint64_t *dts, uint32_t count,
                               const uint8_t *ctts, size_t bytes, uint64_t *pts);
/* Parse one absolute top-level header without reading its payload. */
bool jce_mp4_source_box(JceReadSource *source, uint64_t offset,
                        uint32_t *type, uint64_t *body, uint64_t *end);
/* Bounded metadata only. Caller owns the returned moov box. */
uint8_t *jce_mp4_source_moov(JceReadSource *source, uint64_t *offset,
                            size_t *size);
JCE_EXTERN_C_END
#endif
