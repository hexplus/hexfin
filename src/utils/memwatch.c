/* See utils/memwatch.h. */

#include "utils/memwatch.h"

static unsigned g_min_free;
static unsigned g_min_largest;
static unsigned g_samples;

void memwatch_reset(void) {
    g_min_free    = 0;
    g_min_largest = 0;
    g_samples     = 0;
}

void memwatch_sample(unsigned free_kb, unsigned largest_kb) {
    /* The first reading seeds both: a zero-initialised minimum would swallow
     * every later one. */
    if (g_samples == 0) {
        g_min_free    = free_kb;
        g_min_largest = largest_kb;
    } else {
        if (free_kb < g_min_free) g_min_free = free_kb;
        if (largest_kb < g_min_largest) g_min_largest = largest_kb;
    }
    g_samples++;
}

unsigned memwatch_min_free_kb(void) { return g_min_free; }
unsigned memwatch_min_largest_kb(void) { return g_min_largest; }
unsigned memwatch_samples(void) { return g_samples; }
