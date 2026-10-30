#!/usr/bin/env bash
# Draws the title's home-screen icon (original art: a sun setting on a road
# under an open sky) into ps5/title/sce_sys/icon0.png. Needs ImageMagick 7 and the Open Sans font (Fedora: open-sans-fonts).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="$root/ps5/title/sce_sys/icon0.png"
magick -size 512x320 gradient:'#0b1d3a-#f08a3c' \
  \( -size 512x192 gradient:'#2a2340-#0d0d14' \) -append \
  -fill '#ffd27a' -draw 'circle 256,320 256,236' \
  -fill '#0d0d14' -draw 'rectangle 0,320 512,512' \
  -fill '#1b1b26' -draw 'polygon 236,320 276,320 420,512 92,512' \
  -fill '#ffd27a' -draw 'polygon 254,330 258,330 262,370 250,370' \
  -draw 'polygon 249,390 263,390 270,450 242,450' \
  -fill white -font /usr/share/fonts/open-sans/OpenSans-ExtraBold.ttf -pointsize 46 -gravity north -annotate +0+44 'OPEN HORIZON' \
  -fill '#ffd27a' -pointsize 96 -annotate +0+104 '2' \
  -depth 8 -strip "$out"
echo "$out"
