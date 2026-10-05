/* Minimal MP4/MOV reader for the first H.264 video track: classic files (moov sample tables)
 * and fragmented ones (moof/trun, as written by rtspcam and FFmpeg's frag_keyframe). */
#ifndef RC_MP4_READER_H
#define RC_MP4_READER_H

#include <stdint.h>

typedef struct mp4_sample {
    uint64_t offset;
    uint32_t size;
    int64_t dts, pts;       /* in the track time scale */
    int key;
} mp4_sample;

typedef struct mp4_file {
    uint32_t timescale;
    uint8_t *avcc;          /* avcC record (decoder configuration), NULL if absent (avc3) */
    int avcc_size;
    int nal_length_size;    /* from avcC, 4 if unknown */
    int width, height;      /* from the sample description */
    mp4_sample *samples;
    int n_samples;
} mp4_file;

/* Reads the index of `path`. Returns 0, or -1 with a message in err. */
int mp4_open_index(const char *path, mp4_file *m, char *err, int errlen);
void mp4_free_index(mp4_file *m);

#endif
