#!/usr/bin/env bash
# Run on the laptop: copy the project to the Pi and build there
set -euo pipefail
PI="${PI:-mililivis@10.42.0.221}"

rsync -az --delete --exclude build/ --exclude .git/ ~/AM_RADIO_QT "$PI":~/
ssh "$PI" 'cd ~/AM_RADIO_QT && cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build -j4'
