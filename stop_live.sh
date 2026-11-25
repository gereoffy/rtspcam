#!/bin/bash
# stops the recorders and the player (with the live view) started by start_live.sh (recordings are closed cleanly);
# a separately started liveview.py too
pkill -INT -f '^\./src/rtspcam rtsp'
pkill -f '(player|liveview)[.]py .*--live-dir'   # (on macOS the process is /.../Python, not python3)
sleep 2
echo "recorders left: $(pgrep -f '^\./src/rtspcam rtsp' | wc -l | tr -d ' '), player/live view left: $(pgrep -f '(player|liveview)[.]py .*--live-dir' | wc -l | tr -d ' ')"
