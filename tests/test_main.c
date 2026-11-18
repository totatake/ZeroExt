#include "test_common.h"

int g_test_failures = 0;
int g_test_assertions = 0;

int main(int argc, char **argv) {
    const char *suite = NULL;
    int failures = 0;

    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--suite") == 0) {
            suite = argv[i + 1];
        }
    }

    if (!suite || strcmp(suite, "win32_io") == 0) {
        failures += run_suite_win32_io();
    }
    if (!suite || strcmp(suite, "partition_and_probe") == 0) {
        failures += run_suite_partition_and_probe();
    }
    if (!suite || strcmp(suite, "vfs_operations") == 0) {
        failures += run_suite_vfs_operations();
    }
    if (!suite || strcmp(suite, "winfsp_callbacks") == 0) {
        failures += run_suite_winfsp_callbacks();
    }
    if (!suite || strcmp(suite, "extent_and_overlay") == 0) {
        failures += run_suite_extent_and_overlay();
    }

    printf("\nSummary: failures=%d, assertions=%d\n", failures, g_test_assertions);
    return failures == 0 ? 0 : 1;
}
