# The console build. Everything written goes under build/.
OUT = build
$(shell mkdir -p $(OUT))

TARGET        = $(OUT)/hexfin
PSP_EBOOT     = $(OUT)/EBOOT.PBP
PSP_EBOOT_SFO = $(OUT)/PARAM.SFO

OBJS = src/main.o src/platform/psp_platform.o src/utils/memwatch.o \
       src/platform/av_modules.o src/platform/psp_mpeg_import.o \
       src/media/h264_au.o src/media/h264_mmco.o src/media/video_psp.o \
       src/media/aac_config.o src/media/audio_psp.o src/media/av_sync.o \
       src/media/fmp4.o src/media/source_file.o \
       src/net/net_modules.o src/net/wifi.o src/platform/trace.o src/platform/me_runtime.o src/net/http_parse.o src/net/http.o \
       src/jellyfin/json.o src/jellyfin/requests.o src/jellyfin/items.o src/jellyfin/jellyfin.o \
       src/platform/input.o src/player/player.o src/app/app.o src/ui/text.o src/ui/menu.o \
       src/ui/letterbox.o src/ui/render.o src/ui/stats.o

# -G0 because the small-data area overflows on a program of any size, and the
# failure it produces names nothing useful.
CFLAGS  = -O2 -G0 -Wall -Wextra -Wno-unused-parameter -std=c11 -Isrc $(PROBE_DEFS)

# Which probe a build is, from the command line without editing main.c:
#   scripts/build.sh 'PROBE_DEFS=-DPROBE_JELLYFIN=1 -DPROBE_BUILD_NAME=\"J1\"'
# Not CFLAGS itself: overriding that on the command line also drops the SDK
# include paths build.mak adds to it.
PROBE_DEFS ?=

# Deliberately NOT -lpspmpeg. src/platform/psp_mpeg_import.S declares every
# sceMpeg function this project calls, including sceMpegGetAvcNalAu, which the
# SDK's library does not stub at all. Linking both would put two import tables
# for one library into the module -- duplicate stubs, and an ordering the
# loader was never meant to resolve. One source of truth.
#
# The extra libraries are appended AFTER the build.mak include instead, for the
# reason recorded there.
LIBS =

EXTRA_TARGETS   = $(PSP_EBOOT)
PSP_EBOOT_TITLE = Hexfin

# PSPLINK loads relocatable modules only and refuses a static ELF with
# 0x80020148, so this must not silently default to a plain make.
BUILD_PRX ?= 1

# A PSP-1000 has no extra RAM to get; pinning these keeps the measured numbers
# identical regardless of loader instead of letting build.mak's default
# firmware version decide MEMSIZE in PARAM.SFO.
PSP_FW_VERSION   = 660
PSP_LARGE_MEMORY = 0

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

# AFTER the include, and that is load-bearing, not tidiness.
#
# build.mak builds its link line as `$(OBJS) $(LIBS)`, and prepends the SDK's
# own libraries to whatever LIBS already held. Setting these two before the
# include therefore puts them BEFORE -lpspdebug, -lpspdisplay and the rest --
# and psp-fixup-imports then refuses the module with
#
#   Warning: could not fixup imports, stubs out of order.
#   Ensure the SDK libraries are linked in last to correct this.
#
# That warning is not cosmetic. Measured on this tree: with -lpsputility ahead
# of the SDK libraries, the import table it leaves behind has overlapping
# entries -- sceUtility claiming two functions where its stubs hold one, so its
# NID table runs into sceDisplay's. Appending here instead, the fixup is silent
# and every entry tiles exactly against the next. It reproduces with no
# hand-written imports present at all, so it is a property of the link order
# and nothing to do with psp_mpeg_import.S.
#
# A module whose import tables do not tile is the same hazard that file's
# comment describes at length: the emulator resolves calls by NID and would
# never notice, while a PSP-1000 binds against exactly these tables.
#
# -lpspaudiocodec and -lpspaudio join the list here for the same reason:
# neither is in build.mak's own default LIBS (unlike -lpspdebug and the
# rest), and unlike sceMpeg's NAL entry point, every sceAudiocodec/sceAudio
# call audio_psp.c makes IS stubbed by the SDK -- psp-nm on
# libpspaudiocodec.a shows sceAudiocodecCheckNeedMem/Init/Decode/GetEDRAM/
# ReleaseEDRAM all present, so there is no hand-written import table for
# audio the way psp_mpeg_import.S exists for video.
#
# Their position between build.mak's own libraries and -lpspmpegbase/
# -lpsputility is also load-bearing, not alphabetical: appended AFTER
# -lpspmpegbase -lpsputility instead, psp-fixup-imports printed the exact
# "stubs out of order" warning above again, for the same reason those two
# needed to move once already -- whatever tiling order the fixup tool wants
# across an entire module's import tables, it is sensitive to where each
# library's stubs land relative to every other one, not just relative to
# build.mak's own defaults. Measured on this tree: this order is silent,
# swapping the last two pairs is not.
#
# The network libraries are appended here too, and their POSITION -- ahead of
# the audio and mpeg pairs rather than after them -- is the order that makes
# psp-fixup-imports silent on this tree. Appended last instead, it warns.
#
# -lpspnet and -lpspnet_apctl are NOT repeated: build.mak's own defaults
# already carry them, and naming them again only moved the warning around.
# What did have to be solved separately is that newlib's socket glue
# references sceNetInetGetErrno from an archive scanned after this one, which
# dragged a twelfth sceNetInet stub in on a second pass and left it sitting a
# hundred bytes from its eleven siblings -- see net/http.c's
# err_add_socket_errno, which calls it here so the stub arrives with them.
# -lpsppower (scePowerSetClockFrequency, the 333 MHz experiment) is silent in
# any position but last; measured 2026-09-23, last warns "stubs out of order".
LIBS := $(LIBS) -lpspkubridge -lpspsystemctrl_user -lpsprtc -lpsppower -lpspnet_inet -lpspnet_resolver -lpspaudiocodec -lpspaudio -lpspmpegbase -lpsputility
