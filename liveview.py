#!/usr/bin/env python3
"""liveview - live view of the cameras recorded by the C rtspcam (started with --live DIR).

    python3 liveview.py --live-dir DIR [--host 0.0.0.0] [--port 8781] [--cameras a,b,c]

Every recorder offers its camera's stream on DIR/<camera>.sock (fragmented MP4, one fragment
per picture, browser-ready SPS). This server reads each socket only while somebody watches
that camera, with ONE connection however many viewers there are: it keeps the init segment
and the pictures since the last key frame, so a new viewer starts at once, and copies the
stream to every viewer (GET /live/<camera>); a viewer that cannot keep up is dropped. The
page shows the cameras in a grid (3x3 for 9 cameras); a click shows one camera large. The
browser decodes the H.264 itself (Media Source Extensions): no decoding or re-encoding here.
A red frame marks a camera in alarm (its motion detector triggered), an orange one a recording
that still runs after the alarm (post-roll), a grey "camera unreachable" overlay a camera the
recorder cannot reach (its last pictures stay on the tile); GET /status, polled by the page
every second (the recorders send the state every 2 s, also while the camera is away). Resolution, fps and latency are in the tile's tooltip.
Pages (next to this script, read on every request): / is index.html (info/menu),
/live is liveview.html (the grid). The same live view can run inside player.py (--live-dir DIR there):
class Live is the shared part (/status, /live, /live/<camera>).
Standard library only.
"""
import argparse
import json
import os
import queue
import select
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn

try:
    from http.server import ThreadingHTTPServer
except ImportError:                     # Python < 3.7 (e.g. Ubuntu 16.04: 3.5)
    class ThreadingHTTPServer(ThreadingMixIn, HTTPServer):
        daemon_threads = True
from urllib.parse import unquote

INIT, KEY, DELTA, STATUS = 1, 2, 3, 4
END = None                      # queue sentinel: the stream ended (or restarts), close the response


def browser_gone(conn):
    """the viewer closed its connection (it sends nothing while it streams: readable means EOF)"""
    try:
        r, _, _ = select.select([conn], [], [], 0)
        return bool(r) and not conn.recv(1, socket.MSG_PEEK)
    except OSError:
        return True


class Hub:
    """One recorder socket, shared by all viewers of that camera."""

    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        self.subs = []
        self.init = None
        self.gop = []           # messages since the last key frame
        self.thread = None
        self.status = None      # (alarm, recording, time received, online); only while somebody watches
        self.connected = False  # reading the recorder's socket: the recorder runs

    def subscribe(self):
        q = queue.Queue(maxsize=600)            # ~40 s of pictures; a viewer that far behind is dropped
        with self.lock:
            if self.init is not None:
                q.put(self.init)
                for m in self.gop:
                    q.put(m)
            self.subs.append(q)
            if self.thread is None or not self.thread.is_alive():
                self.thread = threading.Thread(target=self.run, daemon=True)
                self.thread.start()
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.subs:
                self.subs.remove(q)

    def _broadcast(self, data):
        for q in list(self.subs):
            try:
                q.put_nowait(data)
            except queue.Full:
                self.subs.remove(q)
                try:
                    q.get_nowait()
                    q.put_nowait(END)
                except (queue.Empty, queue.Full):
                    pass

    def run(self):
        """reads the socket while there are viewers; reconnects if the recorder restarts"""
        while True:
            with self.lock:
                if not self.subs:
                    self.init, self.gop = None, []
                    return
            idle = False            # stopped because nobody watches (not because the recorder is gone)
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(60)    # a status comes every 2 s, but connecting to a dead camera blocks ~10 s
                s.connect(self.path)
                self.connected = True
                f = s.makefile("rb")
                while True:
                    hdr = f.read(5)
                    if len(hdr) < 5:
                        break
                    typ, n = struct.unpack(">BI", hdr)
                    data = f.read(n)
                    if len(data) < n:
                        break
                    if typ == STATUS:                   # not video: kept for /status
                        if len(data) >= 2:              # older recorders send 2 bytes (no online flag)
                            online = data[2] != 0 if len(data) >= 3 else True
                            self.status = (bool(data[0]), bool(data[1]), time.time(), online)
                        continue
                    with self.lock:
                        if not self.subs:
                            idle = True
                            break
                        if typ == INIT:
                            if self.init is not None:   # new stream parameters: viewers start over
                                for q in self.subs:
                                    try:
                                        q.put_nowait(END)
                                    except queue.Full:
                                        pass
                                self.subs = []
                            self.init, self.gop = data, []
                        elif typ == KEY:
                            self.gop = [data]
                        elif self.gop:
                            self.gop.append(data)
                        self._broadcast(data)
                s.close()
            except OSError:
                pass
            with self.lock:                     # the recorder is gone (restart, reconnect)
                self.connected = False
                if not idle:
                    self.status = None          # its last status is no longer true
                self.init, self.gop = None, []
                for q in self.subs:
                    try:
                        q.put_nowait(END)
                    except queue.Full:
                        pass
                self.subs = []
                return


HERE = os.path.dirname(os.path.abspath(__file__))


class Live:
    """The live view as a part that any BaseHTTPRequestHandler can serve (this script's own server, or player.py with --live-dir):
    handle(h, path) answers /status, /live (the grid page) and /live/<camera> (the stream) and returns False for every other path."""

    def __init__(self, live_dir, cams=None):
        if not cams:
            cams = sorted(f[:-5] for f in os.listdir(live_dir) if f.endswith(".sock"))
        self.cams = cams
        self.hubs = {c: Hub(os.path.join(live_dir, c + ".sock")) for c in cams}

    @staticmethod
    def _send(h, body, ctype, extra=()):
        h.send_response(200)
        h.send_header("Content-Type", ctype)
        for k, v in extra:
            h.send_header(k, v)
        h.send_header("Content-Length", str(len(body)))
        h.end_headers()
        h.wfile.write(body)

    def status(self):
        now = time.time()
        st = {}
        for name, hub in self.hubs.items():
            s = hub.status
            fresh = s is not None and now - s[2] < 15   # the recorder repeats it every 2 s (also while reconnecting)
            if fresh:
                st[name] = {"alarm": s[0], "rec": s[1], "known": True, "online": s[3]}
            else:
                # no recent status: if its socket is open the recorder runs but has no picture (it is
                # connecting to a camera that answers slowly, or waits for a key frame); otherwise
                # the recorder does not run
                st[name] = {"alarm": False, "rec": False, "known": hub.connected, "online": False}
        return json.dumps(st).encode()

    def handle(self, h, path):
        if path == "/status":
            self._send(h, self.status(), "application/json", [("Cache-Control", "no-store")])
            return True
        if path in ("/live", "/live/"):
            with open(os.path.join(HERE, "liveview.html"), "rb") as fh:     # read every time: edits need no restart
                body = fh.read().replace(b"__CAMS__", json.dumps(self.cams).encode())
            self._send(h, body, "text/html; charset=utf-8", [("Cache-Control", "no-cache")])
            return True
        if path.startswith("/live/"):
            hub = self.hubs.get(unquote(path[6:]))
            if hub is None:
                h.send_error(404)
                return True
            q = hub.subscribe()
            h.send_response(200)
            h.send_header("Content-Type", "video/mp4")
            h.send_header("Cache-Control", "no-store")
            h.send_header("Transfer-Encoding", "chunked")
            h.end_headers()
            try:
                while True:
                    try:
                        data = q.get(timeout=5)
                    except queue.Empty:
                        # no picture: the camera is away. Keep the stream open while the recorder
                        # runs (it continues when the camera is back), unless the browser left
                        if not hub.connected or browser_gone(h.connection):
                            break
                        continue
                    if data is END:
                        break
                    h.wfile.write(b"%x\r\n%s\r\n" % (len(data), data))
                h.wfile.write(b"0\r\n\r\n")
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                hub.unsubscribe(q)
            h.close_connection = True
            return True
        return False


class Handler(BaseHTTPRequestHandler):
    live = None
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if self.live.handle(self, path):
            return
        if path == "/favicon.ico":
            try:
                with open(os.path.join(HERE, "favicon.ico"), "rb") as fh:
                    Live._send(self, fh.read(), "image/x-icon", [("Cache-Control", "max-age=86400")])
            except OSError:
                self.send_error(404)
            return
        if path in ("/", "/index.html"):
            with open(os.path.join(HERE, "index.html"), "rb") as fh:
                Live._send(self, fh.read(), "text/html; charset=utf-8", [("Cache-Control", "no-cache")])
            return
        self.send_error(404)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--live-dir", required=True, help="the --live directory of the recorders")
    ap.add_argument("--host", default="127.0.0.1", help="0.0.0.0 to reach it from other machines (trusted network only)")
    ap.add_argument("--port", type=int, default=8781)
    ap.add_argument("--cameras", help="comma separated order of the cameras (default: the sockets, sorted)")
    a = ap.parse_args()
    cams = [c.strip() for c in a.cameras.split(",") if c.strip()] if a.cameras else None
    Handler.live = Live(a.live_dir, cams)
    cams = Handler.live.cams
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    srv.daemon_threads = True
    print("live view of %s on http://%s:%d/live" % (", ".join(cams) or "(no cameras)", a.host, a.port), flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
