#!/bin/sh
# The session tests a release build gets on each test machine, through the
# scripted clients in ../crustdesk-macos-classic/host: by ID through an ID
# server and its relay, typing into Notepad (Legacy characters, Map-mode
# scancodes with Shift, accents by Alt+0nnn), the clipboard both ways in
# Unicode, the file manager (a byte-exact round trip, a Unicode folder name),
# Ctrl+Alt+Del and a screenshot.
#
#   session-test.sh NTXPORT DIRECTPORT ID PASSWORD OUTDIR [rdvpoke.py options]
#
# NTXPORT: where the machine's ntexec is forwarded; DIRECTPORT: the agent's
# direct port, forwarded. CLIPTOOL: tools/nt/cliptool.c's program there
# (default C:\NTEXEC\CLIPTOOL.EXE). OUTDIR gets the VP8 streams (decode them
# with host/build/vp8dump), the clipboard log and the screenshot.
NTXP=$1 DP=$2 ID=$3 PW=$4 O=$5
shift 5
HERE=$(cd "$(dirname "$0")" && pwd)
H=$HERE/../../crustdesk-macos-classic/host
X="python3 $HERE/ntx.py"
CT=${CLIPTOOL:-'C:\NTEXEC\CLIPTOOL.EXE'}
export NTX=127.0.0.1:$NTXP
mkdir -p "$O"
O=$(cd "$O" && pwd)
cd "$H" || exit 1
$X spawn notepad.exe >/dev/null
sleep 6
echo "== by ID, through the relay: typing"
timeout 150 python3 rdvpoke.py "$@" "$ID" "$PW" save "$O/type.bin" sleep 3 \
    type 'Release test: RustDesk! (Legacy) ' keydown 42 key 49 key 20 keyup 42 key 5 \
    type ' accents: ü é ñ' key 28 frames 6 2>&1 | grep -E 'relay|logged in|total|failure'
./build/vp8dump "$O/type.bin" "$O/type.ppm" >/dev/null
echo "== clipboard"
(timeout 40 python3 cdvpoke.py 127.0.0.1:$DP "$PW" sleep 2 clip 'from the peer: héllo ☺' \
    frames 14 > "$O/clip.log" 2>&1 &)
sleep 7
echo "on the machine: $($X run "$CT get" 2>/dev/null | head -1 | xxd -r -p)"
$X run "$CT set $(printf 'copied there: Grüße' | xxd -p | tr -d '\n')" >/dev/null 2>&1
sleep 12
grep -a 'clipboard from' "$O/clip.log"
echo "== files"
head -c 150000 /dev/urandom > "$O/up.bin"
timeout 120 python3 cdvpoke.py --files 127.0.0.1:$DP "$PW" put "$O/up.bin" 'C:\cdvrel\up.bin' \
    get 'C:\cdvrel\up.bin' "$O/down.bin" mkdir 'C:\cdvrel\Grüße' ls 'C:\cdvrel' \
    rm 'C:\cdvrel\up.bin' rmdir 'C:\cdvrel' 2>&1 | grep -E 'Grüße|error' | head
cmp "$O/up.bin" "$O/down.bin" && echo "round trip identical"
echo "== Ctrl+Alt+Del, screenshot"
timeout 60 python3 cdvpoke.py 127.0.0.1:$DP "$PW" save "$O/sas.bin" sleep 3 ctrl 100 frames 5 \
    shot "$O/shot.png" key 1 frames 3 2>&1 | grep -E 'screenshot'
./build/vp8dump "$O/sas.bin" "$O/sas.ppm" >/dev/null
