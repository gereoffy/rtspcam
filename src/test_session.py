#!/usr/bin/env python3
"""Compare the C recording logic (motion detector, state machine, .mvmap writer) with rtspcam.py.

    python c/test_session.py clip.mp4 [clip2.mp4 ...]

For every clip and every option set in SCENARIOS both implementations process the same
packets: rtspcam.main() runs unmodified except that the MP4 Writer is replaced by a stub
that logs the packets of each recording ("dts key size" per line into <file>.mp4.pkts), the
wall clock is frozen (file names) and every packet gets its own decoded picture (see
_AlignedAV: FFmpeg may delay its output by one picture); the C session (librcsession) gets the packets from
PyAV and logs the same way. Then the output trees must match: the same recordings with the
same packets, and .mvmap files with identical headers and frame records.

Cells: the level of every cell must match, except that when several groups tie for the
largest one, Python and C may pick a different one as "largest" (level 2, or 3 in block
mode): the tie-break order is arbitrary, so such frames are only counted.
"""
import ctypes
import logging
import os
import shutil
import sys
import tempfile
import time

import av
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
import mvmap      # noqa: E402
import rtspcam    # noqa: E402

LIB = os.path.join(HERE, "librcsession.dylib" if sys.platform == "darwin" else "librcsession.so")
WALL0 = 1759500000.0   # frozen session start time (only used for file names)

SCENARIOS = {
    "config": ["--config", os.path.join(ROOT, "configs", "kapu.json")],
    "cluster": ["--mv-min", "5", "--min-cluster", "40", "--ignore", "0,0,1,0.08", "--ignore", "0.8,0.5,1,1"],
    "blocks": ["--mv-min", "2", "--min-blocks", "20", "--pre-roll", "3", "--post-roll", "4"],
    "always": ["--always", "--max-segment", "10", "--mv-min", "5", "--min-cluster", "20"],
    "nomap": ["--no-map", "--mv-min", "3", "--min-cluster", "10", "--max-tail", "1"],
}


class _SideData:
    def __init__(self, arr):
        self.arr = arr

    def to_ndarray(self):
        return self.arr


class _Frame:
    """What MotionDetector reads from an av.VideoFrame."""

    def __init__(self, f):
        self.key_frame = f.key_frame
        self.width, self.height = f.width, f.height
        sd = f.side_data.get("MOTION_VECTORS")
        self.side_data = {"MOTION_VECTORS": _SideData(sd.to_ndarray())} if sd is not None else {}


def predecode(clip):
    """pts -> _Frame for every picture, including the ones the decoder only releases on flush"""
    frames = {}
    cont = av.open(clip)
    vs = cont.streams.video[0]
    vs.thread_type = "SLICE"
    vs.codec_context.thread_count = 1
    vs.codec_context.options = {"flags2": "+export_mvs"}
    for pkt in cont.demux(vs):          # the final empty packet flushes the decoder
        for f in pkt.decode():
            frames[f.pts] = _Frame(f)
    cont.close()
    return frames


class _Packet:
    def __init__(self, pkt, frames):
        self._pkt, self._frames = pkt, frames

    def __getattr__(self, k):
        return getattr(self._pkt, k)

    def decode(self):
        f = self._frames.get(self._pkt.pts)
        return [f] if f is not None else []


class _Container:
    def __init__(self, cont, frames):
        self._cont, self._frames = cont, frames

    def __getattr__(self, k):
        return getattr(self._cont, k)

    def demux(self, *a):
        for pkt in self._cont.demux(*a):
            yield _Packet(pkt, self._frames)


class _AlignedAV:
    """Stands in for the `av` module inside rtspcam: packet.decode() returns the packet's own picture.

    FFmpeg's H.264 decoder starts delaying its output by one picture when the camera skips
    a frame (POC gap -> "Increasing reorder buffer to 1"); rtspcam.py then attaches every
    analysis result to the next packet. The C version has no decoder delay, so the reference
    here is rtspcam.py's logic with the pictures matched to their packets by pts."""

    def __init__(self, frames):
        self.frames = frames
        self.error, self.logging = av.error, av.logging

    def open(self, url, **kw):
        return _Container(av.open(url), self.frames)


class StubWriter:
    """Stands in for rtspcam.Writer: logs the packets instead of muxing them."""

    def __init__(self, path, in_stream, fix=None):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.path = path
        self.tb = in_stream.time_base
        self.f = open(path + ".pkts", "w")
        self.last_ts = None

    def write(self, pkt):
        self.f.write("%d %d %d\n" % (pkt.dts, pkt.is_keyframe, pkt.size))
        self.last_ts = float(pkt.dts * self.tb)

    def close(self):
        self.f.close()
        return self.last_ts or 0.0


def run_python(clip, out, args):
    argv = ["rtspcam.py", clip, "-n", "cam", "-o", out] + args
    real_time, real_writer, real_argv = time.time, rtspcam.Writer, sys.argv
    rtspcam.Writer = StubWriter
    time.time = lambda: WALL0
    sys.argv = argv
    rtspcam.stop_requested = False
    t = time.process_time()
    rtspcam.av = _AlignedAV(predecode(clip))
    try:
        rtspcam.main()
    finally:
        time.time, rtspcam.Writer, sys.argv, rtspcam.av = real_time, real_writer, real_argv, av
    return time.process_time() - t


def run_c(clip, out, args):
    lib = ctypes.CDLL(LIB)
    lib.rct_create.restype = ctypes.c_void_p
    lib.rct_create.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_char_p), ctypes.c_double]
    lib.rct_set_stream.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int]
    lib.rct_packet.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_longlong,
                               ctypes.c_longlong, ctypes.c_int, ctypes.c_int]
    lib.rct_free.argtypes = [ctypes.c_void_p]

    # rtspcam.py analyses every picture: the C default (--max-ref-dist 1) would leave some out
    argv = [a.encode() for a in ["rtspcam", clip, "-n", "cam", "-o", out] + args + ["--max-ref-dist", "0"]]
    arr = (ctypes.c_char_p * len(argv))(*argv)
    lib.rct_quiet(1)
    t = lib.rct_create(len(argv), arr, WALL0)
    if not t:
        raise RuntimeError("rct_create failed")
    cont = av.open(clip)
    vs = cont.streams.video[0]
    ex = bytes(vs.codec_context.extradata or b"")
    ls = (ex[4] & 3) + 1 if ex[:1] == b"\x01" else 0
    lib.rct_set_stream(t, ex, len(ex), vs.time_base.numerator, vs.time_base.denominator, ls)
    cpu = 0.0
    for pkt in cont.demux(vs):
        if pkt.dts is None or pkt.size == 0:
            if pkt.size == 0:
                break
            continue
        data = bytes(pkt)
        c0 = time.process_time()
        lib.rct_packet(t, data, len(data), pkt.dts, pkt.pts if pkt.pts is not None else pkt.dts,
                       1 if pkt.is_keyframe else 0, 0)
        cpu += time.process_time() - c0
    c0 = time.process_time()
    lib.rct_free(t)
    cpu += time.process_time() - c0
    cont.close()
    return cpu


def tree(root):
    out = {}
    for dp, _, files in os.walk(root):
        for fn in files:
            p = os.path.join(dp, fn)
            out[os.path.relpath(p, root)] = p
    return out


def groups_of(cells, gw):
    """8-connected groups of a list of cell indices -> list of sorted cell lists"""
    left, out = set(cells), []
    while left:
        stack = [left.pop()]
        g = []
        while stack:
            c = stack.pop()
            g.append(c)
            y, x = divmod(c, gw)
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    q = (y + dy) * gw + x + dx
                    if 0 <= x + dx < gw and q in left:
                        left.remove(q)
                        stack.append(q)
        out.append(sorted(g))
    return out


def same_cells(ca, cb, gw):
    """True if both cell lists are equal, or differ only in which of several equally large
    groups got the 'largest group' level (Python and C break such ties differently)."""
    a = dict(zip(ca["c"].tolist(), ca["l"].tolist()))
    b = dict(zip(cb["c"].tolist(), cb["l"].tolist()))
    if a == b:
        return True
    if set(a) != set(b):
        return False
    groups = groups_of(list(a), gw)
    biggest = max(len(g) for g in groups)
    la, lb = [], []
    for g in groups:
        ga, gb = {a[c] for c in g}, {b[c] for c in g}
        if len(ga) != 1 or len(gb) != 1:
            return False
        if ga != gb:
            if len(g) != biggest:
                return False
            la.append(ga.pop())
            lb.append(gb.pop())
    return sorted(la) == sorted(lb)


def compare_maps(pa, pb):
    ha, fa = mvmap.read(pa)
    hb, fb = mvmap.read(pb)
    for h in (ha, hb):
        if h["params"].get("ignore") in (None, []):
            h["params"]["ignore"] = None
    hb["params"].pop("max_ref_dist", None)      # C only
    hb["params"].pop("skip_after_key", None)    # C only
    if ha != hb:
        return "header differs:\n    py %s\n    c  %s" % (ha, hb)
    if len(fa) != len(fb):
        return "%d frames vs %d" % (len(fa), len(fb))
    ties = 0
    for b in fb:                       # the C recorder marks key frames also as "not analysed" (16)
        if b["flags"] & 1:
            b["flags"] &= ~16
    for i, (a, b) in enumerate(zip(fa, fb)):
        for k in ("t_ms", "cluster", "blocks", "flags"):
            if a[k] != b[k]:
                return "frame %d: %s %s vs %s" % (i, k, a[k], b[k])
        if not same_cells(a["cells"], b["cells"], ha["gw"]):
            return "frame %d: cells differ (%d vs %d cells)" % (i, len(a["cells"]), len(b["cells"]))
        if not np.array_equal(np.sort(a["cells"], order="c"), np.sort(b["cells"], order="c")):
            ties += 1
    return ties


def compare(clip, name, args, work):
    out_py, out_c = os.path.join(work, "py"), os.path.join(work, "c")
    for d in (out_py, out_c):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    cpu_py = run_python(clip, out_py, args)
    cpu_c = run_c(clip, out_c, args)
    ta, tb = tree(out_py), tree(out_c)
    problems, ties = [], 0
    if sorted(ta) != sorted(tb):
        problems.append("files differ: only python %s, only C %s" % (
            sorted(set(ta) - set(tb)), sorted(set(tb) - set(ta))))
    for rel in sorted(set(ta) & set(tb)):
        if rel.endswith(".pkts"):
            if open(ta[rel]).read() != open(tb[rel]).read():
                problems.append("%s: packets differ" % rel)
        elif rel.endswith(".mvmap"):
            r = compare_maps(ta[rel], tb[rel])
            if isinstance(r, str):
                problems.append("%s: %s" % (rel, r))
            else:
                ties += r
    n_rec = sum(1 for r in ta if r.endswith(".pkts"))
    status = "OK" if not problems else "FAIL"
    print("%-28s %-8s %3d recordings  %s%s  | CPU rtspcam.py %6.2f s  C %5.2f s  %4.1fx" % (
        os.path.basename(clip), name, n_rec, status, " (%d tie frames)" % ties if ties else "",
        cpu_py, cpu_c, cpu_py / max(cpu_c, 1e-9)))
    for p in problems[:5]:
        print("    " + p)
    return not problems


def main():
    logging.disable(logging.CRITICAL)      # rtspcam's INFO lines would drown the report
    names = [a[len("--only="):] for a in sys.argv[1:] if a.startswith("--only=")]
    clips = [a for a in sys.argv[1:] if not a.startswith("--")]
    work = tempfile.mkdtemp(prefix="rcsession")
    ok = True
    try:
        for clip in clips:
            for name, args in SCENARIOS.items():
                if names and name not in names:
                    continue
                ok &= compare(clip, name, args, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
