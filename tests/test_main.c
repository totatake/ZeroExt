#include "test_common.h"

#if defined(_MSC_VER) && defined(_DEBUG) && defined(ZEROEXT_ENABLE_LEAK_CHECK)
#include <crtdbg.h>
#endif

int g_test_failures = 0;
int g_test_assertions = 0;

int main(int argc, char **argv) {
    const char *suite = NULL;
    int failures = 0;

#if defined(_MSC_VER) && defined(_DEBUG) && defined(ZEROEXT_ENABLE_LEAK_CHECK)
    _CrtMemState mem_start;
    _CrtMemState mem_end;
    _CrtMemState mem_diff;
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtMemCheckpoint(&mem_start);
#endif

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
    if (!suite || strcmp(suite, "perfetto_trace") == 0) {
        failures += run_suite_perfetto_trace();
    }

#if defined(_MSC_VER) && defined(_DEBUG) && defined(ZEROEXT_ENABLE_LEAK_CHECK)
    _CrtMemCheckpoint(&mem_end);
    if (_CrtMemDifference(&mem_diff, &mem_start, &mem_end)) {
        fprintf(stderr, "\n[LEAK CHECK FAILED] Memory leak detected in suite '%s':\n",
                suite ? suite : "all");
        _CrtMemDumpStatistics(&mem_diff);
        _CrtMemDumpAllObjectsSince(&mem_start);
        failures++;
    }
#endif

    printf("\nSummary: failures=%d, assertions=%d\n", failures, g_test_assertions);
    return failures == 0 ? 0 : 1;
}
