#ifndef LIBEXT2FS_WIN_TEST_COMMON_H
#define LIBEXT2FS_WIN_TEST_COMMON_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern int g_test_failures;
extern int g_test_assertions;

#define TEST_ASSERT(cond) do { \
    g_test_assertions++; \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond); \
        g_test_failures++; \
        return 1; \
    } \
} while (0)

#define TEST_ASSERT_EQ_INT(expected, actual) do { \
    long long _e = (long long)(expected); \
    long long _a = (long long)(actual); \
    g_test_assertions++; \
    if (_e != _a) { \
        fprintf(stderr, "  FAIL [%s:%d]: expected %lld, got %lld (%s vs %s)\n", \
                __FILE__, __LINE__, _e, _a, #expected, #actual); \
        g_test_failures++; \
        return 1; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    printf("[RUN ] %s\n", #fn); \
    if (fn() == 0) { \
        printf("[PASS] %s\n", #fn); \
    } else { \
        printf("[FAIL] %s\n", #fn); \
        suite_failures++; \
    } \
} while (0)

static __inline void test_make_temp_path(char *out, size_t out_sz, const char *filename) {
    char tmp_dir[MAX_PATH];
    DWORD n = GetTempPathA((DWORD)sizeof(tmp_dir), tmp_dir);
    if (n == 0 || n >= sizeof(tmp_dir)) {
        snprintf(out, out_sz, "%s", filename);
    } else {
        snprintf(out, out_sz, "%slibext2fs_win_%u_%s", tmp_dir, (unsigned)GetCurrentProcessId(), filename);
    }
}

int run_suite_win32_io(void);
int run_suite_partition_and_probe(void);
int run_suite_vfs_operations(void);
int run_suite_winfsp_callbacks(void);
int run_suite_extent_and_overlay(void);

#endif /* LIBEXT2FS_WIN_TEST_COMMON_H */
