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
# shrunk (djpeg -scale: cheap, from the JPEG's DCT, package libjpeg-turbo-progs), the first one saved next to the
# recording as <name>.jpg (the player and rotate.py handle it with the recording), then an e-mail with COUNT pictures
# INTERVAL s apart and a link to the recording in the player. It runs at every alarm (every recording gets its
# picture); the mail goes at most once per camera per COOLDOWN minutes. POSIX sh (dash on Ubuntu), no bash needed.

# the recorder's environment has no locale, and mailx refuses non-ASCII text then ("Invalid or incomplete multibyte or
# wide character", the mail goes to dead.letter): the texts below are without accents, and UTF-8 in case one is added
export LC_ALL=C.UTF-8

MAILTO="you@example.com"                     # who gets the mail
PLAYER="http://player.example.com:8888"      # the player as the recipients reach it
COOLDOWN=10                                  # minutes between two mails of the same camera
COUNT=3                                      # pictures in a mail ...
INTERVAL=1                                   # ... this many seconds apart (fractions are fine: GNU sleep)
DELAY=0.5                                    # seconds after the alarm before the first one
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

# the mail: at most one per camera per COOLDOWN minutes (the recording's picture is made anyway)
mail=1
stamp="/tmp/rtspcam-alarm-$cam"
if [ -e "$stamp" ] && [ -z "$(find "$stamp" -mmin +$COOLDOWN 2>/dev/null)" ]; then
    mail=""
else
    touch "$stamp"
fi

# the pictures: COUNT of them INTERVAL s apart (the first DELAY s after the alarm; without a mail only that one), each
# shrunk as snapshots.url says (1/4: 1920x1080 -> 480x270, ~40 KB); the first one goes next to the recording
line=$(awk -v c="$cam" '$1 == c {print $2, $3; exit}' "$SNAPSHOTS" 2>/dev/null)
url=${line%% *}
scale=${line#* }
[ "$scale" = "$line" ] && scale=""
n=$COUNT
[ -z "$mail" ] && n=1
got=0
i=1
sleep $DELAY
while [ -n "$url" ] && [ $i -le $n ]; do
    [ $i -gt 1 ] && sleep $INTERVAL
    if curl -s -f --anyauth --max-time 5 -o "$tmp/big$i.jpg" "$url"; then
        if [ -z "$scale" ] || [ "$scale" = 1 ]; then
            mv "$tmp/big$i.jpg" "$tmp/pic$i.jpg"
        else
            djpeg -scale $scale "$tmp/big$i.jpg" | cjpeg -quality 80 > "$tmp/pic$i.jpg" 2>/dev/null
        fi
        if [ -s "$tmp/pic$i.jpg" ]; then
            got=$((got + 1))
            [ $got -eq 1 ] && [ -n "$path" ] && cp "$tmp/pic$i.jpg" "${path%.mp4}.jpg"     # next to the recording
        fi
    fi
    i=$((i + 1))
done
[ -n "$mail" ] || exit 0

# the link: /player/<camera>#<camera>/<date>/<name> (the last three parts of the path, without .mp4)
link=""
if [ -n "$path" ]; then
    id=$(echo "${path%.mp4}" | awk -F/ '{print $(NF-2) "/" $(NF-1) "/" $NF}')
    link="$PLAYER/player/$cam#$id"
fi

{
    echo "Riasztas: $cam"
    date '+%Y-%m-%d %H:%M:%S'
    [ -n "$link" ] && echo "A felvetel: $link"
    [ $got -eq 0 ] && echo "(a kamera nem ad allokepet)"
} > "$tmp/body.txt"
set --                                     # the pictures as mailx -a options
for f in "$tmp"/pic*.jpg; do
    [ -s "$f" ] && set -- "$@" -a "$f"
done
mailx "$@" -s "Riasztas: $cam" "$MAILTO" < "$tmp/body.txt"
