#!/bin/sh
# Deploy the EBOOT and its fixture to the emulator's memory stick and launch it.
#
#   scripts/run-emu.sh          build, deploy, run
#   scripts/run-emu.sh --no-build
#
# Why a private copy of PPSSPP rather than the installed one:
#
# The installed PPSSPP (C:\Program Files\PPSSPP) ships an installed.txt, which
# puts its memory stick under the user's Documents folder. Windows Defender's
# Controlled Folder Access owns that folder on this machine, so nothing but an
# allow-listed process can write there -- a mkdir reports success and creates
# nothing, and a copy fails with a "file not found" that names a path which
# plainly exists. Without installed.txt beside it, PPSSPP runs portable and
# keeps its memory stick in <exedir>/memstick, which is ours to write.
#
# So this script copies the two files PPSSPP actually needs (the executable and
# assets/) into ppsspp-portable/ once, and from then on the memory stick is a
# directory in this project. Nothing is installed, and nothing outside the
# project is touched.
#
# The emulator is NOT authoritative about anything the Media Engine does --
# PROMPT.md Rule 4. It does not implement sceMpegGetAvcNalAu at all. What a run
# here does prove: the module loads, the memory readings are real, the fMP4
# parser walks a real file, and teardown returns to the menu rather than
# hanging.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PPSSPP_INSTALL=${PPSSPP_INSTALL:-/c/Program Files/PPSSPP}
PORTABLE="$ROOT/ppsspp-portable"
GAMEDIR="$PORTABLE/memstick/PSP/GAME/Hexfin"

if [ "${1:-}" != "--no-build" ]; then
    "$ROOT/scripts/build.sh"
fi

if [ ! -f "$PORTABLE/PPSSPPWindows64.exe" ]; then
    if [ ! -f "$PPSSPP_INSTALL/PPSSPPWindows64.exe" ]; then
        echo "scripts/run-emu.sh: no PPSSPP at $PPSSPP_INSTALL -- set PPSSPP_INSTALL" >&2
        exit 1
    fi
    echo "setting up $PORTABLE (one time)"
    mkdir -p "$PORTABLE"
    cp "$PPSSPP_INSTALL/PPSSPPWindows64.exe" "$PORTABLE/"
    cp -r "$PPSSPP_INSTALL/assets" "$PORTABLE/"
    # installed.txt is deliberately NOT copied: its absence is what makes this
    # copy portable, and the memory stick local.
fi

# A still-running emulator holds EBOOT.PBP open and the copy below fails with
# "Device or resource busy", so the previous run has to go first. Matched on
# the executable's full path, and only ever the copy under this project: the
# operator's own PPSSPP may be running something else entirely, and killing
# somebody's emulator out from under them to save a rebuild is not ours to do.
PORTABLE_WIN=$(cygpath -w "$PORTABLE")
powershell -NoProfile -Command "Get-Process PPSSPPWindows64 -ErrorAction SilentlyContinue | Where-Object { \$_.Path -eq '$PORTABLE_WIN\\PPSSPPWindows64.exe' } | Stop-Process -Force" >/dev/null 2>&1 || true

mkdir -p "$GAMEDIR"
cp "$ROOT/build/EBOOT.PBP" "$GAMEDIR/EBOOT.PBP"

# The fixture is gitignored (it is 1.7 MB of encoded video); scripts/fixtures.sh
# regenerates it. A missing one is not fatal -- the probe reports the failed
# open itself, which is a legitimate thing to want to see.
if [ -f "$ROOT/fixtures/probe360.mp4" ]; then
    cp "$ROOT/fixtures/probe360.mp4" "$GAMEDIR/probe360.mp4"
else
    echo "warning: fixtures/probe360.mp4 is missing -- the probe will report an open failure" >&2
fi

# MSYS_NO_PATHCONV: the argument is a Windows path for a Windows binary and
# must reach it verbatim.
GAMEDIR_WIN=$(cygpath -w "$GAMEDIR")
MSYS_NO_PATHCONV=1 exec "$PORTABLE/PPSSPPWindows64.exe" --windowed "$GAMEDIR_WIN\\EBOOT.PBP"
