#!/usr/bin/env python3
"""onvif_encoder - read or set a camera's video encoder settings over ONVIF (standard library only).

    onvif_encoder.py CAMERA get
    onvif_encoder.py CAMERA options       (what the camera allows: resolutions, fps, GOP, quality per stream)
    onvif_encoder.py CAMERA time          (the camera's clock, time zone, DST flag; against this machine)
    onvif_encoder.py CAMERA settime --tz "CET-1CEST,M3.5.0,M10.5.0/3"   (zone with DST rules)
    onvif_encoder.py CAMERA settime --fixed   (this machine's current offset: for cameras without DST rules, daily)
    onvif_encoder.py CAMERA set [--token VideoEncoder000] [--width 1920 --height 1080] [--fps 15]
                                [--bitrate 2048] [--quality 5] [--gop 30] [--profile Main]

CAMERA is an rtsp://user:password@host/... URL (host and credentials are taken from it) or a
camera name from cameras.url ("<name> <rtsp url>" per line, next to this script or --cameras).
Without --token, `set` changes the first encoder (normally the main stream). Settings that are
not given stay as they are. The change is made persistent on the camera (ForcePersistence).

Most cameras encode with a variable bit rate (VBR): then --bitrate is only the upper limit and a
quiet picture gets far less (an Imou set to 2048 kbit/s sent 240); the picture quality (blocks
when zoomed in) follows --quality (Dahua/Imou: 1-6). The live view shows the real bit rate.

Useful for cameras without a web interface (e.g. Dahua/Imou), whose phone app may reset the
stream settings: run `set` again (or from cron) after the app has been used. `set` only writes
when a value differs (a write restarts the camera's encoder), so it is safe to run periodically.

Without --port the usual ONVIF ports are tried: 80 (Dahua, Hikvision), 8899 (cheap Chinese
"IPCAM / HS-Camera" firmware) and 6688. Some cameras (the HS-Camera ones) reject the WS-Security
user name token with an HTTP 500 but answer without it: then the requests go unauthenticated
(said on stderr; --no-auth forces it).
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
PORTS = (80, 8899, 6688)      # tried in this order without --port


class Camera:
    def __init__(self, host, port, user, password):
        self.host, self.port, self.user, self.password = host, port, user, password
        self.media = "/onvif/media_service"
        self.auth = None        # None: not known yet (with the token first, without it if refused)

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
        if self.auth is not None:
            return self._call(path, body, self.auth)
        try:
            text = self._call(path, body, True)
            self.auth = True
            return text
        except RuntimeError as e:
            if " 500 " not in str(e):
                raise
            try:                # the HS-Camera firmware: HTTP 500 for the token, fine without it
                text = self._call(path, body, False)
            except (OSError, RuntimeError):
                raise e
            self.auth = False
            print("note: %s:%d refuses the ONVIF user name token but answers without it: "
                  "continuing unauthenticated" % (self.host, self.port), file=sys.stderr)
            return text

    def _call(self, path, body, auth):
        env = '<?xml version="1.0" encoding="utf-8"?><s:Envelope %s>%s<s:Body>%s</s:Body></s:Envelope>' % (
            NS, self._security() if auth else "", body)
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
        m = re.search(r"<(?:\w+:)?Media>.*?<(?:\w+:)?XAddr>(.*?)</(?:\w+:)?XAddr>", caps, re.S)
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
    q = tag(xml, "Quality")
    try:
        q = "%g" % float(q)                 # (Dahua writes 4.000000)
    except ValueError:
        pass
    return "%-16s %-5s %5sx%-5s fps %-3s bitrate %-5s kbit/s  quality %-3s GOP %-4s profile %s" % (
        tok, tag(xml, "Encoding"), res.group(1) if res else "?", res.group(2) if res else "?",
        tag(xml, "FrameRateLimit"), tag(xml, "BitrateLimit"), q, tag(xml, "GovLength"), tag(xml, "H264Profile"))


def describe_options(cam, tok):
    """what the camera allows for one encoder configuration (GetVideoEncoderConfigurationOptions): H.264
    resolutions, frame rate, GOP, quality, profiles, and the bit rate range if it tells it"""
    body = ("<trt:GetVideoEncoderConfigurationOptions><trt:ConfigurationToken>%s</trt:ConfigurationToken>"
            "</trt:GetVideoEncoderConfigurationOptions>" % tok)
    try:
        text = cam.call(cam.media, body)
    except RuntimeError:                    # some cameras answer only without a token (then for all streams)
        text = cam.call(cam.media, "<trt:GetVideoEncoderConfigurationOptions/>")
    text = re.sub(r"<(/?)[\w-]+:", r"<\1", text)            # (the prefixes differ by camera)

    def rng(xml, name):
        m = re.search(r"<%s>\s*<Min>([^<]*)</Min>\s*<Max>([^<]*)</Max>" % name, xml)
        return "%s-%s" % (m.group(1), m.group(2)) if m else "?"

    h264 = re.search(r"<H264>(.*?)</H264>", text, re.S)
    h = h264.group(1) if h264 else ""
    res = ", ".join("%sx%s" % m for m in re.findall(r"<ResolutionsAvailable>\s*<Width>(\d+)</Width>\s*"
                                                     r"<Height>(\d+)</Height>", h)) or "?"
    ext = re.search(r"<Extension>.*?<H264>(.*?)</H264>", text, re.S)       # (Media 1 extension: the bit rate range)
    bitrate = rng(ext.group(1), "BitrateRange") if ext else "?"
    # (plain ASCII: the small site PC's terminal and Python 3.5 cannot print anything else)
    return ("%s\n    resolutions: %s\n    fps %s, GOP %s, quality %s, bitrate %s kbit/s, profiles %s" % (
        tok, res, rng(h, "FrameRateRange"), rng(h, "GovLengthRange"), rng(text, "QualityRange"),
        bitrate, ", ".join(re.findall(r"<H264ProfilesSupported>([^<]*)<", h)) or "?"))


def set_value(xml, name, value):
    new, n = re.subn(r"(<tt:%s>)(.*?)(</tt:%s>)" % (name, name), r"\g<1>%s\g<3>" % value, xml, count=1, flags=re.S)
    if not n:
        raise RuntimeError("the camera's configuration has no %s field" % name)
    return new


# ---------------------------------------------------------------- the camera's clock

HU_TZ = "CET-1CEST,M3.5.0,M10.5.0/3"     # POSIX: Central European time with its DST rules


def get_time(cam):
    """the camera's date/time settings (GetSystemDateAndTime): a dict, prefixes stripped"""
    t = re.sub(r"<(/?)[\w-]+:", r"<\1", cam.call("/onvif/device_service", "<tds:GetSystemDateAndTime/>"))

    def dt(name):
        m = re.search(r"<%s>.*?<Hour>(\d+)</Hour>\s*<Minute>(\d+)</Minute>\s*<Second>(\d+)</Second>.*?"
                      r"<Year>(\d+)</Year>\s*<Month>(\d+)</Month>\s*<Day>(\d+)</Day>" % name, t, re.S)
        if not m:
            return None
        h, mi, s, y, mo, d = map(int, m.groups())
        return datetime.datetime(y, mo, d, h, mi, s)

    def val(name):
        m = re.search(r"<%s>([^<]*)</%s>" % (name, name), t)
        return m.group(1).strip() if m else None
    return {"type": val("DateTimeType"), "dst": val("DaylightSavings"), "tz": val("TZ"),
            "utc": dt("UTCDateTime"), "local": dt("LocalDateTime")}


def tz_offset_min(tz):
    """east-positive minutes of a fixed time zone string: "GMT+01:00" (Dahua/Imou) or POSIX "CET-1"/"UTC-2"
    (POSIX counts west-positive); None if it is not a simple fixed offset"""
    m = re.match(r"^(?:GMT|UTC)([+-])(\d{1,2}):(\d{2})$", tz or "")         # (Dahua style: always hh:mm)
    if m:
        return (1 if m.group(1) == "+" else -1) * (int(m.group(2)) * 60 + int(m.group(3)))
    m = re.match(r"^[A-Za-z]{3,}([+-]?)(\d{1,2})(?::(\d{2}))?$", tz or "")
    if m:
        return (-1 if m.group(1) != "-" else 1) * (int(m.group(2)) * 60 + int(m.group(3) or 0))
    return None


def fixed_tz(offset_min, like):
    """the fixed zone for offset_min written the way the camera writes its own (like)"""
    sign, a = ("+" if offset_min >= 0 else "-"), abs(offset_min)
    if re.match(r"^(GMT|UTC)[+-]\d{1,2}:\d{2}$", like or ""):
        return "%s%s%02d:%02d" % (like[:3], sign, a // 60, a % 60)
    name = {60: "CET", 120: "CEST"}.get(offset_min, "LOC")       # POSIX: the sign counts west-positive
    return "%s%s%d%s" % (name, "-" if offset_min >= 0 else "+", a // 60, ":%02d" % (a % 60) if a % 60 else "")


def describe_time(cam):
    tm = get_time(cam)
    now_utc = datetime.datetime.utcnow()
    host_off = int(round((datetime.datetime.now() - now_utc).total_seconds() / 60.0))
    lines = ["mode %s, daylight saving flag %s, time zone %s" % (tm["type"], tm["dst"], tm["tz"])]
    if tm["utc"]:
        lines.append("camera UTC   %s  (this machine: %+.0f s)" % (tm["utc"], (tm["utc"] - now_utc).total_seconds()))
    local, how = tm["local"], ""
    if not local and tm["utc"]:
        off = tz_offset_min(tm["tz"])
        if off is not None:              # (no local time reported: from the zone, + 1 h with the DST flag)
            off += 60 if (tm["dst"] or "").lower() == "true" else 0
            local, how = tm["utc"] + datetime.timedelta(minutes=off), " (computed from the zone)"
    if local:
        lines.append("camera local %s%s  (this machine's local time: %+.0f s)" % (
            local, how, (local - (now_utc + datetime.timedelta(minutes=host_off))).total_seconds()))
    lines.append("this machine: UTC%+d:%02d" % (host_off // 60 if host_off >= 0 else -(-host_off // 60), abs(host_off) % 60))
    return "\n".join(lines)


def set_time(cam, tz=None, fixed=False, dst=None):
    """SetSystemDateAndTime keeping the mode (NTP/manual) and, for NTP, leaving the clock alone. fixed: the zone
    is this machine's current offset (winter +1, summer +2), the DST flag off, manual mode with this machine's
    time, written every time: for cameras that get DST wrong (run it daily from cron). Otherwise it writes
    only when something differs."""
    tm = get_time(cam)
    mode = tm["type"] or "NTP"
    if fixed:       # manual mode, always written: the clock is set every time (cron), no NTP, no DST rules
        host_off = int(round((datetime.datetime.now() - datetime.datetime.utcnow()).total_seconds() / 60.0))
        tz, dst, mode = fixed_tz(host_off, tm["tz"]), False, "Manual"
    new_tz = tz if tz is not None else tm["tz"]
    new_dst = dst if dst is not None else (tm["dst"] or "").lower() == "true"
    if not fixed and new_tz == tm["tz"] and new_dst == ((tm["dst"] or "").lower() == "true"):
        return False
    body = ("<tds:SetSystemDateAndTime><tds:DateTimeType>%s</tds:DateTimeType><tds:DaylightSavings>%s"
            "</tds:DaylightSavings><tds:TimeZone><tt:TZ>%s</tt:TZ></tds:TimeZone>" % (
                mode, "true" if new_dst else "false", new_tz))
    if mode.lower() == "manual":
        u = datetime.datetime.utcnow()       # (manual: the time must be given too; this machine's, NTP-synced)
        body += ("<tds:UTCDateTime><tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second>"
                 "</tt:Time><tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date>"
                 "</tds:UTCDateTime>" % (u.hour, u.minute, u.second, u.year, u.month, u.day))
    cam.call("/onvif/device_service", body + "</tds:SetSystemDateAndTime>")
    return True


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
    ap.add_argument("action", choices=["get", "set", "options", "time", "settime"],
                    help="get: the settings; set: change them; options: what the camera allows per stream; "
                         "time: the camera's clock and time zone; settime: set the time zone (--tz, --fixed, --dst)")
    ap.add_argument("--tz", help="settime: time zone, e.g. %s (POSIX, with the DST rules: for cameras that "
                                 "follow them) or GMT+01:00" % HU_TZ)
    ap.add_argument("--fixed", action="store_true", help="settime: this machine's current offset (winter +1, "
                    "summer +2) as a fixed zone, DST flag off, manual mode with this machine's time (written every "
                    "run): for cameras that get DST wrong; run it daily from cron")
    ap.add_argument("--dst", choices=["on", "off"], help="settime: the daylight saving flag")
    ap.add_argument("--cameras", default=os.path.join(here, "cameras.url"))
    ap.add_argument("--port", type=int, help="ONVIF HTTP port (default: try %s)" % ", ".join(map(str, PORTS)))
    ap.add_argument("--no-auth", action="store_true", help="send the requests without the user name token")
    ap.add_argument("--token", help="encoder configuration to change (default: the first one)")
    ap.add_argument("--width", type=int)
    ap.add_argument("--height", type=int)
    ap.add_argument("--fps", type=int)
    ap.add_argument("--bitrate", type=int, help="kbit/s")
    ap.add_argument("--gop", type=int, help="key frame interval in frames")
    ap.add_argument("--profile", choices=["Baseline", "Main", "High"])
    ap.add_argument("--quality", type=float, help="encoding quality (Dahua/Imou: 1-6, higher is better). With variable bit "
                    "rate (VBR) this sets how much the picture gets, the bit rate is only the upper limit")
    a = ap.parse_args()

    cam = camera_from(a.camera, a.cameras)
    if a.no_auth:
        cam.auth = False
    errors = []
    for port in [a.port] if a.port else PORTS:
        cam.port = port
        try:
            cam.find_media()
            confs = cam.configurations()
            break
        except (OSError, RuntimeError) as e:
            errors.append("port %d: %s" % (port, e))
            cam.auth = False if a.no_auth else None
    else:
        sys.exit("ONVIF error: %s" % "; ".join(errors))
    if not a.port and cam.port != PORTS[0]:
        print("note: ONVIF on port %d" % cam.port, file=sys.stderr)
    if a.action in ("time", "settime"):
        try:
            if a.action == "settime":
                if not (a.tz or a.fixed or a.dst):
                    sys.exit("settime: give --tz, --fixed or --dst")
                print("before:", describe_time(cam).replace("\n", "\n        "))
                changed = set_time(cam, tz=a.tz, fixed=a.fixed, dst=None if a.dst is None else a.dst == "on")
                print("after: " if changed else "nothing to change", describe_time(cam).replace("\n", "\n        ")
                      if changed else "")
            else:
                print(describe_time(cam))
        except (OSError, RuntimeError) as e:
            sys.exit("ONVIF error: %s" % e)
        return
    if not confs:
        sys.exit("the camera reported no video encoder configuration")
    if a.action == "get":
        for tok, xml in confs:
            print(describe(tok, xml))
        return
    if a.action == "options":
        for tok, _ in confs:
            try:
                print(describe_options(cam, tok))
            except (OSError, RuntimeError) as e:
                print("%s\n    options: ONVIF error: %s" % (tok, e))
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
    if a.quality is not None:
        try:
            same = float(tag(conf, "Quality")) == a.quality      # (4.000000 == 4: no needless write)
        except ValueError:
            same = False
        if not same:
            conf = set_value(conf, "Quality", "%g" % a.quality)
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
