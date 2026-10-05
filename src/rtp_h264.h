/* RTP H.264 depacketizer (RFC 6184: single NAL unit, STAP-A, FU-A) producing Annex B
 * access units. An access unit ends when the RTP timestamp changes or a packet has the
 * marker bit set (some cameras do not set the marker reliably, others set it on every
 * packet); parameter sets/SEI without a slice are passed on with the next picture. */
#ifndef RC_RTP_H264_H
#define RC_RTP_H264_H

#include <stdint.h>

typedef struct rtp_h264 rtp_h264;

/* au: Annex B data; ts: RTP timestamp unwrapped to 64 bits; key: contains an IDR slice;
 * broken: packets were lost or could not be depacketized. Return <0 to abort. */
typedef int (*rtp_au_cb)(void *opaque, const uint8_t *au, int size, int64_t ts, int key, int broken);

rtp_h264 *rtp_h264_create(int payload_type, rtp_au_cb cb, void *opaque);
/* one RTP packet; returns the callback's error or 0 */
int rtp_h264_push(rtp_h264 *d, const uint8_t *pkt, int len);
/* number of RTP sequence number jumps seen (lost or reordered packets) */
long rtp_h264_seq_gaps(const rtp_h264 *d);
void rtp_h264_free(rtp_h264 *d);

#endif
