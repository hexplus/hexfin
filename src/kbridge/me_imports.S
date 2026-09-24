/* The kernel import me_boot.c needs. The NID is not derived from a name the
 * way user-library NIDs are: 6.60+ kernel libraries use randomised NIDs, and
 * 0x5DFF5C50 is the one the 6.61 players that boot the ME use for this entry.
 * IMPORT_*, not STUB_*: see src/platform/psp_mpeg_import.S for why. */
	.set noreorder

#include "pspimport.s"

	IMPORT_START "sceMeCore_driver", 0x00010000
	IMPORT_FUNC  "sceMeCore_driver", 0x5DFF5C50, sceMeBootStart660
