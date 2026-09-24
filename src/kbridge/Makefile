# hexfin_me.prx -- see me_boot.c. Built on its own:
#
#   scripts/build.sh -C src/kbridge
#
# and copied next to EBOOT.PBP. Kernel mode, so no C runtime (-nostartfiles)
# and nothing but the kernel library.
TARGET     = hexfin_me
OBJS       = me_boot.o me_imports.o
BUILD_PRX  = 1
PRX_EXPORTS = exports.exp
PSP_FW_VERSION = 661

CFLAGS  = -O2 -G0 -Wall -Wextra
LDFLAGS = -nostartfiles
LIBS    = -lpspkernel

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
