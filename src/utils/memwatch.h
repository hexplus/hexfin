/* The worst moment of a run, not the current one.
 *
 * A reading taken now is the one number a leak never shows up in: a film that
 * peaks once at thirty minutes is back to normal by the time anybody looks.
 * This keeps the minima instead.
 *
 * Free and largest-run are tracked APART, because they fail apart. The
 * decoder's working block wants 4 MB aligned to 4 MB, so a partition with 6 MB
 * free spread across a dozen holes cannot start a film while one with 5 MB in a
 * single run can. See docs/RESEARCH.md section 6.
 *
 * No platform headers on purpose: the console supplies the readings, and the
 * host can then test the arithmetic without one. */
#ifndef UTILS_MEMWATCH_H
#define UTILS_MEMWATCH_H

void memwatch_reset(void);
void memwatch_sample(unsigned free_kb, unsigned largest_kb);

unsigned memwatch_min_free_kb(void);
unsigned memwatch_min_largest_kb(void);
unsigned memwatch_samples(void);

#endif
