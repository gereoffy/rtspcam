"""h264fix - lossless SPS patch for browser-friendlier playback of the camera streams.

The cameras declare max_num_ref_frames=2 in the SPS but use long-term references and
reference list modification; browser hardware decoders handle that stream correctly
when the declared DPB is larger (measured: frame-by-frame error in the browser drops from
~8 to ~0.7, same as a full re-encode). Only the SPS is rewritten (in the avcC extradata
and in-band in the key frame packets); slice data is untouched, so ffmpeg decodes
bit-identical pixels. The optional VUI patch (`vui`) turned out to be unnecessary and is
only safe for SPSs whose last VUI field is bitstream_restriction_flag, so it is off.
"""
import re

try:                      # PyAV is only needed to convert files, not to inspect them
    import av
except ImportError:
    av = None

HIGH_PROFILES = (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135)


def _unescape(b):
    out, i = bytearray(), 0
    while i < len(b):
        if i + 2 < len(b) and b[i] == 0 and b[i + 1] == 0 and b[i + 2] == 3:
            out += b"\0\0"
            i += 3
        else:
            out.append(b[i])
            i += 1
    return bytes(out)


def _escape(b):
    out, z = bytearray(), 0
    for x in b:
        if z >= 2 and x <= 3:
            out.append(3)
            z = 0
        out.append(x)
        z = z + 1 if x == 0 else 0
    return bytes(out)


def _bits(b):
    return [(x >> (7 - i)) & 1 for x in b for i in range(8)]


def _bytes(bits):
    bits = bits + [0] * (-len(bits) % 8)
    return bytes(sum(bits[i + j] << (7 - j) for j in range(8)) for i in range(0, len(bits), 8))


def _ue(v):
    v += 1
    return [0] * (v.bit_length() - 1) + [int(c) for c in bin(v)[2:]]


class _Reader:
    def __init__(self, bits):
        self.b, self.p = bits, 0

    def u(self, n):
        v = 0
        for _ in range(n):
            v = v * 2 + self.b[self.p]
            self.p += 1
        return v

    def ue(self):
        z = 0
        while self.b[self.p] == 0:
            z += 1
            self.p += 1
        self.p += 1
        return (1 << z) - 1 + self.u(z)

    def se(self):
        k = self.ue()
        return (k + 1) // 2 if k % 2 else -(k // 2)


def _locate(nal):
    """Parse an SPS NAL up to max_num_ref_frames. Returns (bits, pos, after, old_value) or None."""
    try:
        bits = _bits(_unescape(nal[1:]))
        r = _Reader(bits)
        prof = r.u(8)
        r.u(16)
        r.ue()
        if prof in HIGH_PROFILES:
            cf = r.ue()
            if cf == 3:
                r.u(1)
            r.ue()
            r.ue()
            r.u(1)
            if r.u(1):                                   # seq_scaling_matrix_present_flag
                for i in range(12 if cf == 3 else 8):
                    if r.u(1):
                        last = nxt = 8
                        for _ in range(16 if i < 6 else 64):
                            if nxt != 0:
                                nxt = (last + r.se() + 256) % 256
                            last = last if nxt == 0 else nxt
        r.ue()                                           # log2_max_frame_num_minus4
        poc_type = r.ue()
        if poc_type == 0:
            r.ue()
        elif poc_type == 1:
            return None                                  # not needed for these cameras
        pos = r.p
        old = r.ue()                                     # max_num_ref_frames
        return bits, pos, r.p, old
    except Exception:
        return None


def patch_sps(nal, refs=None, vui=None):
    """nal = SPS NAL unit including its 1-byte header. refs: new max_num_ref_frames.
    vui: (num_reorder_frames, max_dec_frame_buffering) to add if the SPS has VUI without
    bitstream_restriction (off by default, see module docstring). Returns the new NAL
    (the original one if it cannot be patched)."""
    loc = _locate(nal)
    if loc is None:
        return nal
    bits, pos, after, _ = loc
    new = bits[:pos] + (_ue(refs) if refs is not None else bits[pos:after]) + bits[after:]
    last = max(i for i, b in enumerate(new) if b)        # rbsp stop bit
    if vui is not None and new[last - 1] == 0:
        reorder, dpb = vui
        add = [1, 1] + _ue(0) + _ue(0) + _ue(15) + _ue(15) + _ue(reorder) + _ue(max(dpb, reorder))
        new = new[:last - 1] + add + [1]
    else:
        new = new[:last + 1]
    return nal[:1] + _escape(_bytes(new))


def patch_avcc(extradata, refs=None, vui=None):
    ex = bytes(extradata)
    if len(ex) < 7 or ex[0] != 1:
        return ex
    out, pos = bytearray(ex[:5]), 5
    n = ex[pos] & 0x1F
    out.append(ex[pos])
    pos += 1
    for _ in range(n):
        ln = int.from_bytes(ex[pos:pos + 2], "big")
        sps = patch_sps(ex[pos + 2:pos + 2 + ln], refs, vui)
        out += len(sps).to_bytes(2, "big") + sps
        pos += 2 + ln
    return bytes(out) + ex[pos:]                         # PPS list and optional high-profile tail


def patch_packet(data, length_size, refs=None, vui=None):
    """Patch SPS NAL units inside one length-prefixed (AVCC) sample."""
    data, out, pos = bytes(data), bytearray(), 0
    while pos + length_size <= len(data):
        ln = int.from_bytes(data[pos:pos + length_size], "big")
        nal = data[pos + length_size:pos + length_size + ln]
        if nal and (nal[0] & 31) == 7:
            nal = patch_sps(nal, refs, vui)
        out += len(nal).to_bytes(length_size, "big") + nal
        pos += length_size + ln
    return bytes(out)


_ANNEXB_START = re.compile(b"\x00\x00\x01")


def patch_annexb(data, refs=None, vui=None):
    """Patch SPS NAL units in an Annex B buffer. Returns (new_bytes, changed); the input is
    returned untouched (changed=False) when it holds no SPS."""
    data = bytes(data)
    starts = [m.start() for m in _ANNEXB_START.finditer(data)]
    if not any(st + 3 < len(data) and (data[st + 3] & 31) == 7 for st in starts):
        return data, False
    out, changed = bytearray(data[:starts[0]]), False
    for k, st in enumerate(starts):
        end = starts[k + 1] if k + 1 < len(starts) else len(data)
        nal = data[st + 3:end]
        z = len(nal)
        while z > 0 and nal[z - 1] == 0:            # zero bytes of the next 4-byte start code
            z -= 1
        body, trail = nal[:z], nal[z:]
        if body and (body[0] & 31) == 7:
            new = patch_sps(body, refs, vui)
            changed |= new != body
            body = new
        out += b"\x00\x00\x01" + body + trail
    return (bytes(out), True) if changed else (data, False)


def patch_sample(data, length_size=4, refs=None, vui=None):
    """Patch SPS NAL units in one packet, whatever its format (Annex B from RTSP/raw
    streams, length-prefixed AVCC from MP4). Returns (bytes, changed); `bytes` is the input
    object itself when nothing had to change."""
    raw = bytes(data)
    if raw[:3] == b"\x00\x00\x01" or raw[:4] == b"\x00\x00\x00\x01":
        return patch_annexb(raw, refs, vui)
    pos, n = 0, len(raw)
    while pos + length_size <= n:                    # cheap scan: is there an SPS at all?
        ln = int.from_bytes(raw[pos:pos + length_size], "big")
        if ln and pos + length_size < n and (raw[pos + length_size] & 31) == 7:
            return patch_packet(raw, length_size, refs, vui), True
        pos += length_size + ln
    return raw, False


def patch_extradata(ex, refs=None, vui=None):
    """avcC (MP4) or Annex B (RTSP/raw) codec extradata."""
    ex = bytes(ex or b"")
    if ex[:1] == b"\x01":
        return patch_avcc(ex, refs, vui)
    if ex[:3] == b"\x00\x00\x01" or ex[:4] == b"\x00\x00\x00\x01":
        return patch_annexb(ex, refs, vui)[0]
    return ex


def _need_av():
    if av is None:
        raise RuntimeError("PyAV is required to convert recordings (pip install av)")


def remux_patched(src_path, dst_path, refs=4, vui=None):
    """Stream-copy src into a faststart MP4 with the patched SPS (timestamps rebased to 0)."""
    _need_av()
    src = av.open(src_path)
    vs = src.streams.video[0]
    dst = av.open(dst_path, "w", format="mp4", options={"movflags": "faststart"})
    ds = dst.add_stream_from_template(vs)
    ex = vs.codec_context.extradata
    length_size = ((ex[4] & 3) + 1) if ex and len(ex) > 4 else 4
    if ex:
        ds.codec_context.extradata = patch_avcc(ex, refs, vui)
    base = None
    for pkt in src.demux(vs):
        if pkt.dts is None or pkt.size == 0:
            continue
        base = pkt.dts if base is None else base
        np_ = av.Packet(patch_packet(pkt, length_size, refs, vui))
        np_.dts, np_.pts, np_.duration, np_.time_base = pkt.dts - base, (pkt.pts - base if pkt.pts is not None else None), pkt.duration, pkt.time_base
        np_.is_keyframe = pkt.is_keyframe
        np_.stream = ds
        dst.mux(np_)
    dst.close()
    src.close()


def max_refs(extradata):
    """max_num_ref_frames declared in the first SPS of an avcC extradata blob (None if unknown)."""
    ex = bytes(extradata or b"")
    if len(ex) < 8 or ex[0] != 1 or (ex[5] & 0x1F) < 1:
        return None
    ln = int.from_bytes(ex[6:8], "big")
    loc = _locate(ex[8:8 + ln])
    return None if loc is None else loc[3]


def read_avcc(path):
    """The avcC (H.264 decoder configuration) of an MP4, found by scanning the moov box. No PyAV needed."""
    with open(path, "rb") as fh:
        pos = 0
        while True:
            fh.seek(pos)
            hdr = fh.read(16)
            if len(hdr) < 8:
                return None
            size, typ = int.from_bytes(hdr[:4], "big"), hdr[4:8]
            if size == 1:
                size = int.from_bytes(hdr[8:16], "big")
            if typ == b"moov":
                fh.seek(pos)
                moov = fh.read(min(size, 1 << 24))
                i = moov.find(b"avcC")
                if i < 4:
                    return None
                n = int.from_bytes(moov[i - 4:i], "big")
                return moov[i + 4:i - 4 + n]
            if size < 8:
                return None
            pos += size


_ready_cache = {}


def is_ready(path, min_refs=3):
    """True if the MP4's SPS already declares at least `min_refs` reference frames, i.e. the
    file was written with the fix (inline or remuxed) and needs no copy for browsers."""
    import os
    try:
        st = os.stat(path)
        key = (path, st.st_size, int(st.st_mtime))
        if key not in _ready_cache:
            refs = max_refs(read_avcc(path))
            _ready_cache[key] = refs is not None and refs >= min_refs
        return _ready_cache[key]
    except Exception:
        return False


def fix_file_inplace(path, refs=4):
    """Replace `path` by its browser-ready version (faststart MP4, atomic). Returns False if already ready."""
    import os
    if is_ready(path, refs):
        return False
    _need_av()
    tmp = path + ".fix.tmp"
    try:
        remux_patched(path, tmp, refs=refs)
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    return True


if __name__ == "__main__":
    import argparse
    import sys
    ap = argparse.ArgumentParser(description="Make recordings browser-ready in place "
                                             "(lossless SPS fix + faststart); already fixed files are skipped.")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    done = skipped = failed = 0
    for f in a.files:
        try:
            if fix_file_inplace(f):
                done += 1
                print("fixed  ", f)
            else:
                skipped += 1
        except Exception as e:
            failed += 1
            print("FAILED ", f, e, file=sys.stderr)
    print("%d fixed, %d already ok, %d failed" % (done, skipped, failed))
