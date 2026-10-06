#!/usr/bin/env python3
"""player.py - web player (and, with --live-dir, live view) for the recordings made by the C recorder (src/rtspcam).

    player.py /srv/cameras            # root = the -o directory of the recorder
    player.py ./rec --port 8780 --live-dir /run/rtspcam [--cameras a,b,c]

Pages: / (index.html), /player and /player/<camera> (the player), /live (live view, with --live-dir).
Lists the recordings of all cameras (filter by period / camera / date / time of day), streams the
full-resolution H.264 files to the browser exactly as they are (the recorder writes browser-ready files)
and draws the motion map (.mvmap) / vectors (.mvvec) over them; tuning mode works from the .mvvec.
Standard library only; PyAV is used only to read the length of a recording that has no .mvmap.
"""
import argparse
import gzip
import json
import os
import re
import struct
import subprocess
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn

try:
    from http.server import ThreadingHTTPServer
except ImportError:                     # Python < 3.7 (e.g. Ubuntu 16.04: 3.5)
    class ThreadingHTTPServer(ThreadingMixIn, HTTPServer):
        daemon_threads = True
from urllib.parse import parse_qs, urlparse

try:                      # only needed to time recordings without a map
    import av
except ImportError:
    av = None

import mvmap

HERE = os.path.dirname(os.path.abspath(__file__))
RTSPCAM = os.path.join(HERE, "src", "rtspcam")        # the recorder binary: file mode writes the missing .mvvec of a recording (--rtspcam)
GENVEC_LOCKS = {}                                 # one generation per file at a time
GENVEC_SLOTS = threading.Semaphore(2)             # and at most two at once (it is CPU work)
LIVE = None                                       # liveview.Live when --live-dir is given
CONFIG_DIR = os.path.join(HERE, "configs")        # the tuning panel saves/loads configs/<camera>.json here (see --config-dir)
TUNE_KEYS = ("mv_min", "min_cluster", "global_limit", "window", "trigger_frames", "pre_roll", "post_roll", "ignore")
# <name>_<HHMMSS>.mp4 (the recorder); any other *.mp4 is accepted too, its time of day then comes from the file time
FNAME = re.compile(r"^(?P<stem>.+?)(?:_(?P<hms>\d{6}))?\.mp4$")
DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def has_audio(path):
    """does the MP4 have a sound track? Walks the top-level boxes (the mdat is skipped, not read) to
    the moov (at the start in the recorder's files, often at the end in others) and looks for a
    handler of type 'soun' in it: a few small reads. Asked only for the recording being opened
    (/api/audio), never for the whole listing."""
    try:
        with open(path, "rb") as f:
            size = os.fstat(f.fileno()).st_size
            pos = 0
            while pos + 8 <= size:
                f.seek(pos)
                hdr = f.read(16)
                n, typ = struct.unpack(">I4s", hdr[:8])
                hl = 8
                if n == 1 and len(hdr) >= 16:
                    n, hl = struct.unpack(">Q", hdr[8:16])[0], 16
                elif n == 0:
                    n = size - pos
                if n < hl:
                    return False
                if typ == b"moov":
                    f.seek(pos + hl)
                    moov = f.read(min(n - hl, 16 << 20))
                    i = moov.find(b"hdlr")
                    while i >= 0:
                        if moov[i + 12:i + 16] == b"soun":   # type, version/flags, pre_defined, handler type
                            return True
                        i = moov.find(b"hdlr", i + 4)
                    return False
                pos += n
    except (OSError, struct.error):
        pass
    return False


class Library:
    def __init__(self, root):
        self.root = os.path.realpath(root)
        self.lock = threading.Lock()
        self.meta = {}                        # per-recording summary, kept in memory only (built at start-up by warm_up(), nothing on disk)

    # ---- listing -----------------------------------------------------------------
    def scan(self, since_date=None):
        out = []
        for cam in sorted(os.listdir(self.root)):
            cdir = os.path.join(self.root, cam)
            if not os.path.isdir(cdir):
                continue
            for date in sorted(os.listdir(cdir)):
                ddir = os.path.join(cdir, date)
                if not (DATE.match(date) and os.path.isdir(ddir)) or (since_date and date < since_date):
                    continue            # (older days are not even listed when only the recent ones are wanted)
                for fn in sorted(os.listdir(ddir)):
                    m = FNAME.match(fn)
                    if m:
                        path = os.path.join(ddir, fn)
                        hms = m.group("hms")
                        if not hms:                   # e.g. 4419268-video.mp4 from another recorder
                            try:
                                hms = time.strftime("%H%M%S", time.localtime(os.stat(path).st_mtime))
                            except OSError:
                                continue
                        out.append((cam, date, hms, path))
        return out

    def describe(self, path):
        st = os.stat(path)
        mp = path[:-4] + ".mvmap"
        try:                                      # the map is written after the video: its presence/size/time belongs to the cache key,
            ms = os.stat(mp)                      # otherwise a "no map" result would stay valid forever
            mkey = "%d:%d" % (ms.st_size, int(ms.st_mtime))
        except OSError:
            mkey = "-"
        key = "%s|%d|%d|%s" % (path, st.st_size, int(st.st_mtime), mkey)
        hit = self.meta.get(path)
        if hit and hit["key"] == key:
            return hit["m"]
        m = {"size": st.st_size, "has_map": False, "duration": 0.0, "alarm_s": 0.0, "peak": 0}
        mp = path[:-4] + ".mvmap"
        try:
            if os.path.exists(mp):
                hdr, duration, alarm, peak = mvmap.summary(mp)
                if duration:
                    m.update(duration=duration, alarm_s=alarm, peak=peak, has_map=True)
            if not m["has_map"] and av is not None:     # no map: ask the container (needs PyAV)
                c = av.open(path)
                vs = c.streams.video[0]
                last, first = None, None
                for p in c.demux(vs):
                    if p.pts is not None and p.size:
                        first = p.pts if first is None else first
                        last = p.pts
                if last is not None:
                    m["duration"] = float((last - first) * vs.time_base)
                c.close()
        except Exception:
            pass
        self.meta[path] = {"key": key, "m": m}
        return m

    def listing(self, camera=None, since=None):
        """since = epoch seconds: only recordings starting at or after it (the page loads just the period it shows and
        refreshes incrementally); the cache index is pruned only by a complete listing"""
        items = []
        seen = set()
        for cam, date, hms, path in self.scan(time.strftime("%Y-%m-%d", time.localtime(since)) if since else None):
            seen.add(path)
            if camera is not None and cam != camera:       # /player/<camera>: only that camera (the others stay in the cache index)
                continue
            rel = os.path.relpath(path, self.root)[:-4]
            ts = datetime.strptime(date + hms, "%Y-%m-%d%H%M%S").timestamp()
            if since and ts < since:
                continue
            m = self.describe(path)
            items.append({"id": rel, "camera": cam, "date": date, "time": "%s:%s:%s" % (hms[:2], hms[2:4], hms[4:]),
                          "name": os.path.basename(rel), "ts": ts, "has_vec": os.path.exists(path[:-4] + ".mvvec"), **m})
        if not since:
            self.meta = {k: v for k, v in self.meta.items() if k in seen}      # forget deleted recordings
        return items

    def ensure_vec(self, mp4):
        """path of the recording's .mvvec; a missing one is made first: `rtspcam --vectors FILE.mp4` (file mode) writes only the missing sidecar, never touches an
        existing .mvmap/.mvvec, takes ~0.3 s for 20 s of 2K video. The vectors do not depend on the detection parameters, only on --max-ref-dist, so the
        camera's config (configs/<camera>.json) is passed first, a plain run if that fails. One run per file at a time, at most two at once."""
        vec = mp4[:-4] + ".mvvec"
        if os.path.exists(vec):
            return vec
        if not (RTSPCAM and os.access(RTSPCAM, os.X_OK)):
            raise RuntimeError("no .mvvec, and the recorder binary was not found to make it: %s (--rtspcam)" % RTSPCAM)
        with GENVEC_LOCKS.setdefault(mp4, threading.Lock()), GENVEC_SLOTS:
            if os.path.exists(vec):                      # made meanwhile by another request
                return vec
            cfg = os.path.join(CONFIG_DIR, re.sub(r"[^A-Za-z0-9_.-]", "_", os.path.relpath(mp4, self.root).split(os.sep)[0]) + ".json")
            runs = ([[RTSPCAM, "--config", cfg, "--vectors", mp4]] if os.path.exists(cfg) else []) + [[RTSPCAM, "--vectors", mp4]]
            out = ""
            for cmd in runs:
                r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
                out = r.stdout.decode("utf-8", "replace")[-400:]
                if r.returncode == 0 and os.path.exists(vec):
                    return vec
            raise RuntimeError("rtspcam --vectors failed: " + out)

    def warm_up(self):
        """build the whole index once in the background at start-up, so the first page load does not have to"""
        t = time.time()
        n = len(self.listing())
        print("player: indexed %d recordings in %.1f s" % (n, time.time() - t), flush=True)

    def resolve(self, rid, ext):
        # abspath, not realpath: symlinked camera/date folders or files that point outside the root are
        # intentional (the id cannot contain ".." after normalisation, so it stays below the root)
        p = os.path.abspath(os.path.join(self.root, rid + ext))
        if not p.startswith(self.root + os.sep) or not os.path.isfile(p):
            return None
        return p


def make_handler(lib):
    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a):
            pass

        def send_bytes(self, data, ctype, code=200, extra=None):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(data)

        def cfg_path(self, name):
            return os.path.join(CONFIG_DIR, re.sub(r"[^A-Za-z0-9_.-]", "_", name) + ".json")

        def do_POST(self):
            u = urlparse(self.path)
            q = parse_qs(u.query)
            if u.path == "/api/tuneconfig" and q.get("name"):
                try:
                    new = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))).decode("utf-8"))
                    path = self.cfg_path(q["name"][0])
                    cfg = {}
                    if os.path.exists(path):                    # keep keys the panel does not know (e.g. max_ref_dist)
                        with open(path) as fh:
                            cfg = json.load(fh)
                    cfg.update({k: new[k] for k in TUNE_KEYS if k in new})
                    os.makedirs(CONFIG_DIR, exist_ok=True)
                    with open(path + ".tmp", "w") as fh:
                        json.dump(cfg, fh, indent=2)
                        fh.write("\n")
                    os.replace(path + ".tmp", path)
                    return self.send_bytes(json.dumps({"saved": path}).encode(), "application/json")
                except Exception as e:
                    return self.send_bytes(str(e).encode(), "text/plain", 400)
            self.send_bytes(b"not found", "text/plain", 404)

        def do_GET(self):
            u = urlparse(self.path)
            q = parse_qs(u.query)
            try:
                if u.path == "/favicon.ico":                                # next to this script (read on every request)
                    try:
                        with open(os.path.join(HERE, "favicon.ico"), "rb") as fh:
                            return self.send_bytes(fh.read(), "image/x-icon", extra={"Cache-Control": "max-age=86400"})
                    except OSError:
                        return self.send_bytes(b"not found", "text/plain", 404)
                if LIVE is not None and LIVE.handle(self, u.path):          # /live, /live/<camera>, /status (liveview.py)
                    return
                if LIVE is None and u.path in ("/live", "/live/"):          # no --live-dir: say so instead of a 404
                    return self.send_bytes("<!doctype html><meta charset=utf-8><title>Élőkép</title><body style=\"background:#111318;color:#ffb4a8;"
                                           "font:16px system-ui;text-align:center;padding-top:30vh\">Hiba: nem fut a rögzítő<br>"
                                           "<small style=\"color:#9aa0a6\">(a lejátszó nincs --live-dir kapcsolóval indítva)</small>".encode(),
                                           "text/html; charset=utf-8")
                if u.path == "/":
                    with open(os.path.join(HERE, "index.html"), "rb") as fh:
                        return self.send_bytes(fh.read(), "text/html; charset=utf-8", extra={"Cache-Control": "no-cache"})
                if u.path == "/player" or u.path.startswith("/player/"):       # /player = every camera, /player/<name> = one camera
                    if u.path.count("/") > 2 and u.path != "/player/":
                        return self.send_bytes(b"not found", "text/plain", 404)
                    with open(os.path.join(HERE, "player.html"), "rb") as fh:
                        return self.send_bytes(fh.read(), "text/html; charset=utf-8", extra={"Cache-Control": "no-cache"})
                if u.path == "/api/list":
                    since = float(q["since"][0]) if q.get("since") else None
                    body = json.dumps(lib.listing(q["camera"][0] if q.get("camera") else None, since), separators=(",", ":")).encode()
                    if "gzip" in self.headers.get("Accept-Encoding", "") and len(body) > 1024:      # the list is repetitive text: ~10x smaller
                        return self.send_bytes(gzip.compress(body, 5), "application/json", extra={"Content-Encoding": "gzip", "Vary": "Accept-Encoding"})
                    return self.send_bytes(body, "application/json")
                if u.path == "/api/map":
                    p = lib.resolve(q["id"][0], ".mvmap")
                    if not p:
                        return self.send_bytes(b"no map", "text/plain", 404)
                    hdr, body = mvmap.read_raw(p)
                    h = json.dumps(hdr).encode()
                    return self.send_bytes(struct.pack("<I", len(h)) + h + body, "application/octet-stream")
                if u.path in ("/api/vecinfo", "/api/vec"):
                    # .mvvec motion field: 3 MB per 10 s once unpacked (more than the video!), so it travels as the
                    # file's own zlib stream with Content-Encoding: deflate and the browser unpacks it natively
                    mp4 = lib.resolve(q["id"][0], ".mp4")
                    if not mp4:
                        return self.send_bytes(b"not found", "text/plain", 404)
                    p = lib.ensure_vec(mp4)                # a recording without .mvvec gets it made here, on first use
                    with open(p, "rb") as fh:
                        raw = fh.read()
                    if raw[:8] != mvmap.MAGIC_VEC:
                        return self.send_bytes(b"not an mvvec file", "text/plain", 400)
                    (hl,) = struct.unpack_from("<I", raw, 8)
                    if u.path == "/api/vecinfo":
                        return self.send_bytes(raw[12:12 + hl], "application/json")
                    return self.send_bytes(raw[12 + hl:], "application/octet-stream", extra={"Content-Encoding": "deflate"})
                if u.path == "/api/tuneconfig" and q.get("name"):
                    path = self.cfg_path(q["name"][0])
                    data = open(path, "rb").read() if os.path.exists(path) else b"{}"
                    return self.send_bytes(data, "application/json")
                if u.path == "/api/audio":       # asked for the selected recording only (not in the listing)
                    p = lib.resolve(q["id"][0], ".mp4")
                    if not p:
                        return self.send_bytes(b"not found", "text/plain", 404)
                    return self.send_bytes(json.dumps({"audio": has_audio(p)}).encode(), "application/json")
                if u.path == "/video":
                    p = lib.resolve(q["id"][0], ".mp4")
                    if not p:
                        return self.send_bytes(b"not found", "text/plain", 404)
                    return self.send_file(p)
            except (BrokenPipeError, ConnectionResetError):
                return
            except Exception as e:
                return self.send_bytes(str(e).encode(), "text/plain", 500)
            self.send_bytes(b"not found", "text/plain", 404)

        def send_file(self, path):
            size = os.path.getsize(path)
            start, end, code = 0, size - 1, 200
            m = re.match(r"bytes=(\d*)-(\d*)", self.headers.get("Range", ""))
            if m:
                if m.group(1):
                    start = int(m.group(1))
                    if m.group(2):
                        end = min(int(m.group(2)), size - 1)
                elif m.group(2):
                    start = max(0, size - int(m.group(2)))
                code = 206
            self.send_response(code)
            self.send_header("Content-Type", "video/mp4")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(end - start + 1))
            if code == 206:
                self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
            self.end_headers()
            with open(path, "rb") as fh:
                fh.seek(start)
                left = end - start + 1
                while left > 0:
                    chunk = fh.read(min(1 << 20, left))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    left -= len(chunk)

    return H


def main():
    global CONFIG_DIR, LIVE, RTSPCAM
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", help="recordings root (the -o directory of the recorder)")
    ap.add_argument("--port", type=int, default=8780)
    ap.add_argument("--host", default="127.0.0.1", help="use 0.0.0.0 to reach it from other machines "
                                                          "(there is no authentication!)")
    ap.add_argument("--config-dir", default=CONFIG_DIR, help="where the tuning panel reads/writes <camera>.json")
    ap.add_argument("--rtspcam", default=RTSPCAM, help="the recorder binary; a recording that has no .mvvec when the tuning/vector view asks for it "
                                                       "gets it made with `rtspcam --vectors FILE.mp4` (default: src/rtspcam)")
    ap.add_argument("--live-dir", help="the --live directory of the C recorders: serves the live view too (/live, /status), "
                                       "the same one liveview.py serves on its own")
    ap.add_argument("--cameras", help="with --live-dir: comma separated order of the cameras in the live grid "
                                      "(default: the sockets, sorted)")
    a = ap.parse_args()
    CONFIG_DIR = a.config_dir
    RTSPCAM = a.rtspcam
    if a.cameras and not a.live_dir:
        ap.error("--cameras needs --live-dir")
    if a.live_dir:
        from liveview import Live
        LIVE = Live(a.live_dir, [c.strip() for c in a.cameras.split(",") if c.strip()] if a.cameras else None)
    lib = Library(a.root)
    threading.Thread(target=lib.warm_up, daemon=True).start()
    srv = ThreadingHTTPServer((a.host, a.port), make_handler(lib))
    print("player: http://%s:%d   root: %s" % (a.host, a.port, lib.root), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
