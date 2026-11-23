#include "test_common.h"
#include "../src/vfs/ext4_vfs.h"
#include "../src/extent/extent_map.h"
#include "../src/driver/winfsp_driver.h"
#include "../src/disk/superblock_probe.h"
#include <errno.h>

static void cleanup_dir_recursive(const char *dir_utf8) {
    wchar_t wdir[MAX_PATH];
    wchar_t pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (MultiByteToWideChar(CP_UTF8, 0, dir_utf8, -1, wdir, MAX_PATH) <= 0) return;

    _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%ls\\*", wdir);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t child[MAX_PATH];
            char child_u8[1024];
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            _snwprintf_s(child, MAX_PATH, _TRUNCATE, L"%ls\\%ls", wdir, fd.cFileName);
            SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                WideCharToMultiByte(CP_UTF8, 0, child, -1, child_u8, sizeof(child_u8), NULL, NULL);
                cleanup_dir_recursive(child_u8);
            } else {
                DeleteFileW(child);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(wdir);
}

static int test_direct_extent_deep_tree_and_holes(void) {
    char img_path[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    ext2_ino_t ino = 0;
    ext4_extent_map_t *map = NULL;
    uint32_t w = 0, r = 0;
    uint8_t blk[4096];
    uint8_t read_buf[8192];

    test_make_temp_path(img_path, sizeof(img_path), "ext_deep.img");
    DeleteFileA(img_path);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 32ULL * 1024ULL * 1024ULL, "EXTTEST"));

    /* 1. Direct Extent I/O must be rejected in non-overlay RW mode */
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    cfg.enable_direct_io = 1;
    TEST_ASSERT_EQ_INT(-EINVAL, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT(ctx == NULL);

    /* 1b. Contradictory enable_direct_io + disable_direct_io must be rejected */
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 1;
    cfg.enable_direct_io = 1;
    cfg.disable_direct_io = 1;
    TEST_ASSERT_EQ_INT(-EINVAL, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT(ctx == NULL);

    /* 2. RW mount automatically disables extent_cache */
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    cfg.case_insensitive = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ctx->enable_direct_io);
    TEST_ASSERT(ctx->extent_cache == NULL);

    /* Create a fragmented file with 8 non-contiguous extents (exceeds inline eh_max=4 -> eh_depth >= 1).
     * Also place data on another file in between so physical blocks cannot coalesce. */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/frag.bin", 0, 0644, 0, &ino));
    {
        ext2_ino_t sep_ino = 0;
        TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/sep.bin", 0, 0644, 0, &sep_ino));
        for (uint32_t i = 0; i < 8; i++) {
            uint64_t off = (uint64_t)i * 8192ULL; /* alternate 4KB data + 4KB sparse hole */
            memset(blk, (int)(0x30 + i), sizeof(blk));
            TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, ino, blk, off, sizeof(blk), 0, &w, NULL));
            TEST_ASSERT_EQ_INT(sizeof(blk), w);
            /* Allocate a block in sep_ino to break physical contiguity too */
            TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, sep_ino, blk, (uint64_t)i * 4096ULL, 4096, 0, &w, NULL));
        }
    }
    ext4_vfs_unmount(ctx);
    ctx = NULL;

    /* 3. Default RO mount automatically enables Direct I/O & extent_cache */
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 1;
    cfg.case_insensitive = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT(ctx->extent_cache != NULL);
    TEST_ASSERT_EQ_INT(1, ctx->enable_direct_io);

    TEST_ASSERT_EQ_INT(0, ext4_vfs_get_extents(ctx, "/frag.bin", &map));
    TEST_ASSERT_EQ_INT(EXT4_STORAGE_EXTENTS, map->kind);
    TEST_ASSERT_EQ_INT(8, (long long)map->run_count);
    TEST_ASSERT_EQ_INT(7ULL * 8192ULL + 4096ULL, (long long)map->file_size);

    /* Unaligned cross-hole direct read covering part of extent 2, sparse hole 2, and part of extent 3 */
    memset(read_buf, 0xAA, sizeof(read_buf));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_read(ctx, map->ino, read_buf, 2ULL * 8192ULL + 2000ULL, 7000U, &r));
    TEST_ASSERT_EQ_INT(7000, r);
    for (int j = 0; j < 2096; j++) TEST_ASSERT_EQ_INT(0x32, read_buf[j]);         /* tail of extent 2 */
    for (int j = 2096; j < 2096 + 4096; j++) TEST_ASSERT_EQ_INT(0x00, read_buf[j]); /* sparse hole */
    for (int j = 2096 + 4096; j < 7000; j++) TEST_ASSERT_EQ_INT(0x33, read_buf[j]); /* head of extent 3 */
    ext4_vfs_unmount(ctx);
    ctx = NULL;

    /* Verify exact match when disabling direct_io (--no-direct-io) on RO mount */
    {
        uint8_t cmp_buf[8192];
        uint32_t r2 = 0;
        cfg.disable_direct_io = 1;
        TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
        TEST_ASSERT_EQ_INT(0, ctx->enable_direct_io);
        TEST_ASSERT(ctx->extent_cache == NULL);
        TEST_ASSERT_EQ_INT(0, ext4_vfs_read(ctx, map->ino, cmp_buf, 2ULL * 8192ULL + 2000ULL, 7000U, &r2));
        TEST_ASSERT_EQ_INT(r, r2);
        TEST_ASSERT(memcmp(read_buf, cmp_buf, r) == 0);
        ext4_vfs_unmount(ctx);
    }

    ext4_extent_map_release(map);
    DeleteFileA(img_path);
    return 0;
}

static int test_extent_indirect_uninit_and_inline(void) {
    char img_path[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    ext2_ino_t ind_ino = 0, inl_ino = 0;
    ext4_extent_map_t *map = NULL;
    uint32_t r = 0;
    uint8_t buf[8192];

    test_make_temp_path(img_path, sizeof(img_path), "ext_kinds.img");
    DeleteFileA(img_path);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 16ULL * 1024ULL * 1024ULL, "EXTKIND"));

    /* Populate legacy indirect & inline test inodes in RW mode */
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));

    /* 1. Legacy indirect-block file (clear EXT4_EXTENTS_FL before writing blocks) */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/indirect.bin", 0, 0644, 0, &ind_ino));
    {
        struct ext2_inode inode;
        blk64_t new_blk = 0;
        uint8_t blk0[4096];
        AcquireSRWLockExclusive(&ctx->rwlock);
        TEST_ASSERT_EQ_INT(0, ext2fs_read_inode(ctx->fs, ind_ino, &inode));
        inode.i_flags &= ~EXT4_EXTENTS_FL;
        memset(inode.i_block, 0, sizeof(inode.i_block));
        TEST_ASSERT_EQ_INT(0, ext2fs_alloc_block2(ctx->fs, 0, NULL, &new_blk));
        inode.i_block[0] = (__u32)new_blk;
        ext2fs_inode_size_set(ctx->fs, &inode, 4096);
        ext2fs_iblk_add_blocks(ctx->fs, &inode, 1);
        TEST_ASSERT_EQ_INT(0, ext2fs_write_inode(ctx->fs, ind_ino, &inode));
        memset(blk0, 0x5A, sizeof(blk0));
        TEST_ASSERT_EQ_INT(0, io_channel_write_blk64(ctx->fs->io, new_blk, 1, blk0));
        ReleaseSRWLockExclusive(&ctx->rwlock);
    }

    /* 2. Inline-data file (EXT4_INLINE_DATA_FL) */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/inline.txt", 0, 0644, 0, &inl_ino));
    {
        struct ext2_inode inode;
        const char *msg = "inline-hello";
        AcquireSRWLockExclusive(&ctx->rwlock);
        TEST_ASSERT_EQ_INT(0, ext2fs_read_inode(ctx->fs, inl_ino, &inode));
        inode.i_flags &= ~EXT4_EXTENTS_FL;
        inode.i_flags |= EXT4_INLINE_DATA_FL;
        memset(inode.i_block, 0, sizeof(inode.i_block));
        memcpy(inode.i_block, msg, strlen(msg));
        ext2fs_inode_size_set(ctx->fs, &inode, strlen(msg));
        TEST_ASSERT_EQ_INT(0, ext2fs_write_inode(ctx->fs, inl_ino, &inode));
        ReleaseSRWLockExclusive(&ctx->rwlock);
    }
    ext4_vfs_unmount(ctx);
    ctx = NULL;

    /* Remount in default RO mode (Direct Extent I/O enabled automatically) */
    cfg.read_only = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(1, ctx->enable_direct_io);

    TEST_ASSERT_EQ_INT(0, ext4_vfs_get_extents(ctx, "/indirect.bin", &map));
    TEST_ASSERT_EQ_INT(EXT4_STORAGE_INDIRECT, map->kind);
    TEST_ASSERT_EQ_INT(1, (long long)map->run_count);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_read(ctx, ind_ino, buf, 100, 512, &r));
    TEST_ASSERT_EQ_INT(512, r);
    TEST_ASSERT_EQ_INT(0x5A, buf[0]);
    TEST_ASSERT_EQ_INT(0x5A, buf[511]);
    ext4_extent_map_release(map);
    map = NULL;

    TEST_ASSERT_EQ_INT(0, ext4_vfs_get_extents(ctx, "/inline.txt", &map));
    TEST_ASSERT_EQ_INT(EXT4_STORAGE_INLINE, map->kind);
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_read(ctx, inl_ino, buf, 7, 32, &r));
    TEST_ASSERT_EQ_INT(5, r);
    TEST_ASSERT(memcmp(buf, "hello", 5) == 0);
    ext4_extent_map_release(map);

    /* 3. Uninitialized extent unit check (must zero-fill without reading disk) */
    {
        ext4_extent_run_t u_run = { 0, 4096ULL, 4096ULL, 1 };
        ext4_extent_map_t u_map;
        uint8_t zbuf[64];
        memset(&u_map, 0, sizeof(u_map));
        u_map.ino = 999;
        u_map.file_size = 4096ULL;
        u_map.block_size = 4096U;
        u_map.kind = EXT4_STORAGE_EXTENTS;
        u_map.run_count = 1;
        u_map.runs = &u_run;
        memset(zbuf, 0xCC, sizeof(zbuf));
        TEST_ASSERT_EQ_INT(0, ext4_extent_pread(ctx->fs->io, &u_map, zbuf, 128, sizeof(zbuf), &r));
        TEST_ASSERT_EQ_INT(sizeof(zbuf), r);
        for (size_t k = 0; k < sizeof(zbuf); k++) TEST_ASSERT_EQ_INT(0, zbuf[k]);
    }

    ext4_vfs_unmount(ctx);
    DeleteFileA(img_path);
    return 0;
}

typedef struct ov_list_collector {
    char names[64][EXT2_NAME_LEN + 1];
    int  count;
} ov_list_collector_t;

static int ov_collect_cb(const char *name, const ext4_stat_t *st, void *user_ctx) {
    ov_list_collector_t *c = (ov_list_collector_t *)user_ctx;
    (void)st;
    if (c->count < 64) {
        strcpy_s(c->names[c->count++], EXT2_NAME_LEN + 1, name);
    }
    return 0;
}

static int collector_has(const ov_list_collector_t *c, const char *target) {
    for (int i = 0; i < c->count; i++) {
        if (strcmp(c->names[i], target) == 0) return 1;
    }
    return 0;
}

static int test_overlay_vfs_and_persistence(void) {
    char img_path[MAX_PATH];
    char upper_dir[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    ext2_ino_t ino = 0;
    uint32_t w = 0, r = 0;
    ext4_stat_t st;
    ext4_file_handle_t *h = NULL;
    char buf[256];
    ov_list_collector_t col;

    test_make_temp_path(img_path, sizeof(img_path), "ov_lower.img");
    test_make_temp_path(upper_dir, sizeof(upper_dir), "ov_upper_dir");
    DeleteFileA(img_path);
    cleanup_dir_recursive(upper_dir);

    /* Populate lower ext4 image */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 16ULL * 1024ULL * 1024ULL, "OVLOWER"));
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkdir(ctx, "/models", 0755, &ino));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/models/llama.gguf", 0, 0644, 0, &ino));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, ino, "GGUF-LOWER-WEIGHTS", 0, 18, 0, &w, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/config.txt", 0, 0644, 0, &ino));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, ino, "lower-v1", 0, 8, 0, &w, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkdir(ctx, "/old_dir", 0755, &ino));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/old_dir/secret.txt", 0, 0644, 0, &ino));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, ino, "old-secret", 0, 10, 0, &w, NULL));
    /* Mark superblock dirty (EXT3_FEATURE_INCOMPAT_RECOVER) to prove overlay safely mounts dirty ext4 RO */
    AcquireSRWLockExclusive(&ctx->rwlock);
    ctx->fs->super->s_feature_incompat |= EXT3_FEATURE_INCOMPAT_RECOVER;
    ext2fs_mark_super_dirty(ctx->fs);
    ReleaseSRWLockExclusive(&ctx->rwlock);
    ext4_vfs_unmount(ctx);
    ctx = NULL;

    /* Mount with Overlay (default Direct Extent I/O enabled automatically) even though lower ext4 has dirty state */
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    cfg.case_insensitive = 1;
    cfg.overlay_upper_dir = upper_dir;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(1, ctx->lower_read_only);
    TEST_ASSERT_EQ_INT(0, ctx->read_only);
    TEST_ASSERT_EQ_INT(1, ctx->enable_direct_io);
    TEST_ASSERT(ctx->extent_cache != NULL);

    /* 1. Direct Extent read of lower-only file through overlay */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/models/llama.gguf", 0, &h));
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_read(ctx, h->ino, buf, 0, sizeof(buf), &r));
    TEST_ASSERT_EQ_INT(18, r);
    TEST_ASSERT(strcmp(buf, "GGUF-LOWER-WEIGHTS") == 0);
    ext4_vfs_close_handle(h);

    /* 2. Copy-up on write to lower file (/config.txt -> append & patch) */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/config.txt", 1, &h));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, h->ino, "UPPER!", 6, 6, 0, &w, NULL));
    TEST_ASSERT_EQ_INT(6, w);
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_read(ctx, h->ino, buf, 0, sizeof(buf), &r));
    TEST_ASSERT_EQ_INT(12, r);
    TEST_ASSERT(strcmp(buf, "lower-UPPER!") == 0);
    ext4_vfs_close_handle(h);

    /* 3. Create new file in upper inside lower directory */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/models/cache.bin", 0, 0644, 0, &ino));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, ino, "cache-data", 0, 10, 0, &w, NULL));

    /* Merged readdir should show both llama.gguf (lower) and cache.bin (upper) without duplicates */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_stat(ctx, "/models", &st));
    memset(&col, 0, sizeof(col));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_readdir(ctx, st.ino, ov_collect_cb, &col));
    TEST_ASSERT(collector_has(&col, "llama.gguf"));
    TEST_ASSERT(collector_has(&col, "cache.bin"));

    /* 4. Unlink lower dir contents -> rmdir -> recreate same dir (must be opaque, old child gone) */
    TEST_ASSERT_EQ_INT(-ENOTEMPTY, ext4_vfs_unlink(ctx, "/old_dir"));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unlink(ctx, "/old_dir/secret.txt"));
    TEST_ASSERT_EQ_INT(-ENOENT, ext4_vfs_stat(ctx, "/old_dir/secret.txt", &st));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unlink(ctx, "/old_dir"));
    TEST_ASSERT_EQ_INT(-ENOENT, ext4_vfs_stat(ctx, "/old_dir", &st));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkdir(ctx, "/old_dir", 0755, &ino));
    TEST_ASSERT_EQ_INT(-ENOENT, ext4_vfs_stat(ctx, "/old_dir/secret.txt", &st));

    /* 5. Rename lower-only file and verify whiteout hides old path */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_rename(ctx, "/models/llama.gguf", "/models/llama_renamed.gguf", 1));
    TEST_ASSERT_EQ_INT(-ENOENT, ext4_vfs_stat(ctx, "/models/llama.gguf", &st));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_stat(ctx, "/models/llama_renamed.gguf", &st));
    TEST_ASSERT_EQ_INT(18, st.size);

    /* Root readdir must NOT leak .wh.* whiteout markers */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_stat(ctx, "/", &st));
    memset(&col, 0, sizeof(col));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_readdir(ctx, st.ino, ov_collect_cb, &col));
    for (int i = 0; i < col.count; i++) {
        TEST_ASSERT(strncmp(col.names[i], ".wh.", 4) != 0);
    }

    /* WinFsp callback smoke test on Overlay */
    {
        FSP_FILE_SYSTEM *fs = NULL;
        const FSP_FILE_SYSTEM_INTERFACE *iface = winfsp_driver_get_interface();
        PVOID fctx = NULL;
        FSP_FSCTL_FILE_INFO fi;
        ULONG bt = 0;
        char wbuf[64] = {0};

        TEST_ASSERT_EQ_INT(0, winfsp_driver_create_test_fs(ctx, &fs));
        TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                           iface->Create(fs, L"\\upper_winfsp.txt", 0, FILE_GENERIC_WRITE,
                                         FILE_ATTRIBUTE_ARCHIVE, NULL, 0, &fctx, &fi));
        TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                           iface->Write(fs, fctx, "winfsp-ov", 0, 9, FALSE, FALSE, &bt, &fi));
        TEST_ASSERT_EQ_INT(9, bt);
        iface->Cleanup(fs, fctx, L"\\upper_winfsp.txt", 0);
        iface->Close(fs, fctx);

        TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                           iface->Open(fs, L"\\upper_winfsp.txt", 0, FILE_GENERIC_READ, &fctx, &fi));
        TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->Read(fs, fctx, wbuf, 0, sizeof(wbuf), &bt));
        TEST_ASSERT_EQ_INT(9, bt);
        TEST_ASSERT(memcmp(wbuf, "winfsp-ov", 9) == 0);
        iface->Close(fs, fctx);
        winfsp_driver_destroy_test_fs(fs);
    }

    ext4_vfs_unmount(ctx);
    ctx = NULL;

    /* 6. Remount overlay and confirm persistence, while lower image remained completely untouched! */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/config.txt", 0, &h));
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_read(ctx, h->ino, buf, 0, sizeof(buf), &r));
    TEST_ASSERT(strcmp(buf, "lower-UPPER!") == 0);
    ext4_vfs_close_handle(h);
    TEST_ASSERT_EQ_INT(-ENOENT, ext4_vfs_stat(ctx, "/old_dir/secret.txt", &st));
    ext4_vfs_unmount(ctx);
    ctx = NULL;

    /* 7. Different lower image reusing the same upperdir must be rejected (.wh.__ext4_lower_id guard) */
    {
        char other_img[MAX_PATH];
        ext4_vfs_ctx_t *bad_ctx = NULL;
        test_make_temp_path(other_img, sizeof(other_img), "ov_lower_other.img");
        DeleteFileA(other_img);
        /* Create with same 16MB size to ensure distinct UUIDs alone catch mismatch */
        TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(other_img, 16ULL * 1024ULL * 1024ULL, "OTHER"));
        cfg.source_path = other_img;
        cfg.read_only = 0;
        cfg.overlay_upper_dir = upper_dir;
        TEST_ASSERT_EQ_INT(-EINVAL, ext4_vfs_mount(&cfg, &bad_ctx));
        TEST_ASSERT(bad_ctx == NULL);
        DeleteFileA(other_img);
        cfg.source_path = img_path;
    }

    /* Check lower alone (RO) -> original lower-v1 and old_dir/secret.txt are intact */
    cfg.overlay_upper_dir = NULL;
    cfg.read_only = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/config.txt", 0, &h));
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_read(ctx, h->ino, buf, 0, sizeof(buf), &r));
    TEST_ASSERT_EQ_INT(8, r);
    TEST_ASSERT(strcmp(buf, "lower-v1") == 0);
    ext4_vfs_close_handle(h);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_stat(ctx, "/old_dir/secret.txt", &st));
    ext4_vfs_unmount(ctx);

    cleanup_dir_recursive(upper_dir);
    DeleteFileA(img_path);
    return 0;
}

typedef struct handle_reader_arg {
    ext4_vfs_ctx_t     *ctx;
    ext4_file_handle_t *handle;
    int                 tid_idx;
    int                 err;
} handle_reader_arg_t;

static DWORD WINAPI handle_reader_worker(LPVOID param) {
    handle_reader_arg_t *a = (handle_reader_arg_t *)param;
    uint8_t buf[4096];
    for (uint64_t iter = 0; iter < 64; iter++) {
        uint64_t pg = ((iter * 3U + (uint64_t)a->tid_idx) % 16U);
        uint32_t got = 0;
        int rc = ext4_vfs_read_handle(a->ctx, a->handle, buf, pg * 4096ULL, sizeof(buf), &got);
        if (rc != 0 || got != sizeof(buf)) {
            a->err = rc ? rc : -5;
            return 1;
        }
        for (size_t b = 0; b < sizeof(buf); b++) {
            uint8_t expected = (uint8_t)((pg + b) & 0xFFU);
            if (buf[b] != expected) {
                a->err = -6;
                return 1;
            }
        }
    }
    return 0;
}

static int test_read_handle_cached_extents(void) {
    char img_path[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    ext2_ino_t ino = 0;
    ext4_file_handle_t *h = NULL;
    uint8_t page[4096];
    uint32_t w = 0;
    handle_reader_arg_t args[4];
    HANDLE threads[4];

    test_make_temp_path(img_path, sizeof(img_path), "ext_handle.img");
    DeleteFileA(img_path);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 8ULL * 1024ULL * 1024ULL, "EXTHDL"));

    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/data.bin", 0, 0644, 0, &ino));
    for (uint32_t pg = 0; pg < 16; pg++) {
        for (size_t b = 0; b < sizeof(page); b++) page[b] = (uint8_t)((pg + b) & 0xFFU);
        TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, ino, page, (uint64_t)pg * 4096ULL, sizeof(page), 0, &w, NULL));
        TEST_ASSERT_EQ_INT(sizeof(page), w);
    }
    ext4_vfs_unmount(ctx);
    ctx = NULL;

    /* Mount strictly Read-Only with Direct I/O ON (default) */
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 1;
    cfg.case_insensitive = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));

    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/data.bin", 0, &h));
    TEST_ASSERT(h != NULL);
    TEST_ASSERT(h->cached_extents == NULL);

    for (int i = 0; i < 4; i++) {
        args[i].ctx = ctx;
        args[i].handle = h;
        args[i].tid_idx = i;
        args[i].err = 0;
        threads[i] = CreateThread(NULL, 0, handle_reader_worker, &args[i], 0, NULL);
        TEST_ASSERT(threads[i] != NULL);
    }
    WaitForMultipleObjects(4, threads, TRUE, INFINITE);
    for (int i = 0; i < 4; i++) {
        CloseHandle(threads[i]);
        TEST_ASSERT_EQ_INT(0, args[i].err);
    }
    TEST_ASSERT(h->cached_extents != NULL);

    ext4_vfs_close_handle(h);
    ext4_vfs_unmount(ctx);
    DeleteFileA(img_path);
    return 0;
}

static int test_overlay_case_sensitive_upperdir(void) {
    char img_path[MAX_PATH];
    char upper_dir[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    ext2_ino_t ino = 0;
    uint32_t w = 0, r = 0;
    ext4_stat_t st;
    char buf[64];
    int rc;

    test_make_temp_path(img_path, sizeof(img_path), "ov_cs_lower.img");
    test_make_temp_path(upper_dir, sizeof(upper_dir), "ov_cs_upper_dir");
    DeleteFileA(img_path);
    cleanup_dir_recursive(upper_dir);

    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 8ULL * 1024ULL * 1024ULL, "OVCS"));
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    cfg.case_insensitive = 0;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/Foo.txt", 0, 0644, 0, &ino));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, ino, "lower-FOO", 0, 9, 0, &w, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/foo.txt", 0, 0644, 0, &ino));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, ino, "lower-foo", 0, 9, 0, &w, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkdir(ctx, "/dir", 0755, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/dir/A.txt", 0, 0644, 0, &ino));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, ino, "lower-A", 0, 7, 0, &w, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/dir/a.txt", 0, 0644, 0, &ino));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, ino, "lower-a", 0, 7, 0, &w, NULL));
    ext4_vfs_unmount(ctx);
    ctx = NULL;

    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    cfg.case_insensitive = 0;
    cfg.overlay_upper_dir = upper_dir;
    rc = ext4_vfs_mount(&cfg, &ctx);
    if (rc != 0) {
        /* If host filesystem does not support NTFS FileCaseSensitiveInfo, it must fail explicitly */
        TEST_ASSERT(rc == -ENOTSUP || rc == -EACCES || rc == -ENOTEMPTY);
        cleanup_dir_recursive(upper_dir);
        DeleteFileA(img_path);
        return 0;
    }

    /* Independent copy-up & overwrite for /Foo.txt and /foo.txt */
    {
        ext4_file_handle_t *h1 = NULL, *h2 = NULL;
        TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/Foo.txt", 1, &h1));
        TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/foo.txt", 1, &h2));
        TEST_ASSERT(h1->ino != h2->ino);
        TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, h1->ino, "upper-FOO!", 0, 10, 0, &w, NULL));
        TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, h2->ino, "upper-foo!", 0, 10, 0, &w, NULL));
        ext4_vfs_close_handle(h1);
        ext4_vfs_close_handle(h2);
    }

    /* Case-distinct subdirectories and files created in upperdir */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkdir(ctx, "/Sub", 0755, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkdir(ctx, "/sub", 0755, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/Sub/data.txt", 0, 0644, 0, &ino));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, ino, "SUB-U", 0, 5, 0, &w, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/sub/data.txt", 0, 0644, 0, &ino));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, ino, "sub-l", 0, 5, 0, &w, NULL));

    /* Unlink one case variant in lower-backed dir */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unlink(ctx, "/dir/a.txt"));
    TEST_ASSERT_EQ_INT(-ENOENT, ext4_vfs_stat(ctx, "/dir/a.txt", &st));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_stat(ctx, "/dir/A.txt", &st));
    TEST_ASSERT_EQ_INT(-ENOENT, ext4_vfs_stat(ctx, "/FOO.TXT", &st));

    ext4_vfs_unmount(ctx);
    ctx = NULL;

    /* Reusing case-sensitive upperdir with case_insensitive=1 must be rejected */
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    cfg.case_insensitive = 1;
    cfg.overlay_upper_dir = upper_dir;
    TEST_ASSERT_EQ_INT(-EINVAL, ext4_vfs_mount(&cfg, &ctx));

    /* Reopening with case_insensitive=0 must preserve distinct upperdir contents */
    cfg.case_insensitive = 0;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));

    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_stat(ctx, "/Foo.txt", &st));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_read(ctx, st.ino, buf, 0, sizeof(buf), &r));
    TEST_ASSERT(strcmp(buf, "upper-FOO!") == 0);

    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_stat(ctx, "/foo.txt", &st));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_read(ctx, st.ino, buf, 0, sizeof(buf), &r));
    TEST_ASSERT(strcmp(buf, "upper-foo!") == 0);

    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_stat(ctx, "/Sub/data.txt", &st));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_read(ctx, st.ino, buf, 0, sizeof(buf), &r));
    TEST_ASSERT(strcmp(buf, "SUB-U") == 0);

    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_stat(ctx, "/sub/data.txt", &st));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_read(ctx, st.ino, buf, 0, sizeof(buf), &r));
    TEST_ASSERT(strcmp(buf, "sub-l") == 0);

    TEST_ASSERT_EQ_INT(-ENOENT, ext4_vfs_stat(ctx, "/dir/a.txt", &st));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_stat(ctx, "/dir/A.txt", &st));

    ext4_vfs_unmount(ctx);
    cleanup_dir_recursive(upper_dir);
    DeleteFileA(img_path);
    return 0;
}

int run_suite_extent_and_overlay(void) {
    int suite_failures = 0;
    printf("\n=== Suite: extent_and_overlay ===\n");
    RUN_TEST(test_direct_extent_deep_tree_and_holes);
    RUN_TEST(test_extent_indirect_uninit_and_inline);
    RUN_TEST(test_overlay_vfs_and_persistence);
    RUN_TEST(test_read_handle_cached_extents);
    RUN_TEST(test_overlay_case_sensitive_upperdir);
    return suite_failures;
}
