/* Private timestamped AV1 packet pipeline. Each pending packet owns its bytes until consumed. */
#ifndef JCE_AV1_PACKET_H
#define JCE_AV1_PACKET_H
#include <jce/middleware/video/jce_av1_decode.h>
JCE_EXTERN_C_BEGIN
typedef struct JceAv1PacketFrame {
    const uint8_t *y, *u, *v;
    ptrdiff_t y_stride, uv_stride;
    uint32_t width, height;
    int64_t timestamp;
} JceAv1PacketFrame;
/* ERROR=-1, WAIT=0 (receive, then retry the SAME input; receive may also WAIT), READY=1. */
JceAv1Decoder *jce_av1_packet_open_parallel(void);
int jce_av1_packet_send(JceAv1Decoder *dec, const void *packet,
                       size_t size, int64_t timestamp);
int jce_av1_packet_receive(JceAv1Decoder *dec, JceAv1PacketFrame *frame);
JCE_EXTERN_C_END
#endif
