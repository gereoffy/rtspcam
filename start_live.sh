#! /bin/bash

(cd src; make)

HOST=127.0.0.1
PORT=8888

OUT=rec
LIVE=rclive
LOG=log

cd "$(dirname "$0")" || exit 1
mkdir -p "$OUT" "$LIVE" "$LOG"

cat cameras.url |while read line; do
    NEV=$(echo $line|cut -d ' ' -f 1)
    URL=$(grep "^$NEV " cameras.url|cut -d ' ' -f 2)
#    echo "'$NEV'" "'$URL'"
    # the camera's own thresholds/zones, saved by the player's tuning panel; config file overrides cmdline $ARGS
    if test -f "configs/$NEV.json"; then
        CFG="configs/$NEV.json"
    else
        CFG="configs/DEFAULT.json"
    fi
    echo ./src/rtspcam "$URL" -n "$NEV" -o "$OUT" --live "$LIVE" --config "$CFG" --vectors --fix off --audio '>>' "$LOG/$NEV.log"
    ./src/rtspcam "$URL" -n "$NEV" -o "$OUT" --live "$LIVE" --config "$CFG" --vectors --fix off --audio >> "$LOG/$NEV.log" 2>&1 < /dev/null &
done

CAMS=$(cut -d ' ' -f 1 <cameras.url)
ORDER=$(echo $CAMS|tr ' ' ',')
echo python3 player.py --live-dir "$LIVE" --host "$HOST" --port "$PORT" --cameras "$ORDER" "$OUT" '>>' "$LOG/player.log"
python3 player.py --live-dir "$LIVE" --host "$HOST" --port "$PORT" --cameras "$ORDER" "$OUT" >> "$LOG/player.log" 2>&1 < /dev/null &
