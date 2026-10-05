/* Minimal RTSP client: RTP over the RTSP TCP connection (interleaved), H.264 video only.
 * Credentials in the URL (rtsp://user:password@host/...) are used for Basic or Digest (MD5)
 * authentication, whichever the camera asks for. */
#ifndef RC_RTSP_H
#define RC_RTSP_H

#include <stddef.h>
#include <stdint.h>

typedef struct rtsp_client rtsp_client;

/* Connects and runs OPTIONS, DESCRIBE, SETUP, PLAY. timeout: seconds for connecting
 * and for every read; waiting is abandoned when *stop becomes nonzero. Returns NULL on
 * failure (logged). */
rtsp_client *rtsp_open(const char *url, double timeout, volatile int *stop);

/* SPS/PPS from the SDP (sprop-parameter-sets) as Annex B, NULL/0 if absent */
const uint8_t *rtsp_sprop(const rtsp_client *c, int *size);
int rtsp_clock_rate(const rtsp_client *c);
int rtsp_payload_type(const rtsp_client *c);

/* Next RTP packet of the video stream (RTCP and other channels are skipped, keepalives
 * are sent as needed). Returns the packet length (*pkt valid until the next call),
 * 0 if *stop became nonzero, or -1 on error/timeout/EOF. */
int rtsp_read_rtp(rtsp_client *c, const uint8_t **pkt, volatile int *stop);

/* the URL with the password replaced by *** (for logs) */
void rtsp_redact(const char *url, char *out, size_t outlen);

/* TEARDOWN (best effort) and close */
void rtsp_close(rtsp_client *c);

#endif
