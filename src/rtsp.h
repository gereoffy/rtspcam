/* Minimal RTSP client: RTP over the RTSP TCP connection (interleaved), H.264 video and
 * optionally AAC audio. Credentials in the URL (rtsp://user:password@host/...) are used for
 * Basic or Digest (MD5) authentication, whichever the camera asks for. */
#ifndef RC_RTSP_H
#define RC_RTSP_H

#include <stddef.h>
#include <stdint.h>

typedef struct rtsp_client rtsp_client;

/* the AAC (mpeg4-generic, RFC 3640) audio stream from the SDP */
typedef struct rtsp_audio_info {
    int clock_rate;             /* = sample rate */
    int channels;
    uint8_t config[64];         /* AudioSpecificConfig (fmtp config=) */
    int config_len;
    int size_length, index_length, index_delta_length;     /* AU header layout */
} rtsp_audio_info;

/* Connects and runs OPTIONS, DESCRIBE, SETUP (video; with want_audio also the audio if it is
 * AAC), PLAY. timeout: seconds for connecting and for every read; waiting is abandoned when
 * *stop becomes nonzero. Returns NULL on failure (logged). A missing or unusable audio stream
 * is only logged. */
rtsp_client *rtsp_open(const char *url, double timeout, int want_audio, volatile int *stop);

/* SPS/PPS from the SDP (sprop-parameter-sets) as Annex B, NULL/0 if absent */
const uint8_t *rtsp_sprop(const rtsp_client *c, int *size);
int rtsp_clock_rate(const rtsp_client *c);
int rtsp_payload_type(const rtsp_client *c);

/* the audio stream that was set up, or NULL */
const rtsp_audio_info *rtsp_audio(const rtsp_client *c);

/* Next RTP packet of the video (*audio = 0) or the audio stream (*audio = 1); RTCP and other
 * channels are skipped, keepalives are sent as needed. Returns the packet length (*pkt valid
 * until the next call), 0 if *stop became nonzero, or -1 on error/timeout/EOF (also when no
 * video came for the timeout, even if the connection lives). */
int rtsp_read_rtp(rtsp_client *c, const uint8_t **pkt, int *audio, volatile int *stop);

/* the URL with the password replaced by *** (for logs) */
void rtsp_redact(const char *url, char *out, size_t outlen);

/* TEARDOWN (best effort) and close */
void rtsp_close(rtsp_client *c);

#endif
