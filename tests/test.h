/* What a check file needs. See tests/test_main.c for why a skip is not a
 * pass. */
#ifndef TESTS_TEST_H
#define TESTS_TEST_H

/* 0 passes, above 0 fails, BELOW 0 skipped -- the check could not run at all.
 * Say why in `note`; a passing check may leave a measurement there too. */
typedef int (*test_fn)(char *note, unsigned note_len);

void test_add(const char *group, const char *name, test_fn fn);

#endif
