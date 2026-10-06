/* RTP AAC depacketizer (RFC 3640, mpeg4-generic, AAC-hbr / AAC-lbr): AU headers with
 * sizelength/indexlength/indexdeltalength bits, then the raw AAC frames. An AU split over
 * several packets (one AU header, larger than the packet) is put together again. */
#ifndef RC_RTP_AAC_H
#define RC_RTP_AAC_H

#include <stdint.h>

typedef struct rtp_aac rtp_aac;

/* one raw AAC frame (access unit); ts: RTP timestamp (sample clock) unwrapped to 64 bits */
typedef int (*rtp_aac_cb)(void *opaque, const uint8_t *frame, int size, int64_t ts);

/* frame_len: samples per frame (1024 for AAC-LC), for several AUs in one packet */
rtp_aac *rtp_aac_create(int size_length, int index_length, int index_delta_length, int frame_len,
                        rtp_aac_cb cb, void *opaque);
/* one RTP packet; returns the callback's error or 0 */
int rtp_aac_push(rtp_aac *d, const uint8_t *pkt, int len);
void rtp_aac_free(rtp_aac *d);

#endif
