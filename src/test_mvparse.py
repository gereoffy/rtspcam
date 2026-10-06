#!/usr/bin/env python3
"""Compare the C motion vector parser (libmvparse) with FFmpeg's export_mvs, frame by frame.

    python src/test_mvparse.py clip.mp4 [clip2.mp4 ...]

The same packets are fed to both: PyAV decodes them with flags2=+export_mvs, the C
library parses them with mvp_decode(). Every vector (w, h, dst_x, dst_y, motion_x,
motion_y) and its order must match exactly. Exit status 1 on any difference.
Also reports the CPU time of both (FFmpeg: single-threaded decode with export_mvs).
"""
import ctypes
import os
import sys
import time

import av
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(HERE, "libmvparse.dylib" if sys.platform == "darwin" else "libmvparse.so")

MV_DT = np.dtype([("dst_x", "<i2"), ("dst_y", "<i2"), ("mx", "<i2"), ("my", "<i2"), ("w", "u1"), ("h", "u1")])
MVP_NONE, MVP_P, MVP_I, MVP_UNSUPPORTED = range(4)


class Frame(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("key", ctypes.c_int), ("errors", ctypes.c_int),
                ("width", ctypes.c_int), ("height", ctypes.c_int),
                ("mb_width", ctypes.c_int), ("mb_height", ctypes.c_int),
                ("n_mv", ctypes.c_int), ("mv", ctypes.c_void_p),
                ("ref_dist", ctypes.c_int), ("poc", ctypes.c_int), ("vcl_bytes", ctypes.c_int)]


class MVParser:
    def __init__(self, extradata=None):
        self.lib = lib = ctypes.CDLL(LIB)
        lib.mvp_create.restype = ctypes.c_void_p
        lib.mvp_free.argtypes = [ctypes.c_void_p]
        lib.mvp_set_extradata.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        lib.mvp_decode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
                                   ctypes.POINTER(Frame)]
        self.ctx = lib.mvp_create()
        self.length_size = 0
        if extradata:
            ex = bytes(extradata)
            if ex[:1] == b"\x01":
                self.length_size = (ex[4] & 3) + 1
            lib.mvp_set_extradata(self.ctx, ex, len(ex))

    def decode(self, data):
        fr = Frame()
        self.lib.mvp_decode(self.ctx, data, len(data), self.length_size, ctypes.byref(fr))
        mv = np.zeros(0, MV_DT)
        if fr.n_mv:
            buf = (ctypes.c_char * (fr.n_mv * MV_DT.itemsize)).from_address(fr.mv)
            mv = np.frombuffer(bytes(buf), MV_DT)
        return fr, mv

    def close(self):
        self.lib.mvp_free(self.ctx)


def ffmpeg_vectors(frame):
    sd = frame.side_data.get("MOTION_VECTORS")
    if sd is None:
        return np.zeros(0, MV_DT)
    a = sd.to_ndarray()
    if len(a) == 0:
        return np.zeros(0, MV_DT)
    assert (a["source"] == -1).all() and (a["motion_scale"] == 4).all()
    out = np.zeros(len(a), MV_DT)
    for k, src in (("dst_x", "dst_x"), ("dst_y", "dst_y"), ("mx", "motion_x"), ("my", "motion_y"),
                   ("w", "w"), ("h", "h")):
        out[k] = a[src]
    return out


def compare(path, max_report=5):
    cont = av.open(path)
    vs = cont.streams.video[0]
    vs.thread_type = "SLICE"
    vs.codec_context.thread_count = 1
    vs.codec_context.options = {"flags2": "+export_mvs"}
    parser = MVParser(vs.codec_context.extradata)
    tb = vs.time_base

    ours = {}          # pts -> (Frame, vectors)
    theirs = {}        # pts -> vectors
    t_c = t_ff = 0.0
    for pkt in cont.demux(vs):
        if pkt.size:
            data = bytes(pkt)
            t = time.process_time()
            fr, mv = parser.decode(data)
            t_c += time.process_time() - t
            ours[pkt.pts] = (fr.type, fr.key, fr.errors, mv)
        t = time.process_time()
        frames = pkt.decode()
        t_ff += time.process_time() - t
        for f in frames:
            theirs[f.pts] = (f.key_frame, ffmpeg_vectors(f))
    parser.close()
    cont.close()

    bad = 0
    n_mv = 0
    for pts, (key, ref) in sorted(theirs.items()):
        if pts not in ours:
            print("  pts %s: missing in mvparse" % pts)
            bad += 1
            continue
        typ, our_key, errors, mv = ours[pts]
        n_mv += len(mv)
        problem = None
        if errors:
            problem = "%d slice errors" % errors
        elif typ == MVP_UNSUPPORTED:
            problem = "unsupported"
        elif bool(our_key) != bool(key):
            problem = "key frame flag %d vs %d" % (our_key, key)
        elif len(mv) != len(ref):
            problem = "%d vectors vs %d" % (len(mv), len(ref))
        elif not np.array_equal(mv, ref):
            diff = np.nonzero(mv != ref)[0]
            problem = "%d vectors differ, first at #%d: ours %s ffmpeg %s" % (
                len(diff), diff[0], mv[diff[0]], ref[diff[0]])
        if problem:
            bad += 1
            if bad <= max_report:
                print("  pts %s: %s" % (pts, problem))
    pts = sorted(theirs)
    dur = float((pts[-1] - pts[0]) * tb) * len(pts) / max(1, len(pts) - 1) if pts else 0.0
    print("%-28s %5d frames %9d vectors %3d mismatching | CPU ffmpeg %6.2f s  mvparse %6.2f s  %4.1fx"
          % (os.path.basename(path), len(theirs), n_mv, bad, t_ff, t_c, t_ff / max(t_c, 1e-9)))
    return bad == 0, t_ff, t_c, dur


def main():
    ok, tf, tc, dur = True, 0.0, 0.0, 0.0
    for p in sys.argv[1:]:
        r = compare(p)
        ok &= r[0]
        tf, tc, dur = tf + r[1], tc + r[2], dur + r[3]
    print("total: %.0f s of video, CPU ffmpeg %.2f s (%.1f%% of a core), mvparse %.2f s (%.1f%%), %.1fx faster%s"
          % (dur, tf, 100 * tf / dur, tc, 100 * tc / dur, tf / max(tc, 1e-9), "" if ok else ", MISMATCHES"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
