#include "test_common.h"
#include "../src/trace/perfetto_trace.h"
#include "../src/vfs/ext4_vfs.h"
#include "../src/driver/winfsp_driver.h"

#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE     0x00000001
#endif
#ifndef FILE_NON_DIRECTORY_FILE
#define FILE_NON_DIRECTORY_FILE 0x00000040
#endif

static char *read_entire_file(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    long sz;
    char *buf;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    buf = (char *)malloc((size_t)sz + 1U);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (sz > 0) {
        size_t got = fread(buf, 1, (size_t)sz, fp);
        buf[got] = '\0';
        if (out_len) *out_len = got;
    } else {
        buf[0] = '\0';
        if (out_len) *out_len = 0;
    }
    fclose(fp);
    return buf;
}

static void delete_dir_recursive_a(const char *dir) {
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            char child[MAX_PATH];
            snprintf(child, sizeof(child), "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                delete_dir_recursive_a(child);
            } else {
                SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(child);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

static int test_trace_disabled_noop(void) {
    ztrace_summary_t sum;
    TEST_ASSERT(!ztrace_is_enabled());
    ztrace_set_thread_name("IgnoredThread");
    ztrace_record_complete("test", "span", 100, 50, 1, 2, 0, ZTRACE_ARGS_IO);
    ztrace_record_counter("test", "counter", 1, 2, 3, ZTRACE_ARGS_COUNTER);
    ztrace_record_instant("test", "instant", 0, 0, 0, ZTRACE_ARGS_NONE);
    TEST_ASSERT_EQ_INT(0, ztrace_shutdown(&sum));
    TEST_ASSERT_EQ_INT(0, (long long)sum.events_written);
    return 0;
}

typedef struct trace_reader_arg {
    ext4_vfs_ctx_t *ctx;
    ext2_ino_t      ino;
    int             err;
} trace_reader_arg_t;

static DWORD WINAPI trace_reader_worker(LPVOID param) {
    trace_reader_arg_t *a = (trace_reader_arg_t *)param;
    uint8_t buf[8192];
    ztrace_set_thread_name("VFS-Reader");
    for (uint64_t i = 0; i < 8; i++) {
        uint32_t got = 0;
        int rc = ext4_vfs_read(a->ctx, a->ino, buf, i * 65536ULL, sizeof(buf), &got);
        if (rc != 0 || got != sizeof(buf)) {
            a->err = rc ? rc : -5;
            return 1;
        }
    }
    return 0;
}

static int test_trace_vfs_direct_standard_and_overlay(void) {
    char img_path[MAX_PATH], trace_path[MAX_PATH], upper_dir[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    ext2_ino_t big_ino = 0, small_ino = 0;
    uint32_t w = 0, got = 0;
    uint8_t *big_buf;
    char small_data[] = "Overlay Base Payload";
    char read_buf[64];
    ztrace_summary_t sum;
    char *json = NULL;
    size_t json_len = 0;

    test_make_temp_path(img_path, sizeof(img_path), "trace_vfs.img");
    test_make_temp_path(trace_path, sizeof(trace_path), "trace_vfs.json");
    test_make_temp_path(upper_dir, sizeof(upper_dir), "trace_upper");
    DeleteFileA(img_path);
    DeleteFileA(trace_path);
    delete_dir_recursive_a(upper_dir);

    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 16ULL * 1024ULL * 1024ULL, "TRACEVOL"));

    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));

    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/big.bin", 0, 0644, 0, &big_ino));
    big_buf = (uint8_t *)malloc(1024U * 1024U);
    TEST_ASSERT(big_buf != NULL);
    memset(big_buf, 0xAB, 1024U * 1024U);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, big_ino, big_buf, 0, 1024U * 1024U, 0, &w, NULL));
    free(big_buf);

    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/small.txt", 0, 0644, 0, &small_ino));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, small_ino, small_data, 0, (uint32_t)strlen(small_data), 0, &w, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    /* Enable Perfetto trace */
    TEST_ASSERT_EQ_INT(0, ztrace_init(trace_path, 0));
    TEST_ASSERT(ztrace_is_enabled());
    ztrace_set_thread_name("Main-Tester");

    /* 1. Standard LRU cache read across 4 threads (disable_direct_io = 1) */
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 1;
    cfg.disable_direct_io = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    {
        trace_reader_arg_t args[4];
        HANDLE threads[4];
        for (int i = 0; i < 4; i++) {
            args[i].ctx = ctx;
            args[i].ino = big_ino;
            args[i].err = 0;
            threads[i] = CreateThread(NULL, 0, trace_reader_worker, &args[i], 0, NULL);
            TEST_ASSERT(threads[i] != NULL);
        }
        WaitForMultipleObjects(4, threads, TRUE, INFINITE);
        for (int i = 0; i < 4; i++) {
            CloseHandle(threads[i]);
            TEST_ASSERT_EQ_INT(0, args[i].err);
        }
    }
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    /* 2. Direct I/O read (cold extract + hot extent cache hit) */
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    for (int iter = 0; iter < 2; iter++) {
        TEST_ASSERT_EQ_INT(0, ext4_vfs_read(ctx, big_ino, read_buf, 0, sizeof(read_buf), &got));
        TEST_ASSERT_EQ_INT((long long)sizeof(read_buf), got);
    }
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    /* 3. Overlay copy-up + upper write/read + whiteout unlink */
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    cfg.overlay_upper_dir = upper_dir;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    {
        ext4_file_handle_t *h = NULL;
        TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/small.txt", 1, &h));
        TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, h->ino, "+UP", 0, 3, 1, &w, NULL));
        TEST_ASSERT_EQ_INT(0, ext4_vfs_read(ctx, h->ino, read_buf, 0, sizeof(read_buf), &got));
        TEST_ASSERT(got > 0);
        ext4_vfs_close_handle(h);
        TEST_ASSERT_EQ_INT(0, ext4_vfs_unlink(ctx, "/small.txt"));
    }
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    TEST_ASSERT_EQ_INT(0, ztrace_shutdown(&sum));
    TEST_ASSERT(!ztrace_is_enabled());
    TEST_ASSERT(sum.events_written > 20);
    TEST_ASSERT(sum.threads_seen >= 5);
    TEST_ASSERT_EQ_INT(0, (long long)sum.dropped_events);

    json = read_entire_file(trace_path, &json_len);
    TEST_ASSERT(json != NULL && json_len > 100);
    TEST_ASSERT(strstr(json, "{\"displayTimeUnit\":\"ns\",\"traceEvents\":[") == json);
    TEST_ASSERT(strstr(json, "\n]}\n") != NULL);
    TEST_ASSERT(strstr(json, "\"process_name\"") != NULL);
    TEST_ASSERT(strstr(json, "\"Main-Tester\"") != NULL);
    TEST_ASSERT(strstr(json, "\"VFS-Reader\"") != NULL);
    TEST_ASSERT(strstr(json, "\"ctx->rwlock wait (S)\"") != NULL);
    TEST_ASSERT(strstr(json, "\"ctx->rwlock hold (S)\"") != NULL);
    TEST_ASSERT(strstr(json, "\"win32_io.cache_lock wait (S)\"") != NULL);
    TEST_ASSERT(strstr(json, "\"win32_io.cache_lock wait (X)\"") != NULL);
    TEST_ASSERT(strstr(json, "\"extent_cache.lock wait (S)\"") != NULL);
    TEST_ASSERT(strstr(json, "\"extent_cache.lock wait (X)\"") != NULL);
    TEST_ASSERT(strstr(json, "\"ext2fs_file_read\"") != NULL);
    TEST_ASSERT(strstr(json, "\"ext4_extent_extract\"") != NULL);
    TEST_ASSERT(strstr(json, "\"ext4_extent_pread\"") != NULL);
    TEST_ASSERT(strstr(json, "\"win32_io_pread_direct\"") != NULL);
    TEST_ASSERT(strstr(json, "\"ReadFile\"") != NULL);
    TEST_ASSERT(strstr(json, "\"LRU Block Cache\"") != NULL);
    TEST_ASSERT(strstr(json, "\"Extent Cache\"") != NULL);
    TEST_ASSERT(strstr(json, "\"overlay_resolve\"") != NULL);
    TEST_ASSERT(strstr(json, "\"overlay_copy_up\"") != NULL);
    TEST_ASSERT(strstr(json, "\"overlay_write_upper\"") != NULL);
    TEST_ASSERT(strstr(json, "\"overlay_read_upper\"") != NULL);
    TEST_ASSERT(strstr(json, "\"overlay_whiteout\"") != NULL);
    TEST_ASSERT(strstr(json, "\"Overlay CopyUp Bytes\"") != NULL);

    free(json);
    DeleteFileA(trace_path);
    DeleteFileA(img_path);
    delete_dir_recursive_a(upper_dir);
    return 0;
}

static int test_trace_winfsp_callback_spans(void) {
    char img_path[MAX_PATH], trace_path[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    FSP_FILE_SYSTEM *fs = NULL;
    const FSP_FILE_SYSTEM_INTERFACE *iface = NULL;
    FSP_FSCTL_FILE_INFO fi;
    PVOID file_ctx = NULL, dir_ctx = NULL;
    ULONG transferred = 0;
    char payload[] = "Perfetto WinFsp Test";
    char rbuf[64];
    UINT8 dir_buf[1024];
    ztrace_summary_t sum;
    char *json = NULL;

    test_make_temp_path(img_path, sizeof(img_path), "trace_fsp.img");
    test_make_temp_path(trace_path, sizeof(trace_path), "trace_fsp.json");
    DeleteFileA(img_path);
    DeleteFileA(trace_path);

    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 8ULL * 1024ULL * 1024ULL, "FSPTRACE"));

    TEST_ASSERT_EQ_INT(0, ztrace_init(trace_path, 4096));
    ztrace_set_thread_name("WinFsp-Test");

    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.case_insensitive = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, winfsp_driver_create_test_fs(ctx, &fs));
    iface = winfsp_driver_get_interface();

    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Create(fs, L"\\hello.txt", FILE_NON_DIRECTORY_FILE,
                                     FILE_WRITE_DATA | FILE_READ_DATA, FILE_ATTRIBUTE_ARCHIVE,
                                     NULL, 0, &file_ctx, &fi));
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Write(fs, file_ctx, payload, 0, (ULONG)strlen(payload),
                                    FALSE, FALSE, &transferred, &fi));
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->GetFileInfo(fs, file_ctx, &fi));
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->Flush(fs, file_ctx, &fi));
    iface->Cleanup(fs, file_ctx, L"\\hello.txt", 0);
    iface->Close(fs, file_ctx);

    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Open(fs, L"\\hello.txt", FILE_NON_DIRECTORY_FILE,
                                   FILE_READ_DATA, &file_ctx, &fi));
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Read(fs, file_ctx, rbuf, 0, sizeof(rbuf), &transferred));
    iface->Close(fs, file_ctx);

    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Open(fs, L"\\", FILE_DIRECTORY_FILE, FILE_LIST_DIRECTORY, &dir_ctx, &fi));
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->ReadDirectory(fs, dir_ctx, NULL, NULL, dir_buf, sizeof(dir_buf), &transferred));
    iface->Close(fs, dir_ctx);

    winfsp_driver_destroy_test_fs(fs);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    TEST_ASSERT_EQ_INT(0, ztrace_shutdown(&sum));
    TEST_ASSERT(sum.events_written > 10);

    json = read_entire_file(trace_path, NULL);
    TEST_ASSERT(json != NULL);
    TEST_ASSERT(strstr(json, "\"FspCreate\"") != NULL);
    TEST_ASSERT(strstr(json, "\"FspWrite\"") != NULL);
    TEST_ASSERT(strstr(json, "\"FspGetFileInfo\"") != NULL);
    TEST_ASSERT(strstr(json, "\"FspFlush\"") != NULL);
    TEST_ASSERT(strstr(json, "\"FspCleanup\"") != NULL);
    TEST_ASSERT(strstr(json, "\"FspClose\"") != NULL);
    TEST_ASSERT(strstr(json, "\"FspOpen\"") != NULL);
    TEST_ASSERT(strstr(json, "\"FspRead\"") != NULL);
    TEST_ASSERT(strstr(json, "\"FspReadDirectory\"") != NULL);
    TEST_ASSERT(strstr(json, "\"ext2fs_file_write\"") != NULL);
    TEST_ASSERT(strstr(json, "\"WriteFile\"") != NULL);
    TEST_ASSERT(strstr(json, "\"ctx->rwlock wait (X)\"") != NULL);

    free(json);
    DeleteFileA(trace_path);
    DeleteFileA(img_path);
    return 0;
}

static int test_trace_ring_wrap_and_reinit(void) {
    char trace1[MAX_PATH], trace2[MAX_PATH];
    ztrace_summary_t sum1, sum2;
    char *json2 = NULL;

    test_make_temp_path(trace1, sizeof(trace1), "trace_wrap1.json");
    test_make_temp_path(trace2, sizeof(trace2), "trace_wrap2.json");
    DeleteFileA(trace1);
    DeleteFileA(trace2);

    /* Session 1: wrap 1024-event ring with 2500 events */
    TEST_ASSERT_EQ_INT(0, ztrace_init(trace1, 1024));
    for (int i = 0; i < 2499; i++) {
        ztrace_record_complete("test", "wrap_span", (uint64_t)(1000 + i), 10,
                               (uint64_t)i, 4096, 0, ZTRACE_ARGS_IO);
    }
    ztrace_record_instant("test", "wrap_marker", 0, 0, 0, ZTRACE_ARGS_NONE);
    TEST_ASSERT_EQ_INT(0, ztrace_shutdown(&sum1));
    TEST_ASSERT_EQ_INT(1024, (long long)sum1.events_written);
    TEST_ASSERT_EQ_INT(1476, (long long)sum1.dropped_events);

    /* Session 2: verify clean re-init on same thread (tls_epoch rotation) */
    TEST_ASSERT_EQ_INT(0, ztrace_init(trace2, 1024));
    ztrace_set_thread_name("Reinit-Thread");
    for (int i = 0; i < 5; i++) {
        ztrace_record_complete("test", "reinit_span", ztrace_now_qpc(), 5, 0, 0, 0, ZTRACE_ARGS_NONE);
    }
    TEST_ASSERT_EQ_INT(0, ztrace_shutdown(&sum2));
    TEST_ASSERT_EQ_INT(5, (long long)sum2.events_written);
    TEST_ASSERT_EQ_INT(0, (long long)sum2.dropped_events);

    json2 = read_entire_file(trace2, NULL);
    TEST_ASSERT(json2 != NULL);
    TEST_ASSERT(strstr(json2, "\"Reinit-Thread\"") != NULL);
    TEST_ASSERT(strstr(json2, "\"reinit_span\"") != NULL);
    TEST_ASSERT(strstr(json2, "\"wrap_span\"") == NULL);
    free(json2);

    DeleteFileA(trace1);
    DeleteFileA(trace2);
    return 0;
}

int run_suite_perfetto_trace(void) {
    int suite_failures = 0;
    RUN_TEST(test_trace_disabled_noop);
    RUN_TEST(test_trace_vfs_direct_standard_and_overlay);
    RUN_TEST(test_trace_winfsp_callback_spans);
    RUN_TEST(test_trace_ring_wrap_and_reinit);
    return suite_failures;
}
