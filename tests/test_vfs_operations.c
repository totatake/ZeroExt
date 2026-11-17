#include "test_common.h"
#include "../src/vfs/ext4_vfs.h"
#include "../src/io/win32_io.h"
#include <errno.h>

typedef struct {
    int found_a;
    int found_b;
    int count;
} readdir_count_t;

static int count_cb(const char *name, const ext4_stat_t *st, void *user_ctx) {
    readdir_count_t *rc = (readdir_count_t *)user_ctx;
    (void)st;
    rc->count++;
    if (strcmp(name, "alpha.txt") == 0) rc->found_a = 1;
    if (strcmp(name, "beta.bin") == 0) rc->found_b = 1;
    return 0;
}

static int test_vfs_crud_large_extent_and_persistence(void) {
    char img_path[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    ext4_file_handle_t *h = NULL;
    ext4_stat_t st;
    uint32_t written = 0, got = 0;
    const size_t large_sz = 3 * 1024 * 1024 + 12345; /* > 3 MB multi-block extents */
    uint8_t *wbuf = (uint8_t *)malloc(large_sz);
    uint8_t *rbuf = (uint8_t *)malloc(large_sz);

    TEST_ASSERT(wbuf != NULL && rbuf != NULL);
    for (size_t i = 0; i < large_sz; i++) {
        wbuf[i] = (uint8_t)((i * 131 + 17) & 0xFF);
    }

    test_make_temp_path(img_path, sizeof(img_path), "vfs_crud.img");
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 32ULL * 1024ULL * 1024ULL, "VFS_TEST"));

    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    cfg.case_insensitive = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));

    /* Directory creation & file creation */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkdir(ctx, "/sub", 0755, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkdir(ctx, "/sub/nested", 0755, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/sub/alpha.txt", 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/sub/beta.bin", 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));

    /* Write large file */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/sub/beta.bin", 1, &h));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, h->ino, wbuf, 0, (uint32_t)large_sz, 0, &written, NULL));
    TEST_ASSERT_EQ_INT((long long)large_sz, written);
    ext4_vfs_close_handle(h);

    /* ReadDirectory */
    {
        readdir_count_t rc_cnt = {0, 0, 0};
        ext4_file_handle_t *dh = NULL;
        TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/sub", 0, &dh));
        TEST_ASSERT_EQ_INT(0, ext4_vfs_readdir(ctx, dh->ino, count_cb, &rc_cnt));
        ext4_vfs_close_handle(dh);
        TEST_ASSERT_EQ_INT(1, rc_cnt.found_a);
        TEST_ASSERT_EQ_INT(1, rc_cnt.found_b);
    }

    /* Rename with replace check and cross-dir move */
    TEST_ASSERT_EQ_INT(-EEXIST, ext4_vfs_rename(ctx, "/sub/alpha.txt", "/sub/beta.bin", 0));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_rename(ctx, "/sub/alpha.txt", "/sub/nested/alpha_moved.txt", 0));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_rename(ctx, "/sub/nested/alpha_moved.txt", "/sub/nested/Alpha_Moved.txt", 0));

    /* Symlinks (fast <60B & slow >=60B) */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_symlink(ctx, "/sub/fast_link", "beta.bin"));
    {
        char long_target[256];
        memset(long_target, 'x', 180);
        memcpy(long_target, "/sub/nested/", 12);
        long_target[180] = '\0';
        TEST_ASSERT_EQ_INT(0, ext4_vfs_symlink(ctx, "/sub/long_link", long_target));
    }

    /* Non-empty directory delete guard */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/sub/nested", 0, &h));
    TEST_ASSERT_EQ_INT(-ENOTEMPTY, ext4_vfs_can_delete(ctx, h->ino));
    ext4_vfs_close_handle(h);
    TEST_ASSERT_EQ_INT(-ENOTEMPTY, ext4_vfs_unlink(ctx, "/sub/nested"));

    /* Label update */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_set_label(ctx, "UPDATED_EXT4"));

    /* Unmount and re-mount RO to verify full persistence + checksums */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    cfg.read_only = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_stat(ctx, "/sub/beta.bin", &st));
    TEST_ASSERT_EQ_INT((long long)large_sz, st.size);

    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/sub/beta.bin", 0, &h));
    memset(rbuf, 0, large_sz);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_read(ctx, h->ino, rbuf, 0, (uint32_t)large_sz, &got));
    TEST_ASSERT_EQ_INT((long long)large_sz, got);
    TEST_ASSERT(memcmp(wbuf, rbuf, large_sz) == 0);
    ext4_vfs_close_handle(h);

    /* Verify symlinks survived remount */
    {
        char link_out[256];
        TEST_ASSERT_EQ_INT(0, ext4_vfs_readlink(ctx, "/sub/fast_link", link_out, sizeof(link_out)));
        TEST_ASSERT(strcmp(link_out, "beta.bin") == 0);
        TEST_ASSERT_EQ_INT(0, ext4_vfs_readlink(ctx, "/sub/long_link", link_out, sizeof(link_out)));
        TEST_ASSERT_EQ_INT(180, (int)strlen(link_out));
    }

    {
        ext4_volume_stat_t vstat;
        TEST_ASSERT_EQ_INT(0, ext4_vfs_statfs(ctx, &vstat));
        TEST_ASSERT(strcmp(vstat.volume_label, "UPDATED_EXT4") == 0);
    }

    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));

    /* Re-mount RW and test truncate & deletion */
    cfg.read_only = 0;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/sub/beta.bin", 1, &h));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_truncate(ctx, h->ino, 100));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_stat_ino(ctx, h->ino, "beta.bin", &st));
    TEST_ASSERT_EQ_INT(100, st.size);
    ext4_vfs_close_handle(h);

    TEST_ASSERT_EQ_INT(0, ext4_vfs_unlink(ctx, "/sub/nested/Alpha_Moved.txt"));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unlink(ctx, "/sub/nested"));
    TEST_ASSERT_EQ_INT(-ENOENT, ext4_vfs_stat(ctx, "/sub/nested", &st));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));

    free(wbuf);
    free(rbuf);
    DeleteFileA(img_path);
    return 0;
}

static int test_fragmented_multi_level_extent_tree(void) {
    /* Force >4 non-contiguous extents so extent tree grows to depth >= 1 */
    char img_path[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    ext4_file_handle_t *hf = NULL, *hg = NULL;
    uint8_t blk[4096];
    uint32_t w = 0, r = 0;

    test_make_temp_path(img_path, sizeof(img_path), "vfs_ext_depth.img");
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 8ULL * 1024ULL * 1024ULL, "EXTDEPTH"));

    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));

    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/f1", 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/f2", 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/f1", 1, &hf));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/f2", 1, &hg));

    /* Interleave 10 blocks between f1 and f2 so f1 gets 10 separate extents (depth grows 0 -> 1) */
    for (int i = 0; i < 10; i++) {
        memset(blk, (uint8_t)(i + 1), sizeof(blk));
        TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, hf->ino, blk, (uint64_t)i * 4096ULL, 4096, 0, &w, NULL));
        memset(blk, (uint8_t)(i + 100), sizeof(blk));
        TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, hg->ino, blk, (uint64_t)i * 4096ULL, 4096, 0, &w, NULL));
    }
    ext4_vfs_close_handle(hf);
    ext4_vfs_close_handle(hg);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));

    /* Reopen and verify all 10 interleaved blocks of both files */
    cfg.read_only = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/f1", 0, &hf));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/f2", 0, &hg));
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQ_INT(0, ext4_vfs_read(ctx, hf->ino, blk, (uint64_t)i * 4096ULL, 4096, &r));
        TEST_ASSERT_EQ_INT(4096, r);
        TEST_ASSERT_EQ_INT((uint8_t)(i + 1), blk[0]);
        TEST_ASSERT_EQ_INT((uint8_t)(i + 1), blk[4095]);

        TEST_ASSERT_EQ_INT(0, ext4_vfs_read(ctx, hg->ino, blk, (uint64_t)i * 4096ULL, 4096, &r));
        TEST_ASSERT_EQ_INT(4096, r);
        TEST_ASSERT_EQ_INT((uint8_t)(i + 100), blk[0]);
    }
    ext4_vfs_close_handle(hf);
    ext4_vfs_close_handle(hg);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));

    DeleteFileA(img_path);
    return 0;
}

typedef struct {
    ext4_vfs_ctx_t *ctx;
    ext2_ino_t      ino;
    size_t          total_size;
    int             thread_id;
    int             error;
} mt_read_arg_t;

static DWORD WINAPI mt_read_worker(LPVOID param) {
    mt_read_arg_t *arg = (mt_read_arg_t *)param;
    uint8_t buf[8192];
    for (int iter = 0; iter < 64; iter++) {
        uint64_t off = ((uint64_t)((arg->thread_id * 17 + iter * 53) % 120)) * 4096ULL;
        uint32_t got = 0;
        if (off + sizeof(buf) > arg->total_size) off = 0;
        if (ext4_vfs_read(arg->ctx, arg->ino, buf, off, sizeof(buf), &got) != 0 || got != sizeof(buf)) {
            arg->error = 1;
            return 1;
        }
        for (size_t i = 0; i < got; i++) {
            uint8_t exp = (uint8_t)(((off + i) * 73 + 5) & 0xFF);
            if (buf[i] != exp) {
                arg->error = 2;
                return 1;
            }
        }
    }
    return 0;
}

static int test_concurrent_parallel_reads(void) {
    char img_path[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    ext4_file_handle_t *h = NULL;
    const size_t file_sz = 1024 * 1024;
    uint8_t *buf = (uint8_t *)malloc(file_sz);
    uint32_t w = 0;
    HANDLE threads[8];
    mt_read_arg_t args[8];

    TEST_ASSERT(buf != NULL);
    for (size_t i = 0; i < file_sz; i++) buf[i] = (uint8_t)((i * 73 + 5) & 0xFF);

    test_make_temp_path(img_path, sizeof(img_path), "vfs_mt_read.img");
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 8ULL * 1024ULL * 1024ULL, "MTREAD"));

    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/parallel.bin", 0, 0644, 0, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/parallel.bin", 1, &h));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, h->ino, buf, 0, (uint32_t)file_sz, 0, &w, NULL));
    TEST_ASSERT_EQ_INT((long long)file_sz, w);

    for (int t = 0; t < 8; t++) {
        args[t].ctx = ctx;
        args[t].ino = h->ino;
        args[t].total_size = file_sz;
        args[t].thread_id = t;
        args[t].error = 0;
        threads[t] = CreateThread(NULL, 0, mt_read_worker, &args[t], 0, NULL);
        TEST_ASSERT(threads[t] != NULL);
    }

    WaitForMultipleObjects(8, threads, TRUE, INFINITE);
    for (int t = 0; t < 8; t++) {
        CloseHandle(threads[t]);
        TEST_ASSERT_EQ_INT(0, args[t].error);
    }

    ext4_vfs_close_handle(h);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    free(buf);
    DeleteFileA(img_path);
    return 0;
}

static int test_dirty_journal_safety_policy(void) {
    char img_path[MAX_PATH];
    win32_io_config_t io_cfg;
    ext2_filsys fs = NULL;
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;

    test_make_temp_path(img_path, sizeof(img_path), "vfs_dirty_policy.img");
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 4ULL * 1024ULL * 1024ULL, "DIRTY"));

    memset(&io_cfg, 0, sizeof(io_cfg));
    io_cfg.sector_size = 4096;
    win32_io_set_next_config(&io_cfg);
    TEST_ASSERT_EQ_INT(0, ext2fs_open2(img_path, NULL, EXT2_FLAG_RW | EXT2_FLAG_64BITS, 0, 0, win32_io_manager, &fs));
    fs->super->s_feature_compat |= EXT3_FEATURE_COMPAT_HAS_JOURNAL;
    fs->super->s_feature_incompat |= EXT3_FEATURE_INCOMPAT_RECOVER;
    ext2fs_mark_super_dirty(fs);
    TEST_ASSERT_EQ_INT(0, ext2fs_close_free(&fs));

    /* 1. RW mount without fallback or force must be refused (-EROFS) */
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    cfg.ro_fallback_on_dirty = 0;
    TEST_ASSERT_EQ_INT(-EROFS, ext4_vfs_mount(&cfg, &ctx));

    /* 2. RW mount with ro_fallback_on_dirty=1 falls back safely to RO */
    cfg.ro_fallback_on_dirty = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(1, ctx->read_only);
    TEST_ASSERT_EQ_INT(1, ctx->fell_back_to_ro);
    TEST_ASSERT_EQ_INT(-EROFS, ext4_vfs_create(ctx, "/blocked.txt", 0, 0644, 0, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));

    DeleteFileA(img_path);
    return 0;
}

static int count_all_cb(const char *name, const ext4_stat_t *st, void *user_ctx) {
    int *cnt = (int *)user_ctx;
    (void)name;
    (void)st;
    (*cnt)++;
    return 0;
}

static int test_multiblock_directory_expansion(void) {
    char img_path[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    ext4_file_handle_t *dh = NULL;
    ext4_stat_t st;
    int cnt = 0;

    test_make_temp_path(img_path, sizeof(img_path), "vfs_bigdir.img");
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 8ULL * 1024ULL * 1024ULL, "BIGDIR"));

    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkdir(ctx, "/bigdir", 0755, NULL));

    /* 120 entries with 32-char names exceeds a single 4KB directory block -> triggers ext2fs_expand_dir */
    for (int i = 0; i < 120; i++) {
        char fpath[128];
        snprintf(fpath, sizeof(fpath), "/bigdir/long_filename_entry_%04d_test.txt", i);
        TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, fpath, 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
    }

    /* Delete even-numbered files */
    for (int i = 0; i < 120; i += 2) {
        char fpath[128];
        snprintf(fpath, sizeof(fpath), "/bigdir/long_filename_entry_%04d_test.txt", i);
        TEST_ASSERT_EQ_INT(0, ext4_vfs_unlink(ctx, fpath));
    }

    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));

    /* Remount RO and verify directory checksums, entry count (60 files + '.' + '..' = 62), and odd entries */
    cfg.read_only = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/bigdir", 0, &dh));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_readdir(ctx, dh->ino, count_all_cb, &cnt));
    ext4_vfs_close_handle(dh);
    TEST_ASSERT_EQ_INT(62, cnt);

    TEST_ASSERT_EQ_INT(0, ext4_vfs_stat(ctx, "/bigdir/long_filename_entry_0119_test.txt", &st));
    TEST_ASSERT_EQ_INT(-ENOENT, ext4_vfs_stat(ctx, "/bigdir/long_filename_entry_0118_test.txt", &st));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));

    DeleteFileA(img_path);
    return 0;
}

static void u32_le_write(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void u64_le_write(uint8_t *p, uint64_t v) {
    u32_le_write(p, (uint32_t)(v & 0xFFFFFFFFULL));
    u32_le_write(p + 4, (uint32_t)(v >> 32));
}

static int test_gpt_partition_rw_isolation(void) {
    char disk_path[MAX_PATH], part_img[MAX_PATH];
    uint8_t hdr[64 * 512];
    const uint64_t p1_off = 1024ULL * 512ULL;
    const uint64_t p1_sz  = 512ULL * 512ULL;
    const uint64_t p2_off = 4096ULL * 512ULL; /* 2 MB offset */
    const uint64_t p2_sz  = 4ULL * 1024ULL * 1024ULL;
    FILE *fp;
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    ext4_file_handle_t *h = NULL;
    uint32_t w = 0, r = 0;
    const char payload[] = "Isolated inside GPT partition 2";
    char rbuf[64] = {0};

    test_make_temp_path(disk_path, sizeof(disk_path), "vfs_gpt_rw.img");
    test_make_temp_path(part_img, sizeof(part_img), "vfs_gpt_rw_p2.img");
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(part_img, p2_sz, "GPT_RW_P2"));

    fp = fopen(disk_path, "wb");
    TEST_ASSERT(fp != NULL);
    {
        uint8_t sentinel[4096];
        memset(sentinel, 0x5A, sizeof(sentinel));
        for (uint64_t off = 0; off < p2_off + p2_sz; off += sizeof(sentinel)) {
            fwrite(sentinel, 1, sizeof(sentinel), fp);
        }
    }
    fclose(fp);

    memset(hdr, 0, sizeof(hdr));
    hdr[446 + 4] = 0xEE;
    u32_le_write(hdr + 446 + 8, 1);
    u32_le_write(hdr + 446 + 12, 16383);
    hdr[510] = 0x55; hdr[511] = 0xAA;

    memcpy(hdr + 512, "EFI PART", 8);
    u64_le_write(hdr + 512 + 0x48, 2);
    u32_le_write(hdr + 512 + 0x50, 128);
    u32_le_write(hdr + 512 + 0x54, 128);

    {
        uint8_t *e1 = hdr + 1024;
        uint8_t *e2 = hdr + 1024 + 128;
        uint8_t efi_guid[16] = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };
        uint8_t lnx_guid[16] = { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 };
        memcpy(e1, efi_guid, 16);
        u64_le_write(e1 + 0x20, p1_off / 512);
        u64_le_write(e1 + 0x28, (p1_off + p1_sz) / 512 - 1);
        memcpy(e2, lnx_guid, 16);
        u64_le_write(e2 + 0x20, p2_off / 512);
        u64_le_write(e2 + 0x28, (p2_off + p2_sz) / 512 - 1);
    }

    fp = fopen(disk_path, "r+b");
    TEST_ASSERT(fp != NULL);
    fwrite(hdr, 1, sizeof(hdr), fp);
    {
        FILE *pf = fopen(part_img, "rb");
        uint8_t buf[65536];
        size_t n;
        TEST_ASSERT(pf != NULL);
        fseek(fp, (long)p2_off, SEEK_SET);
        while ((n = fread(buf, 1, sizeof(buf), pf)) > 0) fwrite(buf, 1, n, fp);
        fclose(pf);
    }
    fclose(fp);

    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = disk_path;
    cfg.partition_number = 2;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/gpt_file.txt", 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/gpt_file.txt", 1, &h));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, h->ino, payload, 0, (uint32_t)strlen(payload), 0, &w, NULL));
    ext4_vfs_close_handle(h);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));

    /* Verify Partition 1 sentinel bytes remained 0x5A */
    fp = fopen(disk_path, "rb");
    TEST_ASSERT(fp != NULL);
    fseek(fp, (long)p1_off, SEEK_SET);
    for (uint64_t i = 0; i < 4096; i++) {
        int ch = fgetc(fp);
        TEST_ASSERT_EQ_INT(0x5A, ch);
    }
    fclose(fp);

    /* Remount Partition 2 RO and read back */
    cfg.read_only = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/gpt_file.txt", 0, &h));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_read(ctx, h->ino, rbuf, 0, sizeof(rbuf), &r));
    TEST_ASSERT(strcmp(rbuf, payload) == 0);
    ext4_vfs_close_handle(h);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));

    DeleteFileA(disk_path);
    DeleteFileA(part_img);
    return 0;
}

int run_suite_vfs_operations(void) {
    int suite_failures = 0;
    RUN_TEST(test_vfs_crud_large_extent_and_persistence);
    RUN_TEST(test_fragmented_multi_level_extent_tree);
    RUN_TEST(test_concurrent_parallel_reads);
    RUN_TEST(test_dirty_journal_safety_policy);
    RUN_TEST(test_multiblock_directory_expansion);
    RUN_TEST(test_gpt_partition_rw_isolation);
    return suite_failures;
}
