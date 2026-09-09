#!/bin/bash
# Build build/config/SND0.AT3, the XMB hover sound, from any audio file.
#
#   bash build/make_sound.sh "path/to/nintendo-game-boy-startup.wav"
#
# The PSP XMB plays plain ATRAC3 (WAVE tag 0x0270) at 44.1kHz / 66 kbps. That is
# what MasterBoy's own shipping EBOOT contains, and its jingle does play. ATRAC3plus
# files decode fine in players but leave the XMB silent - build/check_at3.py
# compares against those numbers and says so.
#
# ffmpeg cannot encode ATRAC3 at all, and Sony's encoder is not distributed on its
# own. ATRACTool-Reloaded (MIT, by XyLe-GBP) ships both Sony codec tools as plain
# files under res/, so this script fetches it and uses res/psp_at3tool.exe. The PSP
# tool is the one that matters: the PS3 build sitting next to it refuses 44.1kHz
# input and can only emit ATRAC3plus.
#
# SND0.AT3 must stay under about 500KB, and under 500KB *combined* with ICON1.PMF if
# the EBOOT has one (ours does not).
set -e

SRC="${1:?usage: make_sound.sh <audio file>}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${TEMP:-/tmp}/psp_at3"
BR="${BR:-66}"          # the rate PSP EBOOTs use
TAIL="${TAIL:-1.2}"     # silence appended so the XMB loop does not clip

REL="$WORK/atrel/release-portable/res/psp_at3tool.exe"
URL="https://github.com/XyLe-GBP/ATRACTool-Reloaded/releases/download/v1.52.2620.901/ATRACTool-Rel-Portable.zip"

mkdir -p "$WORK" "$ROOT/build/audio" "$ROOT/build/config"

# A user-supplied encoder wins, so a copy of at3tool can just be dropped in.
TOOL="$ROOT/build/tools/at3tool.exe"
if [ ! -f "$TOOL" ]; then
    if [ ! -f "$REL" ]; then
        echo "--- fetching ATRACTool-Reloaded for its Sony encoders ---"
        mkdir -p "$WORK/atrel"
        curl -sL -o "$WORK/atrel/rel.zip" "$URL"
        (cd "$WORK/atrel" && unzip -oq rel.zip)
    fi
    TOOL="$REL"
fi
[ -f "$TOOL" ] || { echo "ERROR: no at3tool available" >&2; exit 1; }

echo "--- preparing PCM ---"
# Trim leading silence, normalise, append a tail so the loop does not clip. Kept at
# 44.1kHz: the PSP encoder wants it and the XMB expects it.
ffmpeg -hide_banner -loglevel error -y -i "$SRC" \
    -af "silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.02,loudnorm=I=-16:TP=-1.5:LRA=11,apad=pad_dur=$TAIL" \
    -ar 44100 -ac 2 -c:a pcm_s16le "$ROOT/build/audio/SND0_source.wav"

echo "--- encoding ATRAC3 @ ${BR}kbps ---"
"$TOOL" -e -br "$BR" -wholeloop "$ROOT/build/audio/SND0_source.wav" "$WORK/SND0.AT3"
[ -s "$WORK/SND0.AT3" ] || { echo "ERROR: encoder produced nothing" >&2; exit 1; }

cp "$WORK/SND0.AT3" "$ROOT/build/config/SND0.AT3"
echo
python "$ROOT/build/check_at3.py" "$ROOT/build/config/SND0.AT3"
echo
echo "Then run build/package.py to embed it."
