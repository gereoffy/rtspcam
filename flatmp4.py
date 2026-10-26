"""flatmp4 - serve a fragmented MP4 (as the C recorder writes it) as a plain, non-fragmented MP4, without touching the file.

Firefox uses neither the mfra nor a sidx of a fragmented MP4: it cannot tell where a moment of the recording is in the
file, so a seek waits until the download gets there in order (long recordings over a slow line). A plain MP4 has the
whole index (moov) at the start, and the browser asks for the middle of the file right away.

The plain file is virtual, with ONE fixed offset:

    [ new ftyp ] [ new moov: the whole index ] [ mdat header ] [ the ORIGINAL file unchanged, from its first byte ]

An mdat may hold bytes nothing points to (players skip them): the old ftyp / empty moov / moofs / mdat headers / mfra
stay there as filler. The sample tables of the new moov point to `original position + E`, E = the length of the new
header (ftyp + moov + mdat header). The new moov is built from the fragment headers only (each moof is read, the mdats
are skipped with a seek): a few hundred small reads for a recording of several minutes.

    header(path) -> bytes (the first E bytes of the virtual file; E + original size = its size), or raises ValueError
    Cache(16).get(path) -> the same, the last 16 kept (least recently used out); None if the file is not fragmented

Standard library only (Python 3.5+).
"""
import collections
import os
import struct
import threading

CONTAINERS = {b"moov", b"trak", b"mdia", b"minf", b"stbl", b"dinf", b"edts", b"mvex"}


# ---- boxes ------------------------------------------------------------------------------------------------------
def iter_boxes(buf, off=0, end=None):
    """(type, payload start, payload end) of the boxes in buf[off:end]"""
    end = len(buf) if end is None else end
    while off + 8 <= end:
        size, typ = struct.unpack_from(">I4s", buf, off)
        hl = 8
        if size == 1:
            size = struct.unpack_from(">Q", buf, off + 8)[0]
            hl = 16
        elif size == 0:
            size = end - off
        if size < hl or off + size > end:
            raise ValueError("bad box %r at %d" % (typ, off))
        yield typ, off + hl, off + size
        off += size


def parse(buf, off=0, end=None):
    """[[type, payload bytes | children]]: the containers above are opened"""
    out = []
    for typ, a, b in iter_boxes(buf, off, end):
        out.append([typ, parse(buf, a, b) if typ in CONTAINERS else bytes(buf[a:b])])
    return out


def box(typ, payload):
    return struct.pack(">I4s", 8 + len(payload), typ) + payload


def full(typ, version, flags, payload):
    return box(typ, struct.pack(">I", (version << 24) | flags) + payload)


def serialize(tree):
    return b"".join(box(t, serialize(p) if isinstance(p, list) else p) for t, p in tree)


def find(tree, typ):
    for t, p in tree:
        if t == typ:
            return p
    return None


# ---- reading the fragments ----------------------------------------------------------------------------------------
class Track:
    def __init__(self, tid):
        self.tid = tid
        self.sizes, self.durs, self.sync, self.cts = [], [], [], []
        self.chunks = []            # (file offset, number of samples)
        self.start = None           # decode time of the first sample (media time scale)
        self.time = 0               # decode time after the last sample
        self.any_cts = False


def read_fragments(f, size, trex):
    """walks the top-level boxes: the moofs are read, the mdats skipped. trex: {track id: (dur, size, flags)}.
    Returns ({track id: Track}, the original moov payload)"""
    tracks, moov, pos = {}, None, 0
    while pos + 8 <= size:
        f.seek(pos)
        hdr = f.read(16)
        if len(hdr) < 8:
            break
        n, typ = struct.unpack(">I4s", hdr[:8])
        hl = 8
        if n == 1:
            n, hl = struct.unpack(">Q", hdr[8:16])[0], 16
        elif n == 0:
            n = size - pos
        if n < hl or pos + n > size:
            break                                   # (a truncated last box: what came before is still playable)
        if typ in (b"moov", b"moof"):
            f.seek(pos + hl)
            payload = f.read(n - hl)
            if typ == b"moov":
                moov = payload
                for t, a, b in iter_boxes(payload):     # the trex defaults are in moov/mvex
                    if t == b"mvex":
                        for t2, a2, b2 in iter_boxes(payload, a, b):
                            if t2 == b"trex":
                                tid, _, d, s, fl = struct.unpack_from(">IIIII", payload, a2 + 4)
                                trex[tid] = (d, s, fl)
            else:
                moof(payload, pos, tracks, trex)
        pos += n
    if moov is None:
        raise ValueError("no moov")
    return tracks, moov


def moof(buf, mo, tracks, trex):
    prev_end = mo
    first = True
    for typ, a, b in iter_boxes(buf):
        if typ != b"traf":
            continue
        tfhd = tfdt = None
        truns = []
        for t, a2, b2 in iter_boxes(buf, a, b):
            if t == b"tfhd":
                tfhd = (a2, b2)
            elif t == b"tfdt":
                tfdt = (a2, b2)
            elif t == b"trun":
                truns.append((a2, b2))
        if tfhd is None:
            continue
        p = tfhd[0]
        fl = struct.unpack_from(">I", buf, p)[0] & 0xFFFFFF
        tid = struct.unpack_from(">I", buf, p + 4)[0]
        q = p + 8
        dd, ds, df = trex.get(tid, (0, 0, 0))
        if fl & 0x1:
            base = struct.unpack_from(">Q", buf, q)[0]
            q += 8
        else:                                       # default-base-is-moof (the recorder's), or the first traf
            base = mo if (fl & 0x20000) or first else prev_end
        if fl & 0x2:
            q += 4
        if fl & 0x8:
            dd = struct.unpack_from(">I", buf, q)[0]
            q += 4
        if fl & 0x10:
            ds = struct.unpack_from(">I", buf, q)[0]
            q += 4
        if fl & 0x20:
            df = struct.unpack_from(">I", buf, q)[0]
        first = False
        tr = tracks.setdefault(tid, Track(tid))
        if tfdt is not None:
            v = buf[tfdt[0]]
            bmdt = struct.unpack_from(">Q" if v else ">I", buf, tfdt[0] + 4)[0]
            if tr.start is None:
                tr.start = tr.time = bmdt
            elif bmdt > tr.time and tr.durs:        # a gap (the camera dropped pictures): the last sample lasts longer
                tr.durs[-1] += bmdt - tr.time
                tr.time = bmdt
        elif tr.start is None:
            tr.start = 0
        data = base
        for ta, tb in truns:
            v = buf[ta]
            tfl = struct.unpack_from(">I", buf, ta)[0] & 0xFFFFFF
            cnt = struct.unpack_from(">I", buf, ta + 4)[0]
            q = ta + 8
            if tfl & 0x1:
                data = base + struct.unpack_from(">i", buf, q)[0]
                q += 4
            ff = None
            if tfl & 0x4:
                ff = struct.unpack_from(">I", buf, q)[0]
                q += 4
            start, total = data, 0
            for i in range(cnt):
                d, s, sf, c = dd, ds, df, 0
                if tfl & 0x100:
                    d = struct.unpack_from(">I", buf, q)[0]
                    q += 4
                if tfl & 0x200:
                    s = struct.unpack_from(">I", buf, q)[0]
                    q += 4
                if tfl & 0x400:
                    sf = struct.unpack_from(">I", buf, q)[0]
                    q += 4
                elif i == 0 and ff is not None:
                    sf = ff
                if tfl & 0x800:
                    c = struct.unpack_from(">i" if v else ">I", buf, q)[0]
                    q += 4
                    tr.any_cts = tr.any_cts or c != 0
                tr.sizes.append(s)
                tr.durs.append(d)
                tr.sync.append(not (sf & 0x10000))
                tr.cts.append(c)
                tr.time += d
                total += s
            if cnt:
                tr.chunks.append((start, cnt))
            data = start + total
        prev_end = data


# ---- the new moov ---------------------------------------------------------------------------------------------
def runs(values):
    out = []
    for x in values:
        if out and out[-1][1] == x:
            out[-1][0] += 1
        else:
            out.append([1, x])
    return out


def sample_tables(tr, stsd, shift):
    stts = runs(tr.durs)
    out = [[b"stsd", stsd],
           [b"stts", full(b"stts", 0, 0, struct.pack(">I", len(stts)) + b"".join(struct.pack(">II", n, d) for n, d in stts))[8:]]]
    if tr.any_cts:
        neg = any(c < 0 for c in tr.cts)
        ct = runs(tr.cts)
        out.append([b"ctts", full(b"ctts", 1 if neg else 0, 0, struct.pack(">I", len(ct)) +
                                  b"".join(struct.pack(">Ii" if neg else ">II", n, c) for n, c in ct))[8:]])
    if not all(tr.sync):
        idx = [i + 1 for i, s in enumerate(tr.sync) if s]
        out.append([b"stss", full(b"stss", 0, 0, struct.pack(">I", len(idx)) + b"".join(struct.pack(">I", i) for i in idx))[8:]])
    stsc = []
    for i, (_, n) in enumerate(tr.chunks):
        if not stsc or stsc[-1][1] != n:
            stsc.append((i + 1, n))
    out.append([b"stsc", full(b"stsc", 0, 0, struct.pack(">I", len(stsc)) +
                              b"".join(struct.pack(">III", c, n, 1) for c, n in stsc))[8:]])
    if len(set(tr.sizes)) == 1:
        stsz = struct.pack(">II", tr.sizes[0], len(tr.sizes))
    else:
        stsz = struct.pack(">II", 0, len(tr.sizes)) + b"".join(struct.pack(">I", s) for s in tr.sizes)
    out.append([b"stsz", full(b"stsz", 0, 0, stsz)[8:]])
    offs = [o + shift for o, _ in tr.chunks]
    if offs and max(offs) > 0xFFFFFFFF:
        out.append([b"co64", full(b"co64", 0, 0, struct.pack(">I", len(offs)) + b"".join(struct.pack(">Q", o) for o in offs))[8:]])
    else:
        out.append([b"stco", full(b"stco", 0, 0, struct.pack(">I", len(offs)) + b"".join(struct.pack(">I", o) for o in offs))[8:]])
    return out


def set_duration(payload, typ, value):
    """mvhd / mdhd / tkhd payload with a new duration (version 0 or 1)"""
    b = bytearray(payload)
    v = b[0]
    if typ == b"tkhd":
        p = 4 + (16 if v else 8) + 8       # creation, modification; track id, reserved
    else:
        p = 4 + (16 if v else 8) + 4       # creation, modification; time scale
    if v:
        struct.pack_into(">Q", b, p, value)
    else:
        struct.pack_into(">I", b, p, min(value, 0xFFFFFFFF))
    return bytes(b)


def timescale(payload):
    return struct.unpack_from(">I", payload, 4 + (16 if payload[0] else 8))[0]


def track_id(tkhd):
    return struct.unpack_from(">I", tkhd, 4 + (16 if tkhd[0] else 8))[0]


def build_moov(moov_payload, tracks, shift):
    tree = parse(moov_payload)
    mvhd = find(tree, b"mvhd")
    if mvhd is None:
        raise ValueError("no mvhd")
    mts = timescale(mvhd)
    out, longest = [], 0
    for typ, p in tree:
        if typ == b"mvex":
            continue                                        # not fragmented any more
        if typ != b"trak":
            out.append([typ, p])
            continue
        tkhd, mdia = find(p, b"tkhd"), find(p, b"mdia")
        mdhd = find(mdia, b"mdhd") if mdia else None
        minf = find(mdia, b"minf") if mdia else None
        stbl = find(minf, b"stbl") if minf else None
        stsd = find(stbl, b"stsd") if stbl else None
        tr = tracks.get(track_id(tkhd)) if tkhd else None
        if tr is None or not tr.sizes or stsd is None or mdhd is None:
            continue                                        # (a track without samples is left out)
        ts = timescale(mdhd)
        media = sum(tr.durs)
        start = tr.start or 0
        movie = int(round(media * mts / float(ts)))
        delay = int(round(start * mts / float(ts)))        # e.g. the sound starts after the first picture
        longest = max(longest, delay + movie)
        trak = []
        for t2, p2 in p:
            if t2 == b"tkhd":
                trak.append([t2, set_duration(p2, t2, delay + movie)])
                if delay > 0:                               # an empty edit first, then the samples
                    trak.append([b"edts", [[b"elst", full(b"elst", 0, 0, struct.pack(">I", 2) +
                                 struct.pack(">IiI", delay, -1, 0x10000) + struct.pack(">IiI", movie, 0, 0x10000))[8:]]]])
            elif t2 == b"edts":
                continue
            elif t2 == b"mdia":
                md = []
                for t3, p3 in p2:
                    if t3 == b"mdhd":
                        md.append([t3, set_duration(p3, t3, media)])
                    elif t3 == b"minf":
                        mi = []
                        for t4, p4 in p3:
                            mi.append([t4, sample_tables(tr, stsd, shift)] if t4 == b"stbl" else [t4, p4])
                        md.append([t3, mi])
                    else:
                        md.append([t3, p3])
                trak.append([t2, md])
            else:
                trak.append([t2, p2])
        out.append([b"trak", trak])
    out = [[t, set_duration(p, t, longest)] if t == b"mvhd" else [t, p] for t, p in out]
    if not any(t == b"trak" for t, _ in out):
        raise ValueError("no track with samples")
    return box(b"moov", serialize(out))


FTYP = box(b"ftyp", b"isom" + struct.pack(">I", 0x200) + b"isomiso2avc1mp41")


def header(path):
    """the first E bytes of the plain virtual file (ftyp + moov + mdat header); ValueError if it cannot be made"""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        tracks, moov = read_fragments(f, size, {})
    if not tracks:
        raise ValueError("not fragmented")
    big = size + 16 > 0xFFFFFFFF
    mdat_hl = 16 if big else 8
    # the moov's size does not depend on the offsets (stco: 4 bytes each), so two passes give the exact E
    m = build_moov(moov, tracks, 0)
    e = len(FTYP) + len(m) + mdat_hl
    m = build_moov(moov, tracks, e)
    if len(FTYP) + len(m) + mdat_hl != e:           # (stco -> co64 because of the shift: once more)
        e = len(FTYP) + len(m) + mdat_hl
        m = build_moov(moov, tracks, e)
    mdat = struct.pack(">I4sQ", 1, b"mdat", 16 + size) if big else struct.pack(">I4s", 8 + size, b"mdat")
    return FTYP + m + mdat


class Cache:
    """the headers of the last n recordings played/downloaded (least recently used out); key: path + size + mtime"""

    def __init__(self, n=16):
        self.n, self.lock, self.d = n, threading.Lock(), collections.OrderedDict()

    def get(self, path):
        st = os.stat(path)
        key = (path, st.st_size, int(st.st_mtime))
        with self.lock:
            if key in self.d:
                self.d.move_to_end(key)
                return self.d[key]
        try:
            h = header(path)
        except (ValueError, struct.error, OSError, IndexError):
            h = None                                     # (not a fragmented MP4, or damaged: the file goes as it is)
        with self.lock:
            self.d[key] = h
            while len(self.d) > self.n:
                self.d.popitem(last=False)
        return h
