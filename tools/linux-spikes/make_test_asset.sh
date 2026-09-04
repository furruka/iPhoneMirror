#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
#
# Test asset for the S3/S4 probe: a 1170x2532@60 vertical "phone screen" with
# large frame numbers and a timestamp (letterbox/aspect/decoding checks) and a
# moving cyan box (tearing checks); the testsrc2 inset covers colour checks.
# drawbox supports t (timestamp); gt(t,N) gives the wrap so the box sweeps a
# 220 px window repeatedly instead of running off screen.
#
# Usage: make_test_asset.sh [output-dir]   (default /tmp/ipm_s4)
set -e
OUT_DIR=${1:-/tmp/ipm_s4}
mkdir -p "$OUT_DIR"
FONT=${IPM_TEST_FONT:-/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc}
ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i "testsrc2=size=990x2145:rate=60" \
  -f lavfi -i "color=c=0x102030:s=1170x2532:r=60" \
  -filter_complex "\
[1:v][0:v]overlay=(W-w)/2:(H-h)/2[v0];\
[v0]drawbox=x='w/2-170+t*2.4*60-gt(t\,3.66)*2640-gt(t\,7.32)*2640-gt(t\,10.98)*2640-gt(t\,14.64)*2640-gt(t\,18.30)*2640-gt(t\,21.96)*2640-gt(t\,25.62)*2640':y=h*0.70:w=120:h=120:color=0x00E5FF@0.9:t=fill,\
drawtext=fontfile=${FONT}:text='%{n}':fontsize=220:fontcolor=white:borderw=8:bordercolor=black:x=(w-tw)/2:y=h*0.30,\
drawtext=fontfile=${FONT}:text='%{pts\:hms}':fontsize=90:fontcolor=0xFFD880:borderw=5:bordercolor=black:x=(w-tw)/2:y=h*0.52[vout]" \
  -map "[vout]" -t 30 -c:v libx264 -preset fast -crf 20 -pix_fmt yuv420p \
  "$OUT_DIR/phone_screen.mp4"
