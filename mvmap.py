"""mvmap - sidecar file with the per-frame motion map of a recording.

Written by rtspcam.py next to every <name>_<HHMMSS>.mp4 as <name>_<HHMMSS>.mvmap,
read by the player. Layout (all integers little-endian):

    8 bytes   magic  b"RCMVMAP1"
    u32       length of the JSON header, followed by the UTF-8 JSON header
    ...       a single zlib stream (flushed after every frame, so a truncated
              file is still readable) holding frame records back to back:

        u32 t_ms      frame time in ms from the first frame of the mp4 (== mp4 timestamps)
        u16 cluster   size of the largest connected group of moving 16x16 cells
        u16 blocks    moving 16x16-macroblock equivalents (clamped to 65535)
        u8  flags     1 = key frame (no vectors), 2 = "motion frame", 4 = detector triggered,
                      8 = no data (the decoder produced no frame for this packet),
                      16 = not analysed: the frame's reference picture is too far back
                      (C rtspcam --max-ref-dist; hierarchical P frames); the C recorder also
                      sets it on key frames (1 | 16), which have no vectors either
        u16 n         number of cells that follow
        n x (u16 cell, u8 level)   cell = gy * gw + gx
                                   level 1 = moving cell, 2 = the largest group while it is below the
                                   threshold, 3 = every connected group at/above the threshold
                                   (--min-cluster; in block mode the largest group once --min-blocks
                                   is reached), i.e. what makes the detector trigger

Cells that are not listed have level 0 (nothing).

Optional motion field sidecar <name>_<HHMMSS>.mvvec (C rtspcam --vectors): same framing with
the magic b"RCMVVEC2" (JSON header with w, h, gw, gh), then one record per packet, in the same
order and with the same t_ms as the .mvmap records:

        u32 t_ms
        u8  has_grid    0 = no vectors (key frame, frame not analysed, no data)
        u32 frame_bytes the coded picture as stored in the MP4 (with length fields, SPS/PPS/SEI)
        u32 vcl_bytes   what the decoder works from: the RBSP bytes of the picture's slice NAL units,
                        without NAL header bytes, start codes/length fields, parameter sets, SEI,
                        emulation prevention bytes and trailing zero padding
        gw * gh bytes if has_grid: the longest motion vector of each 16x16 cell (all vectors,
            no threshold or zones, measured like the detector: hypot(mx, my) / 4):
            low 4 bits = its length in pixels, floored (0-15, longer ones are 15),
            high 4 bits = its direction in 22.5 degree steps: 0 = right (+x), 4 = down (+y),
            8 = left, 12 = up (image coordinates); a byte of 0 means no vector of 1 px or more.
            For an integer --mv-min M, the cells with length >= M are exactly the detector's
            moving cells (before the --ignore zones), so the clusters can be recomputed The header carries the grid size
(gw x gh cells of `cell` px), the frame size and the heuristic parameters in effect.
"""
import json
import os
import struct
import zlib

try:                      # numpy is only needed to write maps and to read them cell by cell
    import numpy as np
except ImportError:       # (the player works without it)
    np = None

MAGIC = b"RCMVMAP1"
MAGIC_VEC = b"RCMVVEC2"
MAGIC_VEC1 = b"RCMVVEC1"       # version 1: no frame sizes
REC = struct.Struct("<IHHBH")
CELL_DT = np.dtype([("c", "<u2"), ("l", "u1")]) if np is not None else None  # packed, 3 bytes
F_KEY, F_MOVING, F_TRIG, F_NODATA, F_SKIP = 1, 2, 4, 8, 16


class MapWriter:
    def __init__(self, path, header):
        self.path = path
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.f = open(path + ".part", "wb")
        hdr = json.dumps(dict(header, version=1, cell=16, time_unit="ms")).encode()
        self.f.write(MAGIC + struct.pack("<I", len(hdr)) + hdr)
        self.z = zlib.compressobj(6)

    def write(self, t_ms, cluster, blocks, flags, cells):
        """cells: structured numpy array of CELL_DT (may be empty)."""
        payload = REC.pack(max(0, t_ms), min(cluster, 65535), min(int(blocks), 65535),
                           flags, len(cells)) + cells.tobytes()
        self.f.write(self.z.compress(payload) + self.z.flush(zlib.Z_SYNC_FLUSH))

    def close(self):
        self.f.write(self.z.flush())
        self.f.close()
        os.replace(self.path + ".part", self.path)


def read_raw(path, magic=MAGIC):
    """Return (header dict, decompressed record bytes). Tolerates a truncated file."""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != magic:
        raise ValueError("not an %s file: %s" % ("mvmap" if magic == MAGIC else "mvvec", path))
    (hl,) = struct.unpack_from("<I", data, 8)
    header = json.loads(data[12:12 + hl].decode("utf-8"))
    return header, zlib.decompressobj().decompress(data[12 + hl:])


def summary(path):
    """(header, duration_s, alarm_s, peak_cluster) without numpy: whole-recording numbers for lists."""
    header, body = read_raw(path)
    t, trig, peak, pos = [], [], 0, 0
    while pos + REC.size <= len(body):
        tm, cluster, _, flags, n = REC.unpack_from(body, pos)
        end = pos + REC.size + 3 * n
        if end > len(body):
            break
        t.append(tm / 1000.0)
        trig.append(bool(flags & F_TRIG))
        peak = max(peak, cluster)
        pos = end
    if not t:
        return header, 0.0, 0.0, 0
    step = (t[-1] - t[0]) / max(1, len(t) - 1)
    alarm = sum((t[i + 1] if i + 1 < len(t) else t[i] + step) - t[i] for i in range(len(t)) if trig[i])
    return header, t[-1] + step, alarm, peak


def read(path):
    """Return (header, frames); frames is a list of dicts with t_ms, cluster, blocks,
    flags and cells (numpy CELL_DT array). Tolerates a truncated file. Needs numpy."""
    if np is None:
        raise RuntimeError("numpy is required to read cell data (pip install numpy)")
    header, body = read_raw(path)
    frames, pos = [], 0
    while pos + REC.size <= len(body):
        t, cluster, blocks, flags, n = REC.unpack_from(body, pos)
        end = pos + REC.size + 3 * n
        if end > len(body):
            break
        cells = np.frombuffer(body, CELL_DT, n, pos + REC.size)
        frames.append({"t_ms": t, "cluster": cluster, "blocks": blocks, "flags": flags, "cells": cells})
        pos = end
    return header, frames


def read_vec(path):
    """Return (header, frames) of an .mvvec file; frames is a list of dicts with t_ms, grid
    (uint8 array of shape (gh, gw), None for frames without vectors), frame_bytes and vcl_bytes
    (None in version 1 files). Needs numpy."""
    if np is None:
        raise RuntimeError("numpy is required to read vector grids (pip install numpy)")
    with open(path, "rb") as fh:
        v1 = fh.read(8) == MAGIC_VEC1
    header, body = read_raw(path, MAGIC_VEC1 if v1 else MAGIC_VEC)
    n = header["gw"] * header["gh"]
    rec = struct.Struct("<IB" if v1 else "<IBII")
    frames, pos = [], 0
    while pos + rec.size <= len(body):
        fields = rec.unpack_from(body, pos)
        end = pos + rec.size + (n if fields[1] else 0)
        if end > len(body):
            break
        grid = np.frombuffer(body, np.uint8, n, pos + rec.size).reshape(header["gh"], header["gw"]) if fields[1] else None
        frames.append({"t_ms": fields[0], "grid": grid,
                       "frame_bytes": None if v1 else fields[2], "vcl_bytes": None if v1 else fields[3]})
        pos = end
    return header, frames


def decode_vec(grid):
    """(length_px, angle_deg) arrays of a grid: the longest vector of each cell, length floored to
    whole pixels; angle in image coordinates (0 = right, 90 = down)."""
    return grid & 15, (grid >> 4) * 22.5
