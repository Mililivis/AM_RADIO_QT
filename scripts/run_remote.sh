#!/usr/bin/env bash
# Run on the laptop: start AM radio on the Pi, window via X11 forwarding, sound via the SSH pipe
#   -Y  trusted X11 forwarding (Qt needs it)
#   -C  compression (spectrum repaints are much smaller)
# Audio: am_radio writes raw PCM s16le mono 48 kHz to stdout, pw-cat plays it on the laptop
set -euo pipefail
PI="${PI:-mililivis@10.42.0.221}"

ssh -Y -C -o ServerAliveInterval=10 "$PI" \
    'cd ~/AM_RADIO_QT && QT_QPA_PLATFORM=xcb QT_X11_NO_MITSHM=1 exec ./build/am_radio --audio-stdout' \
  | pw-cat --playback --format s16 --rate 48000 --channels 1 --latency 100ms -
