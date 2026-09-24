/* Hand-written imports for the sceMpeg firmware library.
 *
 * This file is the reason the project is possible. PSPSDK's pspmpeg.h exposes
 * an API built entirely around Sony's PSMF container -- ringbuffers,
 * sceMpegRegistStream, sceMpegGetAvcAu -- and read at face value it says the
 * Media Engine cannot be fed a Jellyfin transcode. But the firmware also
 * exports sceMpegGetAvcNalAu, which takes a raw H.264 access unit plus the
 * parameter sets with NO container at all. PSPSDK does not declare it and
 * libpspmpeg does not stub it, so the only way to call it is to build the
 * import table here.
 *
 *
 * WHY EVERY sceMpeg FUNCTION IS LISTED HERE AND -lpspmpeg IS NOT LINKED
 *
 * Mixing this file with libpspmpeg's stubs would put two import tables for
 * the same library into one module: duplicate stubs, and an ordering the
 * loader was never meant to resolve. One source of truth. Everything this
 * project calls in sceMpeg is declared below, and the Makefile deliberately
 * omits -lpspmpeg.
 *
 *
 * THE TRAP -- READ THIS BEFORE CHANGING ANYTHING BELOW
 *
 * Two mistakes are possible here, and BOTH produce a module that loads and
 * runs correctly in the emulator and FAILS TO START on a real PSP:
 *
 *   1. Using STUB_START / STUB_FUNC (pspstub.s) instead of
 *      IMPORT_START / IMPORT_FUNC (pspimport.s).
 *   2. Getting the library version word wrong. For sceMpeg it is 0x00090000.
 *
 * The two macro families are not interchangeable. STUB_FUNC emits a real
 * `jr $ra / nop` body and STUB_START emits a finished stub header pointing at
 * its own id table -- a table the module treats as already correct.
 * IMPORT_FUNC emits a two-word placeholder (module pointer, NID) with the
 * header's id-table and text pointers left as __executable_start, which
 * psp-fixup-imports then rewrites at link time into the real table the
 * firmware loader binds against. A STUB_-built table is simply never bound on
 * hardware; the emulator resolves calls by NID regardless and so cannot see
 * the difference.
 *
 * The consequence is the one to carry forward: DEVELOPMENT HERE IS ON AN
 * EMULATOR, AND AN EMULATOR PASS PROVES THE CODE RUNS, NOT THAT THE STUB IS
 * RIGHT.
 * There is no emulator test, of any thoroughness, that can catch this class
 * of error. Only a PSP-1000 can. See docs/RESEARCH.md section 1.
 *
 * psp-fixup-imports may warn "stubs out of order" once hand-written imports
 * are present. That warning is expected and is not itself a failure.
 *
 *
 * THE NIDs ARE DERIVED, NOT COPIED
 *
 * A PSP NID is the first four bytes of SHA-1(function_name), read
 * little-endian. Every value below was computed from the name beside it, and
 * any of them can be reproduced with:
 *
 *   python3 -c "import hashlib;print(hex(int.from_bytes(hashlib.sha1(b'sceMpegGetAvcNalAu').digest()[:4],'little')))"
 *
 * So nothing here is a magic number taken on trust. They are a property of
 * the function names Sony chose.
 */

#include "pspimport.s"

    IMPORT_START "sceMpeg", 0x00090000

    /* Library lifetime. */
    IMPORT_FUNC  "sceMpeg", 0x682A619B, sceMpegInit
    IMPORT_FUNC  "sceMpeg", 0x874624D6, sceMpegFinish

    /* Instance lifetime. sceMpegQueryMemSize says how large the work buffer
     * must be; we refuse a figure above a compiled-in ceiling rather than
     * allocating whatever the firmware asks for. */
    IMPORT_FUNC  "sceMpeg", 0xC132E22F, sceMpegQueryMemSize
    IMPORT_FUNC  "sceMpeg", 0xD8C5F121, sceMpegCreate
    IMPORT_FUNC  "sceMpeg", 0x606A4649, sceMpegDelete

    /* The access unit descriptor the decode call reads from. */
    IMPORT_FUNC  "sceMpeg", 0x167AFD9E, sceMpegInitAu

    /* Decode. sceMpegAvcDecodeMode is what selects RGB565 output, which is
     * the single highest-value thing this phase measures (design 3.2):
     * framebuffer-format output means no colour-space stage at all. */
    IMPORT_FUNC  "sceMpeg", 0xA11C7026, sceMpegAvcDecodeMode
    IMPORT_FUNC  "sceMpeg", 0x0E3C2E9D, sceMpegAvcDecode
    IMPORT_FUNC  "sceMpeg", 0x740FCCD1, sceMpegAvcDecodeStop

    /* The undocumented one. Raw access unit in, no PSMF anywhere. PSPSDK
     * neither declares nor stubs this; its C declaration is in
     * src/platform/psp_mpeg_nal.h, with how its signature was settled after
     * a guessed one switched the console off. This import entry was right
     * all along: the same NID and version word are what working hardware
     * players use. */
    IMPORT_FUNC  "sceMpeg", 0x11F95CF1, sceMpegGetAvcNalAu

    /* Where the picture sceMpegAvcDecode produced actually is, when it was
     * told to write it nowhere: the YUV planes sceMpegBaseCscAvc converts.
     * Also undeclared by PSPSDK; see psp_mpeg_nal.h. */
    IMPORT_FUNC  "sceMpeg", 0xCF3547A2, sceMpegAvcDecodeDetail2
