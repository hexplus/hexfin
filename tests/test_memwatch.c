/* See utils/memwatch.h. The case worth the file is the last one: free and
 * largest-run fail apart, and a tracker watching only the total would report a
 * healthy partition right up to the claim that cannot be placed. */

#include "utils/memwatch.h"

#include "test.h"

#include <stdio.h>

static int t_the_first_reading_is_the_minimum(char *note, unsigned n) {
    memwatch_reset();
    memwatch_sample(9000, 4500);

    if (memwatch_min_free_kb() != 9000 || memwatch_min_largest_kb() != 4500) {
        snprintf(note, n, "one reading gave free %u largest %u", memwatch_min_free_kb(), memwatch_min_largest_kb());
        return 1;
    }
    snprintf(note, n, "free 9000 kB, largest run 4500 kB, from one reading");
    return 0;
}

static int t_the_minimum_is_the_smallest_reading(char *note, unsigned n) {
    memwatch_reset();
    memwatch_sample(9000, 4500);
    memwatch_sample(6200, 3100);
    memwatch_sample(8100, 4000);

    if (memwatch_min_free_kb() != 6200) {
        snprintf(note, n, "three readings gave %u, not the smallest", memwatch_min_free_kb());
        return 1;
    }
    if (memwatch_samples() != 3) {
        snprintf(note, n, "counted %u readings, not 3", memwatch_samples());
        return 1;
    }
    snprintf(note, n, "6200 kB, the smallest of three");
    return 0;
}

static int t_a_reset_forgets_the_run(char *note, unsigned n) {
    memwatch_reset();
    memwatch_sample(1000, 500);
    memwatch_reset();

    if (memwatch_samples() != 0) {
        snprintf(note, n, "%u readings survived the reset", memwatch_samples());
        return 1;
    }
    memwatch_sample(9000, 4500);
    if (memwatch_min_free_kb() != 9000) {
        snprintf(note, n, "the run before the reset still shows: %u", memwatch_min_free_kb());
        return 1;
    }
    snprintf(note, n, "the run before the reset is gone");
    return 0;
}

/* The fault the module exists for: plenty free, no run big enough. */
static int t_free_and_largest_run_are_tracked_apart(char *note, unsigned n) {
    memwatch_reset();
    memwatch_sample(12000, 8000);
    memwatch_sample(11800, 1160); /* the shape of a fragmented partition */

    if (memwatch_min_free_kb() != 11800) {
        snprintf(note, n, "free reads %u, not 11800", memwatch_min_free_kb());
        return 1;
    }
    if (memwatch_min_largest_kb() != 1160) {
        snprintf(note, n, "largest run reads %u, not 1160", memwatch_min_largest_kb());
        return 1;
    }
    snprintf(note, n, "11800 kB free with a 1160 kB largest run -- no 4 MB claim fits");
    return 0;
}

void test_memwatch_register(void) {
    test_add("memwatch", "the first reading is the minimum", t_the_first_reading_is_the_minimum);
    test_add("memwatch", "the minimum is the smallest reading", t_the_minimum_is_the_smallest_reading);
    test_add("memwatch", "a reset forgets the run", t_a_reset_forgets_the_run);
    test_add("memwatch", "free and largest run are tracked apart", t_free_and_largest_run_are_tracked_apart);
}
