#!/usr/bin/env python3
"""onvif_encoder - read or set a camera's video encoder settings over ONVIF (standard library only).

    onvif_encoder.py CAMERA get
    onvif_encoder.py CAMERA set [--token VideoEncoder000] [--width 1920 --height 1080] [--fps 15]
                                [--bitrate 2048] [--gop 30] [--profile Main]

CAMERA is an rtsp://user:password@host/... URL (host and credentials are taken from it) or a
camera name from cameras.url ("<name> <rtsp url>" per line, next to this script or --cameras).
Without --token, `set` changes the first encoder (normally the main stream). Settings that are
not given stay as they are. The change is made persistent on the camera (ForcePersistence).

Useful for cameras without a web interface (e.g. Dahua/Imou), whose phone app may reset the
stream settings: run `set` again (or from cron) after the app has been used. `set` only writes
when a value differs (a write restarts the camera's encoder), so it is safe to run periodically.
"""
import argparse
import base64
import datetime
import hashlib
import os
import re
import socket
import sys
from urllib.parse import unquote, urlsplit

NS = ('xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tt="http://www.onvif.org/ver10/schema" '
      'xmlns:trt="http://www.onvif.org/ver10/media/wsdl" xmlns:tds="http://www.onvif.org/ver10/device/wsdl"')


class Camera:
    def __init__(self, host, port, user, password):
        self.host, self.port, self.user, self.password = host, port, user, password
        self.media = "/onvif/media_service"

    def _security(self):
        nonce = os.urandom(16)
        created = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        digest = base64.b64encode(hashlib.sha1(nonce + created.encode() + self.password.encode()).digest()).decode()
        return ('<s:Header><Security s:mustUnderstand="1" xmlns="http://docs.oasis-open.org/wss/2004/01/'
                'oasis-200401-wss-wssecurity-secext-1.0.xsd"><UsernameToken><Username>%s</Username>'
                '<Password Type="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0'
                '#PasswordDigest">%s</Password><Nonce EncodingType="http://docs.oasis-open.org/wss/2004/01/'
                'oasis-200401-wss-soap-message-security-1.0#Base64Binary">%s</Nonce><Created xmlns="http://docs.'
                'oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd">%s</Created>'
                '</UsernameToken></Security></s:Header>' % (self.user, digest, base64.b64encode(nonce).decode(), created))

    def call(self, path, body):
        env = '<?xml version="1.0" encoding="utf-8"?><s:Envelope %s>%s<s:Body>%s</s:Body></s:Envelope>' % (
            NS, self._security(), body)
        data = env.encode()
        s = socket.create_connection((self.host, self.port), timeout=15)
        s.sendall(("POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/soap+xml; charset=utf-8\r\n"
                   "Content-Length: %d\r\nConnection: close\r\n\r\n" % (path, self.host, len(data))).encode() + data)
        resp = b""
        while True:
            d = s.recv(65536)
            if not d:
                break
            resp += d
        s.close()
        text = resp.decode(errors="replace")
        status = text.split("\r\n", 1)[0]
        if " 200 " not in status:
            fault = re.search(r"<(?:\w+:)?Text[^>]*>(.*?)</", text, re.S)
            raise RuntimeError("%s %s" % (status, fault.group(1) if fault else ""))
        return text

    def find_media(self):
        caps = self.call("/onvif/device_service",
                         '<tds:GetCapabilities><tds:Category>Media</tds:Category></tds:GetCapabilities>')
        m = re.search(r"<tt:Media>.*?<tt:XAddr>(.*?)</tt:XAddr>", caps, re.S)
        if m:
            self.media = urlsplit(m.group(1)).path or self.media

    def configurations(self):
        """[(token, xml of the <tt:...Configurations> element)]"""
        text = self.call(self.media, "<trt:GetVideoEncoderConfigurations/>")
        out = []
        for m in re.finditer(r"<(\w+):Configurations\b(.*?)</\1:Configurations>", text, re.S):
            tok = re.search(r'token="([^"]+)"', m.group(2))
            out.append((tok.group(1) if tok else "?", m.group(0)))
        return out


def tag(xml, name):
    m = re.search(r"<tt:%s>(.*?)</tt:%s>" % (name, name), xml, re.S)
    return m.group(1) if m else "?"


def describe(tok, xml):
    res = re.search(r"<tt:Resolution>\s*<tt:Width>(\d+)</tt:Width>\s*<tt:Height>(\d+)</tt:Height>", xml)
    return "%-16s %-5s %5sx%-5s fps %-3s bitrate %-5s kbit/s  GOP %-4s profile %s" % (
        tok, tag(xml, "Encoding"), res.group(1) if res else "?", res.group(2) if res else "?",
        tag(xml, "FrameRateLimit"), tag(xml, "BitrateLimit"), tag(xml, "GovLength"), tag(xml, "H264Profile"))


def set_value(xml, name, value):
    new, n = re.subn(r"(<tt:%s>)(.*?)(</tt:%s>)" % (name, name), r"\g<1>%s\g<3>" % value, xml, count=1, flags=re.S)
    if not n:
        raise RuntimeError("the camera's configuration has no %s field" % name)
    return new


def camera_from(arg, cameras_file):
    url = arg
    if not arg.startswith("rtsp://"):
        try:
            lines = open(cameras_file).read().splitlines()
        except OSError:
            sys.exit("cannot read %s" % cameras_file)
        found = [l.split(None, 1)[1].strip() for l in lines if l.split(None, 1)[:1] == [arg]]
        if not found:
            sys.exit("camera %s not found in %s" % (arg, cameras_file))
        url = found[0]
    u = urlsplit(url)
    if not u.username:
        sys.exit("the URL has no user name/password")
    return Camera(u.hostname, 80, unquote(u.username), unquote(u.password or ""))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("camera", help="rtsp://user:password@host/... or a name from cameras.url")
    ap.add_argument("action", choices=["get", "set"])
    ap.add_argument("--cameras", default=os.path.join(here, "cameras.url"))
    ap.add_argument("--port", type=int, default=80, help="ONVIF HTTP port")
    ap.add_argument("--token", help="encoder configuration to change (default: the first one)")
    ap.add_argument("--width", type=int)
    ap.add_argument("--height", type=int)
    ap.add_argument("--fps", type=int)
    ap.add_argument("--bitrate", type=int, help="kbit/s")
    ap.add_argument("--gop", type=int, help="key frame interval in frames")
    ap.add_argument("--profile", choices=["Baseline", "Main", "High"])
    a = ap.parse_args()

    cam = camera_from(a.camera, a.cameras)
    cam.port = a.port
    try:
        cam.find_media()
        confs = cam.configurations()
    except (OSError, RuntimeError) as e:
        sys.exit("ONVIF error: %s" % e)
    if not confs:
        sys.exit("the camera reported no video encoder configuration")
    if a.action == "get":
        for tok, xml in confs:
            print(describe(tok, xml))
        return

    tok, xml = next(((t, x) for t, x in confs if t == a.token), (None, None)) if a.token else confs[0]
    if xml is None:
        sys.exit("no encoder configuration %s (have: %s)" % (a.token, ", ".join(t for t, _ in confs)))
    print("before:", describe(tok, xml))
    conf = xml
    if (a.width is None) != (a.height is None):
        sys.exit("give --width and --height together")
    if a.width:
        conf = set_value(conf, "Width", a.width)
        conf = set_value(conf, "Height", a.height)
    if a.fps:
        conf = set_value(conf, "FrameRateLimit", a.fps)
    if a.bitrate:
        conf = set_value(conf, "BitrateLimit", a.bitrate)
    if a.gop:
        conf = set_value(conf, "GovLength", a.gop)
    if a.profile:
        conf = set_value(conf, "H264Profile", a.profile)
    if conf == xml:
        print("nothing to change")            # writing would restart the encoder for nothing
        return
    # <trt:Configurations ...> (a list entry) -> <trt:Configuration ...> (the request element)
    conf = re.sub(r"^<(\w+):Configurations\b", r"<trt:Configuration", conf)
    conf = re.sub(r"</(\w+):Configurations>$", r"</trt:Configuration>", conf)
    try:
        cam.call(cam.media, "<trt:SetVideoEncoderConfiguration>%s<trt:ForcePersistence>true</trt:ForcePersistence>"
                            "</trt:SetVideoEncoderConfiguration>" % conf)
        after = dict(cam.configurations())
    except (OSError, RuntimeError) as e:
        sys.exit("ONVIF error: %s" % e)
    print("after: ", describe(tok, after.get(tok, "")))


if __name__ == "__main__":
    main()
