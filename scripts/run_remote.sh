#!/usr/bin/env bash
# Run on the laptop: start AM radio on the Pi, window via X11 forwarding, sound via the SSH pipe
#   -Y  trusted X11 forwarding (Qt needs it)
#   -C  compression (spectrum and waterfall repaints are much smaller)
#   -L  tunnel for the rtl_tcp IQ server: SDR++ / GQRX on the laptop connect to localhost:1234
# Audio: am_radio writes raw PCM s16le mono 48 kHz to stdout, pw-cat plays it on the laptop
set -euo pipefail
# Extra arguments are passed to am_radio, e.g.: run_remote.sh --panorama 2300-26100
PI="${PI:-mililivis@10.42.0.221}"
ARGS=""
[ $# -gt 0 ] && ARGS=$(printf ' %q' "$@")

ssh -Y -C -o ServerAliveInterval=10 -L 1234:localhost:1234 "$PI" \
    "cd ~/AM_RADIO_QT && QT_QPA_PLATFORM=xcb QT_X11_NO_MITSHM=1 exec ./build/am_radio --audio-stdout${ARGS}" \
  | pw-cat --playback --format s16 --rate 48000 --channels 1 --latency 100ms -
