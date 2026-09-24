#!/bin/sh
# The console build, in a container, from any working directory.
#
#   scripts/build.sh            the EBOOT
#   scripts/build.sh clean
#
# BUILD_PRX=1 always: PSPLINK loads relocatable modules only and refuses a
# static ELF with 0x80020148, so a build that cannot be loaded over the cable
# is not worth making.
set -eu

IMAGE=hexfin-build
ROOT=$(cd "$(dirname "$0")/.." && pwd)

# Docker Desktop on Windows wants a drive-letter path; MSYS hands us /c/....
# `pwd -W` is the MSYS spelling and is absent elsewhere, hence the fallback.
HOSTROOT=$(cd "$ROOT" && { pwd -W 2>/dev/null || pwd; })

docker build -t "$IMAGE" "$ROOT/docker"

# MSYS_NO_PATHCONV on the run only: its /src arguments are CONTAINER paths and
# must survive verbatim, while docker build's argument above is a real host
# path that needs the conversion.
MSYS_NO_PATHCONV=1 exec docker run --rm -v "$HOSTROOT:/src" -w /src "$IMAGE" \
    make BUILD_PRX=1 "$@"
