#include "test_common.h"
#include "../src/io/win32_io.h"
#include <ext2fs/ext2_err.h>

static int create_patterned_file(const char *path, size_t size_bytes, uint8_t fill) {
    FILE *fp = fopen(path, "wb");
    uint8_t buf[4096];
    size_t rem = size_bytes;
    if (!fp) return -1;
    memset(buf, fill, sizeof(buf));
    while (rem > 0) {
        size_t chunk = rem > sizeof(buf) ? sizeof(buf) : rem;
        fwrite(buf, 1, chunk, fp);
        rem -= chunk;
    }
    fclose(fp);
    return 0;
}

static int test_4kb_rmw_and_alignment(void) {
    char path[MAX_PATH];
    win32_io_config_t cfg;
    io_channel io = NULL;
    uint8_t blk1[1024], blk_read[1024], sector_buf[4096];
    win32_io_stats_t stats;
    FILE *fp;

    test_make_temp_path(path, sizeof(path), "io_rmw.img");
    TEST_ASSERT(create_patterned_file(path, 65536, 0xAA) == 0);

    memset(&cfg, 0, sizeof(cfg));
    cfg.sector_size = 4096;
    cfg.max_cache_blocks = 16;
    win32_io_set_next_config(&cfg);

    TEST_ASSERT_EQ_INT(0, io_channel_open(win32_io_manager, path, IO_FLAG_RW, &io));
    TEST_ASSERT_EQ_INT(0, io_channel_set_blksize(io, 1024));

    for (size_t i = 0; i < sizeof(blk1); i++) blk1[i] = (uint8_t)(i & 0xFF);
    /* Write 1024-byte block 1 (offset 1024..2047) inside 4096-byte sector 0 -> forces RMW */
    TEST_ASSERT_EQ_INT(0, io_channel_write_blk64(io, 1, 1, blk1));
    TEST_ASSERT_EQ_INT(0, io_channel_flush(io));

    win32_io_get_stats(io, &stats);
    TEST_ASSERT(stats.rmw_operations >= 1);

    memset(blk_read, 0, sizeof(blk_read));
    TEST_ASSERT_EQ_INT(0, io_channel_read_blk64(io, 1, 1, blk_read));
    TEST_ASSERT(memcmp(blk1, blk_read, sizeof(blk1)) == 0);
    io_channel_close(io);

    /* Verify bytes outside offset 1024..2047 remained 0xAA */
    fp = fopen(path, "rb");
    TEST_ASSERT(fp != NULL);
    TEST_ASSERT_EQ_INT(sizeof(sector_buf), fread(sector_buf, 1, sizeof(sector_buf), fp));
    fclose(fp);
    for (size_t i = 0; i < 1024; i++) TEST_ASSERT_EQ_INT(0xAA, sector_buf[i]);
    TEST_ASSERT(memcmp(sector_buf + 1024, blk1, 1024) == 0);
    for (size_t i = 2048; i < 4096; i++) TEST_ASSERT_EQ_INT(0xAA, sector_buf[i]);

    DeleteFileA(path);
    return 0;
}

static int test_partition_boundary_guard(void) {
    char path[MAX_PATH];
    win32_io_config_t cfg;
    io_channel io = NULL;
    uint8_t buf[4096];
    uint8_t file_buf[32768];
    FILE *fp;

    test_make_temp_path(path, sizeof(path), "io_bounds.img");
    TEST_ASSERT(create_patterned_file(path, sizeof(file_buf), 0x55) == 0);

    memset(&cfg, 0, sizeof(cfg));
    cfg.partition_offset_bytes = 8192;
    cfg.partition_size_bytes = 16384; /* 4 blocks of 4096 bytes */
    cfg.sector_size = 4096;
    win32_io_set_next_config(&cfg);

    TEST_ASSERT_EQ_INT(0, io_channel_open(win32_io_manager, path, IO_FLAG_RW, &io));
    TEST_ASSERT_EQ_INT(0, io_channel_set_blksize(io, 4096));

    memset(buf, 0xCC, sizeof(buf));
    /* Block 3 is last valid block (offset 12288..16383 inside partition) */
    TEST_ASSERT_EQ_INT(0, io_channel_write_blk64(io, 3, 1, buf));
    /* Block 4 is outside partition boundary -> must fail with EXT2_ET_LLSEEK_FAILED */
    TEST_ASSERT_EQ_INT(EXT2_ET_LLSEEK_FAILED, io_channel_write_blk64(io, 4, 1, buf));
    TEST_ASSERT_EQ_INT(EXT2_ET_LLSEEK_FAILED, io_channel_read_blk64(io, 4, 1, buf));
    io_channel_close(io);

    fp = fopen(path, "rb");
    TEST_ASSERT(fp != NULL);
    TEST_ASSERT_EQ_INT(sizeof(file_buf), fread(file_buf, 1, sizeof(file_buf), fp));
    fclose(fp);

    /* Ensure bytes before offset 8192 and after offset 24576 are untouched (0x55) */
    for (size_t i = 0; i < 8192; i++) TEST_ASSERT_EQ_INT(0x55, file_buf[i]);
    for (size_t i = 8192 + 12288; i < 8192 + 16384; i++) TEST_ASSERT_EQ_INT(0xCC, file_buf[i]);
    for (size_t i = 24576; i < sizeof(file_buf); i++) TEST_ASSERT_EQ_INT(0x55, file_buf[i]);

    DeleteFileA(path);
    return 0;
}

static int test_read_only_and_lru_cache(void) {
    char path[MAX_PATH];
    win32_io_config_t cfg;
    io_channel io = NULL;
    uint8_t buf[4096];
    win32_io_stats_t stats;

    test_make_temp_path(path, sizeof(path), "io_ro_cache.img");
    TEST_ASSERT(create_patterned_file(path, 32768, 0x33) == 0);

    memset(&cfg, 0, sizeof(cfg));
    cfg.sector_size = 4096;
    cfg.max_cache_blocks = 8;
    cfg.read_only = 1;
    win32_io_set_next_config(&cfg);

    TEST_ASSERT_EQ_INT(0, io_channel_open(win32_io_manager, path, 0, &io));
    TEST_ASSERT_EQ_INT(0, io_channel_set_blksize(io, 4096));

    /* Write must be rejected in RO mode */
    memset(buf, 0x77, sizeof(buf));
    TEST_ASSERT_EQ_INT(EXT2_ET_RO_FILSYS, io_channel_write_blk64(io, 0, 1, buf));

    /* First read -> miss, next 3 reads -> hits */
    TEST_ASSERT_EQ_INT(0, io_channel_read_blk64(io, 0, 1, buf));
    TEST_ASSERT_EQ_INT(0x33, buf[0]);
    TEST_ASSERT_EQ_INT(0, io_channel_read_blk64(io, 0, 1, buf));
    TEST_ASSERT_EQ_INT(0, io_channel_read_blk64(io, 0, 1, buf));
    TEST_ASSERT_EQ_INT(0, io_channel_read_blk64(io, 0, 1, buf));

    win32_io_get_stats(io, &stats);
    TEST_ASSERT_EQ_INT(1, stats.cache_misses);
    TEST_ASSERT_EQ_INT(3, stats.cache_hits);

    io_channel_close(io);
    DeleteFileA(path);
    return 0;
}

typedef struct overlapped_reader_arg {
    io_channel io;
    uint32_t   seed;
    int        err;
} overlapped_reader_arg_t;

static DWORD WINAPI overlapped_reader_worker(LPVOID param) {
    overlapped_reader_arg_t *arg = (overlapped_reader_arg_t *)param;
    uint8_t *buf = (uint8_t *)_aligned_malloc(16384U, 4096U);
    uint32_t s = arg->seed;
    if (!buf) {
        arg->err = -1;
        return 1;
    }

    for (int iter = 0; iter < 1024; iter++) {
        uint32_t pages;
        uint32_t start_page;
        size_t req_len;
        errcode_t rc;

        s = s * 1664525U + 1013904223U;
        pages = ((s & 3U) == 0U) ? 4U : (((s & 1U) == 0U) ? 2U : 1U);
        req_len = (size_t)pages * 4096U;
        s = s * 1664525U + 1013904223U;
        start_page = (s >> 8) % (256U - pages + 1U);

        rc = win32_io_pread_direct(arg->io, (uint64_t)start_page * 4096ULL, buf, req_len);
        if (rc != 0) {
            arg->err = (int)rc;
            break;
        }
        for (uint32_t p = 0; p < pages; p++) {
            const uint8_t *pg = buf + (size_t)p * 4096U;
            uint32_t pg_idx = start_page + p;
            for (size_t b = 0; b < 4096U; b++) {
                uint8_t expected = (uint8_t)(((pg_idx * 37U) + b) & 0xFFU);
                if (pg[b] != expected) {
                    arg->err = -2;
                    _aligned_free(buf);
                    return 1;
                }
            }
        }
    }

    _aligned_free(buf);
    return arg->err ? 1 : 0;
}

static int test_overlapped_concurrent_pread(void) {
    char path[MAX_PATH];
    win32_io_config_t cfg;
    io_channel io = NULL;
    FILE *fp;
    uint8_t page[4096];
    overlapped_reader_arg_t args[8];
    HANDLE threads[8];
    uint8_t tail[4096];

    test_make_temp_path(path, sizeof(path), "io_overlapped_mt.img");
    fp = fopen(path, "wb");
    TEST_ASSERT(fp != NULL);
    for (uint32_t p = 0; p < 256; p++) {
        for (size_t b = 0; b < sizeof(page); b++) {
            page[b] = (uint8_t)(((p * 37U) + b) & 0xFFU);
        }
        TEST_ASSERT_EQ_INT(sizeof(page), fwrite(page, 1, sizeof(page), fp));
    }
    fclose(fp);

    memset(&cfg, 0, sizeof(cfg));
    cfg.sector_size = 4096;
    cfg.read_only = 1;
    cfg.disable_cache = 1;
    win32_io_set_next_config(&cfg);

    TEST_ASSERT_EQ_INT(0, io_channel_open(win32_io_manager, path, 0, &io));

    for (int i = 0; i < 8; i++) {
        args[i].io = io;
        args[i].seed = 0x9E3779B9U + (uint32_t)i * 7919U;
        args[i].err = 0;
        threads[i] = CreateThread(NULL, 0, overlapped_reader_worker, &args[i], 0, NULL);
        TEST_ASSERT(threads[i] != NULL);
    }
    WaitForMultipleObjects(8, threads, TRUE, INFINITE);
    for (int i = 0; i < 8; i++) {
        CloseHandle(threads[i]);
        TEST_ASSERT_EQ_INT(0, args[i].err);
    }

    /* Boundary check */
    TEST_ASSERT_EQ_INT(EXT2_ET_LLSEEK_FAILED,
                       win32_io_pread_direct(io, 256ULL * 4096ULL, tail, sizeof(tail)));

    io_channel_close(io);
    DeleteFileA(path);
    return 0;
}

int run_suite_win32_io(void) {
    int suite_failures = 0;
    RUN_TEST(test_4kb_rmw_and_alignment);
    RUN_TEST(test_partition_boundary_guard);
    RUN_TEST(test_read_only_and_lru_cache);
    RUN_TEST(test_overlapped_concurrent_pread);
    return suite_failures;
}
