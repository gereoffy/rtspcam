#!/bin/bash
# Live view test: starts a C recorder with --live for the given cameras (default: all in
# cameras.url) and the liveview.py web server on the public address (port 8781).
#   ./start_live.sh                 all cameras
#   ./start_live.sh Dahua-Fix Hikvision
# Stop: ./stop_live.sh
cd "$(dirname "$0")" || exit 1
LIVE=${LIVE:-rclive}
OUT=${OUT:-rec}
ARGS=${ARGS:---mv-min 5 --min-cluster 40 --pre-roll 1 --post-roll 3 --vectors}
HOST=${HOST:-127.0.0.1}
mkdir -p "$LIVE" "$OUT/logs"
cams=("$@")
[ ${#cams[@]} -eq 0 ] && cams=($(awk 'NF >= 2 { print $1 }' cameras.url))
for cam in "${cams[@]}"; do
    url=$(awk -v n="$cam" '$1 == n { print $2 }' cameras.url)
    [ -z "$url" ] && { echo "unknown camera $cam"; continue; }
    extra=$(awk -v n="$cam" '$1 == n { $1 = ""; print }' rotate_args.conf 2>/dev/null)
    echo src/rtspcam "$url" -n "$cam" -o "$OUT" --live "$LIVE" $ARGS $extra
    ./src/rtspcam "$url" -n "$cam" -o "$OUT" --live "$LIVE" $ARGS $extra >> "$OUT/logs/$cam.log" 2>&1 < /dev/null &
done
order=$(IFS=,; echo "${cams[*]}")
#python3 liveview.py --live-dir "$LIVE" --host "$HOST" --port 8888 --cameras "$order" > "$LIVE/liveview.log" 2>&1 < /dev/null &
python3 player.py --live-dir "$LIVE" --host "$HOST" --port 8888 --cameras "$order" > "$LIVE/player.log" 2>&1 < /dev/null &
sleep 3
echo "recorders: $(pgrep -f '^\./src/rtspcam rtsp' | wc -l | tr -d ' '), live view: http://$HOST:8888/"
