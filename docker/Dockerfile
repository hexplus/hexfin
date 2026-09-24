# The console toolchain, pinned so a build is reproducible rather than
# whatever the machine happened to have. pspdev/pspdev is Alpine-based and
# carries the cross compiler, psp-config and build.mak.
FROM pspdev/pspdev:latest

# make is not in the base image, and python3 is here for the generators this
# project will grow; adding both now costs nothing and avoids a confusing
# failure later.
RUN apk add --no-cache make python3 bash

WORKDIR /src
