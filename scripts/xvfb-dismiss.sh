#!/usr/bin/env bash
set -euo pipefail

DISPLAY_NUM=$1; shift
WIN_NAME=$1; shift
KEY=$1; shift

xvfb-run -n "$DISPLAY_NUM" "$@" &
CMD_PID=$!

sleep 1
while kill -0 "$CMD_PID" 2>/dev/null; do
  WID=$(DISPLAY=":$DISPLAY_NUM" xdotool search --name "$WIN_NAME" 2>/dev/null | head -1)
  if [ -n "$WID" ]; then
    DISPLAY=":$DISPLAY_NUM" xdotool key --window "$WID" "$KEY"
  fi
  sleep 0.5
done

wait "$CMD_PID"
