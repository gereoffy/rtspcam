#!/bin/bash
# stops the recorders and the live view started by start_live.sh (recordings are closed cleanly)
pkill -INT -f '^\./src/rtspcam rtsp'
pkill -f 'liveview[.]py --live-dir'   # (on macOS the process is /.../Python, not python3)
sleep 2
echo "recorders left: $(pgrep -f '^\./src/rtspcam rtsp' | wc -l | tr -d ' '), live view left: $(pgrep -f 'liveview[.]py --live-dir' | wc -l | tr -d ' ')"
