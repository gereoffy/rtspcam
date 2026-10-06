/* Live stream output: a Unix domain socket that hands the camera's stream (fragmented MP4,
 * one fragment per picture) to local viewers, e.g. liveview.py. Nothing is done while nobody
 * is connected except keeping the pictures since the last key frame, so that a new viewer
 * starts at once. Slow readers are dropped; the recording never waits for them.
 *
 * Messages on the socket: u8 type (1 = init segment, 2 = fragment of a key frame,
 * 3 = other fragment, 4 = status), u32 big-endian length, then the bytes. A new type 1 message
 * means the stream parameters changed and the receiver has to start over. A status message is
 * not video: 3 bytes, alarm (the detector is triggered), recording (a file is being written) and
 * online (the camera is connected; 0 while it is unreachable, then the pictures are stale);
 * it is sent on every change and every few seconds, and to a new viewer right after the video. */
#ifndef RC_LIVE_H
#define RC_LIVE_H

#include <stddef.h>
#include <stdint.h>

typedef struct live_srv live_srv;

live_srv *live_open(const char *path);
void live_send_init(live_srv *l, const uint8_t *init, size_t len);
void live_send_frame(live_srv *l, const uint8_t *frag, size_t len, int key);
void live_send_status(live_srv *l, int alarm, int recording, int online);
/* accepts new viewers and sends what is pending; call often (every RTP packet): it never blocks */
void live_poll(live_srv *l);
void live_close(live_srv *l);

#endif
