/* The check runner.
 *
 * A skip is NOT a pass: a suite that silently skipped the checks that matter
 * would report green while proving nothing, so the process exits non-zero when
 * anything was skipped as well as when anything failed. */

#include <stdio.h>
#include <string.h>

#define TEST_MAX      256
#define TEST_NOTE_MAX 128

typedef int (*test_fn)(char *note, unsigned note_len);

static struct {
    const char *group;
    const char *name;
    test_fn     fn;
} g_tests[TEST_MAX];

static unsigned g_count;
static unsigned g_turned_away;

void test_add(const char *group, const char *name, test_fn fn) {
    if (g_count >= TEST_MAX) {
        g_turned_away++;
        return;
    }
    g_tests[g_count].group = group;
    g_tests[g_count].name  = name;
    g_tests[g_count].fn    = fn;
    g_count++;
}

/* Every registration function, declared here because a registry that filled
 * itself through a constructor would drop whole files depending on link
 * order. */
void test_memwatch_register(void);
void test_fmp4_register(void);
void test_h264_au_register(void);
void test_h264_mmco_register(void);
void test_render_register(void);
void test_aac_config_register(void);
void test_av_sync_register(void);
void test_stats_register(void);
void test_http_parse_register(void);
void test_jellyfin_register(void);
void test_ui_text_register(void);


static void register_all(void) {
    test_memwatch_register();
    test_fmp4_register();
    test_h264_au_register();
    test_h264_mmco_register();
    test_render_register();
    test_aac_config_register();
    test_av_sync_register();
    test_stats_register();
    test_http_parse_register();
    test_jellyfin_register();
    test_ui_text_register();
}

int main(int argc, char **argv) {
    const char *filter = (argc > 1) ? argv[1] : 0;
    unsigned    i, passed = 0, failed = 0, skipped = 0, ran = 0;
    char        note[TEST_NOTE_MAX];
    int         rc;

    register_all();

    for (i = 0; i < g_count; i++) {
        if (filter && strncmp(g_tests[i].group, filter, strlen(filter)) != 0) continue;

        ran++;
        note[0] = 0;
        rc      = g_tests[i].fn(note, sizeof(note));

        if (rc == 0) {
            passed++;
            printf("  ok   %s: %s%s%s\n", g_tests[i].group, g_tests[i].name, note[0] ? " -- " : "", note);
        } else if (rc < 0) {
            skipped++;
            printf("  SKIP %s: %s%s%s\n", g_tests[i].group, g_tests[i].name, note[0] ? " -- " : "", note);
        } else {
            failed++;
            printf("  FAIL %s: %s%s%s\n", g_tests[i].group, g_tests[i].name, note[0] ? " -- " : "", note);
        }
    }

    /* A filter that matched nothing is a typo, not an empty suite -- an
     * unfiltered registry with zero checks is a different fault, already
     * caught by g_turned_away/g_count elsewhere. Reporting success here would
     * repeat exactly the mistake a skip already guards against: green while
     * proving nothing. */
    if (filter && ran == 0) {
        printf("\nno check group matches \"%s\"\n", filter);
        return 1;
    }

    printf("\n%u passed, %u failed, %u skipped", passed, failed, skipped);
    if (g_turned_away) printf(", %u TURNED AWAY (raise TEST_MAX)", g_turned_away);
    printf("\n");

    return (failed || skipped || g_turned_away) ? 1 : 0;
}
