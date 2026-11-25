#!/bin/sh
# alarm-example.sh - sample --on-alarm script for the recorder (src/rtspcam), to be copied and adapted:
#
#     ./src/rtspcam URL -n Szeru2 -o rec ... --on-alarm /home/rtspcam/alarm.sh
#     (or in configs/<camera>.json: "on_alarm": "/home/rtspcam/alarm.sh")
#
# The recorder runs it in the background when an alarm starts a recording, and does not wait for it:
#     $1 = the camera name, $2 = the recording's path (rec/<camera>/<date>/<camera>_<HHMMSS>.mp4; it is written as
#          <path>.part until the recording ends)
# What it does: a still picture from the camera (its address from snapshots.url, fetched with curl at once - no ONVIF),
# a big one shrunk (djpeg -scale: cheap, from the JPEG's DCT, package libjpeg-turbo-progs) and saved next to the recording
# as <name>.jpg (the player and rotate.py handle it with the recording), then an e-mail with the picture and a link to
# the recording in the player. It runs at every alarm (every recording gets its picture); the mail goes at most once per
# camera per COOLDOWN minutes. POSIX sh (dash on Ubuntu), no bash needed.

# the recorder's environment has no locale, and mailx refuses non-ASCII text then ("Invalid or incomplete multibyte or
# wide character", the mail goes to dead.letter): the texts below are without accents, and UTF-8 in case one is added
export LC_ALL=C.UTF-8

MAILTO="you@example.com"                     # who gets the mail
PLAYER="http://player.example.com:8888"      # the player as the recipients reach it
COOLDOWN=10                                  # minutes between two mails of the same camera
# the cameras' still picture addresses: one "<camera> <url> [scale]" per line, the user and password in the URL (curl
# --anyauth picks digest or basic); scale: 1/2, 1/4 or 1/8 to shrink the picture (djpeg -scale), none or 1: as it is.
# The addresses come from "python3 onvif_encoder.py <camera> url" (the "snapshot:" lines), e.g.
#   Szeru2 http://admin:PASSWORD@192.168.1.108/cgi-bin/snapshot.cgi?channel=1 1/4
#   Hikvision http://admin:PASSWORD@192.168.1.64/ISAPI/Streaming/channels/101/picture 1/4
#   Eteto http://admin:PASSWORD@192.168.1.65/onvifsnapshot/media_service/snapshot?channel=1
# a camera that is not listed gets no picture (the mail goes without it)
SNAPSHOTS=/home/rtspcam/snapshots.url

cam="$1"
path="$2"
[ -n "$cam" ] || exit 1
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

# the picture: from the camera, shrunk as snapshots.url says (1/4: 1920x1080 -> 480x270, ~40 KB)
jpg=""
line=$(awk -v c="$cam" '$1 == c {print $2, $3; exit}' "$SNAPSHOTS" 2>/dev/null)
url=${line%% *}
scale=${line#* }
[ "$scale" = "$line" ] && scale=""
if [ -n "$url" ] && curl -s -f --anyauth --max-time 5 -o "$tmp/big.jpg" "$url"; then
    if [ -z "$scale" ] || [ "$scale" = 1 ]; then
        jpg="$tmp/big.jpg"
    elif djpeg -scale $scale "$tmp/big.jpg" | cjpeg -quality 80 > "$tmp/small.jpg" 2>/dev/null && [ -s "$tmp/small.jpg" ]; then
        jpg="$tmp/small.jpg"
    fi
    [ -n "$jpg" ] && [ -n "$path" ] && cp "$jpg" "${path%.mp4}.jpg"     # next to the recording
fi

# the link: /player/<camera>#<camera>/<date>/<name> (the last three parts of the path, without .mp4)
link=""
if [ -n "$path" ]; then
    id=$(echo "${path%.mp4}" | awk -F/ '{print $(NF-2) "/" $(NF-1) "/" $NF}')
    link="$PLAYER/player/$cam#$id"
fi

# the mail: at most one per camera per COOLDOWN minutes (the picture above is saved anyway)
stamp="/tmp/rtspcam-alarm-$cam"
if [ -e "$stamp" ] && [ -z "$(find "$stamp" -mmin +$COOLDOWN 2>/dev/null)" ]; then
    exit 0
fi
touch "$stamp"

{
    echo "Riasztas: $cam"
    date '+%Y-%m-%d %H:%M:%S'
    [ -n "$link" ] && echo "A felvetel: $link"
    [ -z "$jpg" ] && echo "(a kamera nem ad allokepet)"
} > "$tmp/body.txt"
if [ -n "$jpg" ]; then
    mailx -a "$jpg" -s "Riasztas: $cam" "$MAILTO" < "$tmp/body.txt"
else
    mailx -s "Riasztas: $cam" "$MAILTO" < "$tmp/body.txt"
fi
