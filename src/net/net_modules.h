/* One owner for the firmware's network modules.
 *
 * The same shape as platform/av_modules.h, and for the same reason it was
 * written: sceUtilityLoadNetModule refuses a module that is already loaded
 * rather than quietly succeeding, and docs/RESEARCH.md section 8 records what
 * that cost the AV path when two subsystems each kept a flag of their own --
 * the second caller got a driver error for a module that was loaded and
 * working, and the error named the wrong subsystem entirely.
 *
 * The net modules have exactly that shape. PSP_NET_MODULE_COMMON is wanted by
 * anything that touches the radio; INET by sockets and by the resolver;
 * PARSEURI and PARSEHTTP would be wanted by sceHttp and by nothing else. A
 * flag per caller here would reproduce a bug that is already documented, so
 * the flag belongs to the module.
 *
 * There is no release, for the reason platform/av_modules.h gives: unloading
 * is a step that can itself fail in the middle of a teardown that runs on the
 * error path, and design section 3.4 wants teardown to be the one thing that
 * cannot break. net/wifi.c does take the radio down -- that is a different
 * thing from unloading the PRX that drives it. */
#ifndef NET_NET_MODULES_H
#define NET_NET_MODULES_H

/* Returns 0 if the module is loaded -- whether this call loaded it or an
 * earlier one did. On failure returns the firmware's own negative result. A
 * module id outside the firmware's range returns -1, which is not a firmware
 * code and is not meant to look like one. */
int net_module_acquire(int module);

#endif /* NET_NET_MODULES_H */
