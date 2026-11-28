#include "test_common.h"
#include "../src/vfs/ext4_vfs.h"
#include "../src/extent/extent_map.h"
#include "../src/overlay/overlay_vfs.h"
#include "../src/overlay/overlay_tools.h"
#include "../src/common/win_namespace.h"
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

static void write_le32_ov(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void write_le64_ov(uint8_t *p, uint64_t v) {
    write_le32_ov(p, (uint32_t)(v & 0xFFFFFFFFULL));
    write_le32_ov(p + 4, (uint32_t)(v >> 32));
}

static int create_gpt_with_raw_part(const char *disk_path, const char *part_img, const uint8_t part_guid[16]) {
    uint8_t *hdr = (uint8_t *)calloc(64, 512);
    uint8_t *buf = (uint8_t *)calloc(1, 65536);
    const uint64_t part2_lba = 4096;
    const uint64_t part_sectors = (4ULL * 1024ULL * 1024ULL) / 512ULL;
    FILE *fp;
    size_t n;

    if (!hdr || !buf) {
        free(hdr);
        free(buf);
        return -1;
    }

    fp = fopen(disk_path, "wb");
    if (!fp) {
        free(hdr);
        free(buf);
        return -1;
    }
    for (int i = 0; i < 128; i++) fwrite(buf, 1, 65536, fp);
    fclose(fp);

    hdr[446 + 4] = 0xEE;
    write_le32_ov(hdr + 446 + 8, 1);
    write_le32_ov(hdr + 446 + 12, 16383);
    hdr[510] = 0x55;
    hdr[511] = 0xAA;

    memcpy(hdr + 512, "EFI PART", 8);
    write_le64_ov(hdr + 512 + 0x48, 2);
    write_le32_ov(hdr + 512 + 0x50, 128);
    write_le32_ov(hdr + 512 + 0x54, 128);

    {
        uint8_t *e2 = hdr + 1024 + 128;
        uint8_t linux_guid[16] = { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 };
        memcpy(e2, linux_guid, 16);
        memcpy(e2 + 0x10, part_guid, 16);
        write_le64_ov(e2 + 0x20, part2_lba);
        write_le64_ov(e2 + 0x28, part2_lba + part_sectors - 1);
        e2[0x38] = 'r'; e2[0x3A] = 'o'; e2[0x3C] = 'o'; e2[0x3E] = 't';
    }

    fp = fopen(disk_path, "r+b");
    if (!fp) {
        free(hdr);
        free(buf);
        return -1;
    }
    fwrite(hdr, 1, 64 * 512, fp);
    fclose(fp);

    {
        FILE *src = fopen(part_img, "rb");
        FILE *dst = fopen(disk_path, "r+b");
        if (!src || !dst) {
            if (src) fclose(src);
            if (dst) fclose(dst);
            free(hdr);
            free(buf);
            return -1;
        }
        _fseeki64(dst, (__int64)(part2_lba * 512ULL), SEEK_SET);
        while ((n = fread(buf, 1, 65536, src)) > 0) {
            fwrite(buf, 1, n, dst);
        }
        fclose(src);
        fclose(dst);
    }
    free(hdr);
    free(buf);
    return 0;
}

static int test_overlay_upperdir_guards_v2(void) {
    char img_path[MAX_PATH];
    char legacy_upper[MAX_PATH];
    char fresh_upper[MAX_PATH];
    char clone_a[MAX_PATH], clone_b[MAX_PATH], clone_part[MAX_PATH], clone_upper[MAX_PATH];
    char existing_file[MAX_PATH], legacy_marker[MAX_PATH], marker_file[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    ext2_ino_t ino = 0;
    ext4_stat_t st;
    uint32_t w = 0, r = 0;
    char buf[512];
    FILE *fp;
    ext4_probe_result_t probe;
    overlay_lower_identity_t lid;
    overlay_upper_check_t chk;

    test_make_temp_path(img_path, sizeof(img_path), "ov_guard_base.img");
    test_make_temp_path(legacy_upper, sizeof(legacy_upper), "ov_guard_legacy_upper");
    test_make_temp_path(fresh_upper, sizeof(fresh_upper), "ov_guard_fresh_upper");
    test_make_temp_path(clone_a, sizeof(clone_a), "ov_clone_a.img");
    test_make_temp_path(clone_b, sizeof(clone_b), "ov_clone_b.img");
    test_make_temp_path(clone_part, sizeof(clone_part), "ov_clone_part.img");
    test_make_temp_path(clone_upper, sizeof(clone_upper), "ov_clone_upper");

    DeleteFileA(img_path);
    DeleteFileA(clone_a);
    DeleteFileA(clone_b);
    DeleteFileA(clone_part);
    cleanup_dir_recursive(legacy_upper);
    cleanup_dir_recursive(fresh_upper);
    cleanup_dir_recursive(clone_upper);

    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 16ULL * 1024ULL * 1024ULL, "OVGUARD"));
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/hello.txt", 0, 0644, 0, &ino));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, ino, "lower-hi", 0, 8, 0, &w, NULL));
    ext4_vfs_unmount(ctx);
    ctx = NULL;

    /* 1. Non-empty unmarked upperdir must be unconditionally refused without modifying directory */
    TEST_ASSERT(CreateDirectoryA(legacy_upper, NULL) != 0);
    _snprintf_s(existing_file, sizeof(existing_file), _TRUNCATE, "%s\\existing.txt", legacy_upper);
    _snprintf_s(legacy_marker, sizeof(legacy_marker), _TRUNCATE, "%s\\.wh.__ext4_lower_id", legacy_upper);
    fp = fopen(existing_file, "wb");
    TEST_ASSERT(fp != NULL);
    fwrite("user-data", 1, 9, fp);
    fclose(fp);

    TEST_ASSERT_EQ_INT(0, ext4_probe_partition(img_path, 0, 0, 4096, &probe));
    memset(&lid, 0, sizeof(lid));
    strcpy_s(lid.fs_uuid, sizeof(lid.fs_uuid), probe.uuid);
    lid.blocks_count = probe.blocks_count;
    lid.block_size = probe.block_size;
    lid.partition_size = probe.total_bytes;
    lid.case_sensitive = 0;
    overlay_check_upper_dir(legacy_upper, &lid, &chk);
    TEST_ASSERT_EQ_INT(OV_UPPER_NON_EMPTY_UNMARKED, chk.status);
    TEST_ASSERT_EQ_INT(0, chk.can_mount);

    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    cfg.case_insensitive = 1;
    cfg.overlay_upper_dir = legacy_upper;
    TEST_ASSERT_EQ_INT(-ENOTEMPTY, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT(ctx == NULL);
    TEST_ASSERT_EQ_INT(INVALID_FILE_ATTRIBUTES, GetFileAttributesA(legacy_marker));

    /* 2. Fresh empty upperdir writes v2 marker and remounts cleanly */
    cfg.overlay_upper_dir = fresh_upper;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_stat(ctx, "/hello.txt", &st));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_read(ctx, st.ino, buf, 0, 8, &r));
    TEST_ASSERT_EQ_INT(8, r);
    TEST_ASSERT(memcmp(buf, "lower-hi", 8) == 0);
    ext4_vfs_unmount(ctx);
    ctx = NULL;

    _snprintf_s(marker_file, sizeof(marker_file), _TRUNCATE, "%s\\.wh.__ext4_lower_id", fresh_upper);
    fp = fopen(marker_file, "rb");
    TEST_ASSERT(fp != NULL);
    memset(buf, 0, sizeof(buf));
    fread(buf, 1, sizeof(buf) - 1, fp);
    buf[sizeof(buf) - 1] = '\0';
    fclose(fp);
    TEST_ASSERT(strncmp(buf, "v2;", 3) == 0);

    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    ext4_vfs_unmount(ctx);
    ctx = NULL;

    /* 3. Corrupted marker & unknown future format version rejection */
    SetFileAttributesA(marker_file, FILE_ATTRIBUTE_NORMAL);
    fp = fopen(marker_file, "wb");
    TEST_ASSERT(fp != NULL);
    fwrite("corrupt-garbage", 1, 15, fp);
    fclose(fp);
    overlay_check_upper_dir(fresh_upper, &lid, &chk);
    TEST_ASSERT_EQ_INT(OV_UPPER_CORRUPT_MARKER, chk.status);
    TEST_ASSERT_EQ_INT(0, chk.can_mount);
    TEST_ASSERT_EQ_INT(-EINVAL, ext4_vfs_mount(&cfg, &ctx));

    fp = fopen(marker_file, "wb");
    TEST_ASSERT(fp != NULL);
    fclose(fp);
    TEST_ASSERT_EQ_INT(-EINVAL, ext4_vfs_mount(&cfg, &ctx));

    fp = fopen(marker_file, "wb");
    TEST_ASSERT(fp != NULL);
    fprintf(fp, "v99;uuid=%s;blocks=%llu;bsize=%u",
            probe.uuid, (unsigned long long)probe.blocks_count, (unsigned)probe.block_size);
    fclose(fp);
    overlay_check_upper_dir(fresh_upper, &lid, &chk);
    TEST_ASSERT_EQ_INT(OV_UPPER_UNSUPPORTED_VERSION, chk.status);
    TEST_ASSERT_EQ_INT(0, chk.can_mount);
    TEST_ASSERT_EQ_INT(-ENOTSUP, ext4_vfs_mount(&cfg, &ctx));

    /* 4. Backward-compatible v1 marker remains mountable */
    fp = fopen(marker_file, "wb");
    TEST_ASSERT(fp != NULL);
    fprintf(fp, "%s:%llu:%u",
            probe.uuid, (unsigned long long)probe.blocks_count, (unsigned)probe.block_size);
    fclose(fp);
    overlay_check_upper_dir(fresh_upper, &lid, &chk);
    TEST_ASSERT_EQ_INT(OV_UPPER_OK_MATCH_V1, chk.status);
    TEST_ASSERT_EQ_INT(1, chk.can_mount);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    ext4_vfs_unmount(ctx);
    ctx = NULL;

    /* 5. Cloned partition with identical ext4 UUID/size but distinct GPT PartUUID must be rejected */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(clone_part, 4ULL * 1024ULL * 1024ULL, "CLONEEXT4"));
    {
        uint8_t guid_a[16] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x48, 0x89, 0xAB, 0xCD, 0xEF, 0x00, 0x00, 0x00, 0x0A };
        uint8_t guid_b[16] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x48, 0x89, 0xAB, 0xCD, 0xEF, 0x00, 0x00, 0x00, 0x0B };
        TEST_ASSERT_EQ_INT(0, create_gpt_with_raw_part(clone_a, clone_part, guid_a));
        TEST_ASSERT_EQ_INT(0, create_gpt_with_raw_part(clone_b, clone_part, guid_b));
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = clone_a;
    cfg.partition_number = 2;
    cfg.read_only = 0;
    cfg.case_insensitive = 1;
    cfg.overlay_upper_dir = clone_upper;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    ext4_vfs_unmount(ctx);
    ctx = NULL;

    cfg.source_path = clone_b;
    TEST_ASSERT_EQ_INT(-EINVAL, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT(ctx == NULL);

    cleanup_dir_recursive(legacy_upper);
    cleanup_dir_recursive(fresh_upper);
    cleanup_dir_recursive(clone_upper);
    DeleteFileA(img_path);
    DeleteFileA(clone_a);
    DeleteFileA(clone_b);
    DeleteFileA(clone_part);
    return 0;
}

static int test_overlay_hashmap_scalability_and_deterministic_inodes(void) {
    char img_path[MAX_PATH];
    char upper_dir[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    ext4_file_handle_t *h = NULL;
    ext4_stat_t st_first, st_second;
    ext2_ino_t synth_before = 0;

    test_make_temp_path(img_path, sizeof(img_path), "ov_hash_scale.img");
    test_make_temp_path(upper_dir, sizeof(upper_dir), "ov_hash_scale_upper");
    DeleteFileA(img_path);
    cleanup_dir_recursive(upper_dir);

    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 16ULL * 1024ULL * 1024ULL, "OVHASH"));
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    cfg.case_insensitive = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/base.txt", 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    cfg.overlay_upper_dir = upper_dir;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkdir(ctx, "/many", 0755, NULL));

    for (int i = 0; i < 650; i++) {
        char fpath[128];
        char payload[64];
        uint32_t w = 0;
        snprintf(fpath, sizeof(fpath), "/many/file_%04d.dat", i);
        snprintf(payload, sizeof(payload), "item-%04d", i);
        TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, fpath, 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
        TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, fpath, 1, &h));
        TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, h->ino, payload, 0, (uint32_t)strlen(payload), 0, &w, NULL));
        ext4_vfs_close_handle(h);
        h = NULL;
    }

    TEST_ASSERT_EQ_INT(0, ext4_vfs_stat(ctx, "/many/file_0420.dat", &st_first));
    TEST_ASSERT(st_first.ino >= 0x80000000U);
    synth_before = st_first.ino;

    /* Rename parent directory and verify open handle / mapped inode stays stable */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_rename(ctx, "/many", "/many_renamed", 0));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_stat(ctx, "/many_renamed/file_0420.dat", &st_second));
    TEST_ASSERT_EQ_INT((long long)synth_before, (long long)st_second.ino);

    /* Unmount & remount twice with same upperdir: synthetic inode for renamed path must stay deterministic */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_stat(ctx, "/many_renamed/file_0420.dat", &st_first));
    TEST_ASSERT(st_first.ino >= 0x80000000U);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_stat(ctx, "/many_renamed/file_0420.dat", &st_second));
    TEST_ASSERT_EQ_INT((long long)st_first.ino, (long long)st_second.ino);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    cleanup_dir_recursive(upper_dir);
    DeleteFileA(img_path);
    return 0;
}

static int test_overlay_tools_status_diff_validate_export_reset(void) {
    char img_path[MAX_PATH];
    char upper_dir[MAX_PATH];
    char unmarked_dir[MAX_PATH];
    char tar_path[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    ext4_file_handle_t *h = NULL;
    uint32_t w = 0;

    test_make_temp_path(img_path, sizeof(img_path), "ov_tools.img");
    test_make_temp_path(upper_dir, sizeof(upper_dir), "ov_tools_upper");
    test_make_temp_path(unmarked_dir, sizeof(unmarked_dir), "ov_tools_unmarked");
    test_make_temp_path(tar_path, sizeof(tar_path), "ov_tools_export.tar");
    DeleteFileA(img_path);
    DeleteFileA(tar_path);
    cleanup_dir_recursive(upper_dir);
    cleanup_dir_recursive(unmarked_dir);

    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 8ULL * 1024ULL * 1024ULL, "OVTOOLS"));
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    cfg.case_insensitive = 1;
    cfg.create_uid = 1000U;
    cfg.create_gid = 1001U;
    cfg.has_create_gid = 1;
    cfg.create_fmode = 0755U;
    cfg.create_dmode = 0750U;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/mod.txt", 0, 0, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/mod.txt", 1, &h));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, h->ino, "before", 0, 6, 0, &w, NULL));
    ext4_vfs_close_handle(h);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkdir(ctx, "/home_user", 0, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/delme.txt", 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    cfg.case_insensitive = 1;
    cfg.overlay_upper_dir = upper_dir;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/mod.txt", 1, &h));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, h->ino, "after-update", 0, 12, 0, &w, NULL));
    ext4_vfs_close_handle(h);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/added.txt", 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/added.txt", 1, &h));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, h->ino, "new-data", 0, 8, 0, &w, NULL));
    ext4_vfs_close_handle(h);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkdir(ctx, "/home_user/sub_new", 0, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/home_user/sub_new/child.txt", 0, 0, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unlink(ctx, "/delme.txt"));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    /* Mount lower RO for inspection */
    cfg.overlay_upper_dir = NULL;
    cfg.read_only = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));

    {
        overlay_diff_report_t rep;
        overlay_export_options_t eopts;
        int saw_mod = 0, saw_root_add = 0, saw_sub_child = 0;
        memset(&eopts, 0, sizeof(eopts));
        eopts.inherit_owner = 1;
        eopts.default_fmode = 0640U;
        eopts.default_dmode = 0750U;

        TEST_ASSERT_EQ_INT(0, overlay_tools_scan_ex(upper_dir, ctx, NULL, &eopts, 1, &rep));
        TEST_ASSERT_EQ_INT(3, (long long)rep.summary.added_count);
        TEST_ASSERT_EQ_INT(2, (long long)rep.summary.modified_count); /* /mod.txt and /home_user dir */
        TEST_ASSERT_EQ_INT(1, (long long)rep.summary.deleted_count);
        TEST_ASSERT_EQ_INT(20, (long long)rep.summary.total_upper_bytes);
        for (size_t i = 0; i < rep.entry_count; i++) {
            const overlay_diff_entry_t *e = &rep.entries[i];
            if (strcmp(e->path, "/mod.txt") == 0) {
                saw_mod = 1;
                TEST_ASSERT_EQ_INT(OV_DIFF_MODIFIED, e->kind);
                TEST_ASSERT_EQ_INT(1000, (long long)e->uid);
                TEST_ASSERT_EQ_INT(1001, (long long)e->gid);
                TEST_ASSERT_EQ_INT(0755, (int)(e->mode & 07777));
            } else if (strcmp(e->path, "/added.txt") == 0) {
                saw_root_add = 1;
                TEST_ASSERT_EQ_INT(OV_DIFF_ADDED, e->kind);
                TEST_ASSERT_EQ_INT(0, (long long)e->uid);
                TEST_ASSERT_EQ_INT(0, (long long)e->gid);
                TEST_ASSERT_EQ_INT(0640, (int)(e->mode & 07777));
            } else if (strcmp(e->path, "/home_user/sub_new/child.txt") == 0) {
                saw_sub_child = 1;
                TEST_ASSERT_EQ_INT(OV_DIFF_ADDED, e->kind);
                /* Nearest existing lower ancestor is /home_user (1000:1001) */
                TEST_ASSERT_EQ_INT(1000, (long long)e->uid);
                TEST_ASSERT_EQ_INT(1001, (long long)e->gid);
                TEST_ASSERT_EQ_INT(0640, (int)(e->mode & 07777));
            }
        }
        TEST_ASSERT(saw_mod && saw_root_add && saw_sub_child);
        overlay_tools_free_diff_report(&rep);
    }

    /* Inject stale tmp + orphan whiteout and validate warnings */
    {
        char tmp_f[MAX_PATH], orp_f[MAX_PATH];
        FILE *fp;
        snprintf(tmp_f, sizeof(tmp_f), "%s\\foo.ext4tmp_1_1", upper_dir);
        snprintf(orp_f, sizeof(orp_f), "%s\\.wh.never_existed", upper_dir);
        fp = fopen(tmp_f, "wb");
        TEST_ASSERT(fp != NULL);
        fputs("tmp", fp);
        fclose(fp);
        fp = fopen(orp_f, "wb");
        TEST_ASSERT(fp != NULL);
        fclose(fp);

        overlay_validate_report_t vrep;
        TEST_ASSERT_EQ_INT(0, overlay_tools_validate(upper_dir, ctx, NULL, &vrep));
        TEST_ASSERT_EQ_INT(1, vrep.valid);
        TEST_ASSERT_EQ_INT(0, (long long)vrep.error_count);
        TEST_ASSERT(vrep.warning_count >= 2);
        overlay_tools_free_validate_report(&vrep);
    }

    /* Export tar and check USTAR headers (root uname/gname on manifest, preserved numeric 1000:1001 on files/mod.txt) */
    {
        overlay_export_summary_t esum;
        overlay_export_options_t eopts;
        uint8_t *hdr = (uint8_t *)calloc(1, 512);
        FILE *tfp;
        int found_mod_hdr = 0;
        memset(&eopts, 0, sizeof(eopts));
        eopts.inherit_owner = 1;
        TEST_ASSERT(hdr != NULL);
        TEST_ASSERT_EQ_INT(0, overlay_tools_export_tar_ex(upper_dir, ctx, NULL, &eopts, tar_path, &esum));
        TEST_ASSERT(esum.exported_entries >= 5);
        TEST_ASSERT(esum.archive_bytes >= 1536);

        tfp = fopen(tar_path, "rb");
        TEST_ASSERT(tfp != NULL);
        TEST_ASSERT_EQ_INT(512, (long long)fread(hdr, 1, 512, tfp));
        TEST_ASSERT(strcmp((const char *)hdr, "manifest.json") == 0);
        TEST_ASSERT(memcmp(hdr + 257, "ustar", 5) == 0);
        TEST_ASSERT(strcmp((const char *)(hdr + 265), "root") == 0);
        TEST_ASSERT(strcmp((const char *)(hdr + 297), "root") == 0);

        rewind(tfp);
        while (fread(hdr, 1, 512, tfp) == 512) {
            if (hdr[0] == '\0') break;
            unsigned long long fsz = strtoull((const char *)(hdr + 124), NULL, 8);
            if (strcmp((const char *)hdr, "files/mod.txt") == 0) {
                unsigned long t_mode = strtoul((const char *)(hdr + 100), NULL, 8);
                unsigned long t_uid = strtoul((const char *)(hdr + 108), NULL, 8);
                unsigned long t_gid = strtoul((const char *)(hdr + 116), NULL, 8);
                TEST_ASSERT_EQ_INT(0755, (int)t_mode);
                TEST_ASSERT_EQ_INT(1000, (int)t_uid);
                TEST_ASSERT_EQ_INT(1001, (int)t_gid);
                /* Non-root numeric uid/gid leaves uname/gname empty so tar extracts exact numeric IDs */
                TEST_ASSERT_EQ_INT(0, (int)hdr[265]);
                TEST_ASSERT_EQ_INT(0, (int)hdr[297]);
                found_mod_hdr = 1;
            }
            unsigned long long blocks = (fsz + 511ULL) / 512ULL;
            if (blocks > 0) fseek(tfp, (long)(blocks * 512ULL), SEEK_CUR);
        }
        TEST_ASSERT(found_mod_hdr);
        fclose(tfp);
        free(hdr);
    }

    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    /* Reset upperdir preserves .wh.__ext4_lower_id and refuses unmarked non-empty dir */
    {
        uint64_t removed = 0;
        char id_path[MAX_PATH];
        char added_path[MAX_PATH];
        char keep_file[MAX_PATH];
        FILE *fp;

        TEST_ASSERT_EQ_INT(0, overlay_tools_reset_upper(upper_dir, 1, &removed));
        TEST_ASSERT(removed >= 4);
        snprintf(id_path, sizeof(id_path), "%s\\.wh.__ext4_lower_id", upper_dir);
        snprintf(added_path, sizeof(added_path), "%s\\added.txt", upper_dir);
        TEST_ASSERT(GetFileAttributesA(id_path) != INVALID_FILE_ATTRIBUTES);
        TEST_ASSERT(GetFileAttributesA(added_path) == INVALID_FILE_ATTRIBUTES);

        CreateDirectoryA(unmarked_dir, NULL);
        snprintf(keep_file, sizeof(keep_file), "%s\\precious.txt", unmarked_dir);
        fp = fopen(keep_file, "wb");
        TEST_ASSERT(fp != NULL);
        fputs("do-not-delete", fp);
        fclose(fp);
        TEST_ASSERT_EQ_INT(-ENOTEMPTY, overlay_tools_reset_upper(unmarked_dir, 1, &removed));
        TEST_ASSERT(GetFileAttributesA(keep_file) != INVALID_FILE_ATTRIBUTES);
    }

    cleanup_dir_recursive(upper_dir);
    cleanup_dir_recursive(unmarked_dir);
    DeleteFileA(tar_path);
    DeleteFileA(img_path);
    return 0;
}

static int test_overlay_symlink_and_windows_namespace_guards(void) {
    char img_path[MAX_PATH];
    char upper_dir[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    ext4_file_handle_t *h = NULL;
    char target[256];

    TEST_ASSERT_EQ_INT(EXT4_WIN_NAME_OK, ext4_win_check_path("/normal/File.txt"));
    TEST_ASSERT_EQ_INT(EXT4_WIN_NAME_RESERVED_DEVICE, ext4_win_check_path("/dir/CON"));
    TEST_ASSERT_EQ_INT(EXT4_WIN_NAME_RESERVED_DEVICE, ext4_win_check_path("/nul.txt"));
    TEST_ASSERT_EQ_INT(EXT4_WIN_NAME_INVALID_CHAR, ext4_win_check_path("/bad:name"));
    TEST_ASSERT_EQ_INT(EXT4_WIN_NAME_TRAILING_DOT_SPACE, ext4_win_check_path("/trail."));

    test_make_temp_path(img_path, sizeof(img_path), "ov_sym_ns.img");
    test_make_temp_path(upper_dir, sizeof(upper_dir), "ov_sym_ns_upper");
    DeleteFileA(img_path);
    cleanup_dir_recursive(upper_dir);

    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 8ULL * 1024ULL * 1024ULL, "OVSYMNS"));
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    cfg.case_insensitive = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/hello.txt", 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_symlink(ctx, "/sym", "/hello.txt"));
    /* Lower image holds reserved Windows name safely in read-only layer */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/CON", 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    cfg.overlay_upper_dir = upper_dir;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));

    TEST_ASSERT_EQ_INT(0, ext4_vfs_readlink(ctx, "/sym", target, sizeof(target)));
    TEST_ASSERT(strcmp(target, "/hello.txt") == 0);
    TEST_ASSERT_EQ_INT(-ENOTSUP, ext4_vfs_symlink(ctx, "/new_sym", "/hello.txt"));
    TEST_ASSERT_EQ_INT(-ENOTSUP, ext4_vfs_open(ctx, "/sym", 1, &h));
    TEST_ASSERT_EQ_INT(-ENOTSUP, ext4_vfs_rename(ctx, "/sym", "/sym_moved", 0));

    /* Reserved / invalid Windows names cannot be created or copied up in overlay */
    TEST_ASSERT_EQ_INT(-EINVAL, ext4_vfs_create(ctx, "/AUX", 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(-EINVAL, ext4_vfs_create(ctx, "/com1.log", 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(-EINVAL, ext4_vfs_create(ctx, "/bad:file", 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(-EINVAL, ext4_vfs_open(ctx, "/CON", 1, &h));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/CON", 0, &h));
    ext4_vfs_close_handle(h);

    /* Unlinking a lower symlink creates a whiteout cleanly */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unlink(ctx, "/sym"));
    TEST_ASSERT_EQ_INT(-ENOENT, ext4_vfs_readlink(ctx, "/sym", target, sizeof(target)));

    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    cleanup_dir_recursive(upper_dir);
    DeleteFileA(img_path);
    return 0;
}

typedef struct ov_case_readdir_check {
    const char *want_exact;
    const char *forbid_exact;
    int         want_count;
    int         forbid_count;
} ov_case_readdir_check_t;

static int ov_case_readdir_cb(const char *name, const ext4_stat_t *st, void *user_ctx) {
    ov_case_readdir_check_t *chk = (ov_case_readdir_check_t *)user_ctx;
    (void)st;
    if (chk->want_exact && strcmp(name, chk->want_exact) == 0) chk->want_count++;
    if (chk->forbid_exact && strcmp(name, chk->forbid_exact) == 0) chk->forbid_count++;
    return 0;
}

static int test_overlay_case_only_rename(void) {
    char img_path[MAX_PATH];
    char upper_dir[MAX_PATH];
    char wh_path[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    ext4_file_handle_t *h = NULL;
    ext4_stat_t st;
    char actual_leaf[EXT2_NAME_LEN + 1];
    char buf[64];
    uint32_t nw = 0, nr = 0;
    ov_case_readdir_check_t rchk;

    test_make_temp_path(img_path, sizeof(img_path), "ov_case_ren.img");
    test_make_temp_path(upper_dir, sizeof(upper_dir), "ov_case_ren_upper");
    DeleteFileA(img_path);
    cleanup_dir_recursive(upper_dir);

    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 8ULL * 1024ULL * 1024ULL, "OVCASEREN"));
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    cfg.case_insensitive = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/hello.txt", 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/hello.txt", 1, &h));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, h->ino, "lower-hello", 0, 11, 0, &nw, NULL));
    ext4_vfs_close_handle(h);
    h = NULL;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkdir(ctx, "/lower_dir", 0755, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/lower_dir/inside.txt", 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/lower_dir/inside.txt", 1, &h));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, h->ino, "lower-inside", 0, 12, 0, &nw, NULL));
    ext4_vfs_close_handle(h);
    h = NULL;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    cfg.overlay_upper_dir = upper_dir;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));

    /* 1. Upper-only file: a.txt -> a.TXT (replace=0), simulating WinFsp uppercased FileName too */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/a.txt", 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/a.txt", 1, &h));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, h->ino, "hello-case", 0, 10, 0, &nw, NULL));
    ext4_vfs_close_handle(h);
    h = NULL;

    TEST_ASSERT_EQ_INT(0, ext4_vfs_rename(ctx, "/a.txt", "/a.TXT", 0));
    TEST_ASSERT_EQ_INT(0, overlay_vfs_stat_with_name(ctx, "/a.txt", &st, actual_leaf, sizeof(actual_leaf)));
    TEST_ASSERT(strcmp(actual_leaf, "a.TXT") == 0);
    memset(&rchk, 0, sizeof(rchk));
    rchk.want_exact = "a.TXT";
    rchk.forbid_exact = "a.txt";
    TEST_ASSERT_EQ_INT(0, ext4_vfs_readdir(ctx, EXT2_ROOT_INO, ov_case_readdir_cb, &rchk));
    TEST_ASSERT_EQ_INT(1, rchk.want_count);
    TEST_ASSERT_EQ_INT(0, rchk.forbid_count);

    /* 2. Upper-only file: a.TXT -> A.txt (replace=1) must NOT self-delete */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_rename(ctx, "/A.TXT", "/A.txt", 1));
    TEST_ASSERT_EQ_INT(0, overlay_vfs_stat_with_name(ctx, "/a.txt", &st, actual_leaf, sizeof(actual_leaf)));
    TEST_ASSERT(strcmp(actual_leaf, "A.txt") == 0);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/A.txt", 0, &h));
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_read_handle(ctx, h, buf, 0, 10, &nr));
    TEST_ASSERT_EQ_INT(10, (long long)nr);
    TEST_ASSERT(memcmp(buf, "hello-case", 10) == 0);
    ext4_vfs_close_handle(h);
    h = NULL;

    /* 3. Genuine no-op when actual on-disk casing already matches */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_rename(ctx, "/a.txt", "/A.txt", 0));
    TEST_ASSERT_EQ_INT(0, overlay_vfs_stat_with_name(ctx, "/a.txt", &st, actual_leaf, sizeof(actual_leaf)));
    TEST_ASSERT(strcmp(actual_leaf, "A.txt") == 0);

    /* 4. Lower-only file case-only rename (replace=0 & replace=1) must NOT whiteout itself */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_rename(ctx, "/hello.txt", "/hello.TXT", 0));
    snprintf(wh_path, sizeof(wh_path), "%s\\.wh.hello.txt", upper_dir);
    TEST_ASSERT(GetFileAttributesA(wh_path) == INVALID_FILE_ATTRIBUTES);
    TEST_ASSERT_EQ_INT(0, overlay_vfs_stat_with_name(ctx, "/hello.txt", &st, actual_leaf, sizeof(actual_leaf)));
    TEST_ASSERT(strcmp(actual_leaf, "hello.TXT") == 0);
    memset(&rchk, 0, sizeof(rchk));
    rchk.want_exact = "hello.TXT";
    rchk.forbid_exact = "hello.txt";
    TEST_ASSERT_EQ_INT(0, ext4_vfs_readdir(ctx, EXT2_ROOT_INO, ov_case_readdir_cb, &rchk));
    TEST_ASSERT_EQ_INT(1, rchk.want_count);
    TEST_ASSERT_EQ_INT(0, rchk.forbid_count);

    TEST_ASSERT_EQ_INT(0, ext4_vfs_rename(ctx, "/hello.TXT", "/Hello.Txt", 1));
    TEST_ASSERT_EQ_INT(0, overlay_vfs_stat_with_name(ctx, "/hello.txt", &st, actual_leaf, sizeof(actual_leaf)));
    TEST_ASSERT(strcmp(actual_leaf, "Hello.Txt") == 0);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/Hello.Txt", 0, &h));
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_read_handle(ctx, h, buf, 0, 11, &nr));
    TEST_ASSERT_EQ_INT(11, (long long)nr);
    TEST_ASSERT(memcmp(buf, "lower-hello", 11) == 0);
    ext4_vfs_close_handle(h);
    h = NULL;

    TEST_ASSERT_EQ_INT(0, ext4_vfs_unlink(ctx, "/Hello.Txt"));
    TEST_ASSERT_EQ_INT(-ENOENT, ext4_vfs_stat(ctx, "/hello.txt", &st));

    /* 5. Upper-only directory case-only rename (replace=0 & replace=1) */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkdir(ctx, "/sub_case", 0755, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/sub_case/item.txt", 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/sub_case/item.txt", 1, &h));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_write(ctx, h->ino, "sub-item", 0, 8, 0, &nw, NULL));
    ext4_vfs_close_handle(h);
    h = NULL;

    TEST_ASSERT_EQ_INT(0, ext4_vfs_rename(ctx, "/sub_case", "/SUB_CASE", 0));
    TEST_ASSERT_EQ_INT(0, overlay_vfs_stat_with_name(ctx, "/sub_case", &st, actual_leaf, sizeof(actual_leaf)));
    TEST_ASSERT(strcmp(actual_leaf, "SUB_CASE") == 0);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_rename(ctx, "/SUB_CASE", "/Sub_Case", 1));
    TEST_ASSERT_EQ_INT(0, overlay_vfs_stat_with_name(ctx, "/sub_case", &st, actual_leaf, sizeof(actual_leaf)));
    TEST_ASSERT(strcmp(actual_leaf, "Sub_Case") == 0);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/Sub_Case/item.txt", 0, &h));
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_read_handle(ctx, h, buf, 0, 8, &nr));
    TEST_ASSERT_EQ_INT(8, (long long)nr);
    TEST_ASSERT(memcmp(buf, "sub-item", 8) == 0);
    ext4_vfs_close_handle(h);
    h = NULL;

    /* 6. Lower directory case-only rename preserves children without self-whiteout */
    TEST_ASSERT_EQ_INT(0, ext4_vfs_rename(ctx, "/lower_dir", "/LOWER_DIR", 0));
    TEST_ASSERT_EQ_INT(0, overlay_vfs_stat_with_name(ctx, "/lower_dir", &st, actual_leaf, sizeof(actual_leaf)));
    TEST_ASSERT(strcmp(actual_leaf, "LOWER_DIR") == 0);
    memset(&rchk, 0, sizeof(rchk));
    rchk.want_exact = "LOWER_DIR";
    rchk.forbid_exact = "lower_dir";
    TEST_ASSERT_EQ_INT(0, ext4_vfs_readdir(ctx, EXT2_ROOT_INO, ov_case_readdir_cb, &rchk));
    TEST_ASSERT_EQ_INT(1, rchk.want_count);
    TEST_ASSERT_EQ_INT(0, rchk.forbid_count);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_open(ctx, "/LOWER_DIR/inside.txt", 0, &h));
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_read_handle(ctx, h, buf, 0, 12, &nr));
    TEST_ASSERT_EQ_INT(12, (long long)nr);
    TEST_ASSERT(memcmp(buf, "lower-inside", 12) == 0);
    ext4_vfs_close_handle(h);
    h = NULL;

    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;
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
    RUN_TEST(test_overlay_upperdir_guards_v2);
    RUN_TEST(test_overlay_hashmap_scalability_and_deterministic_inodes);
    RUN_TEST(test_overlay_tools_status_diff_validate_export_reset);
    RUN_TEST(test_overlay_symlink_and_windows_namespace_guards);
    RUN_TEST(test_overlay_case_only_rename);
    return suite_failures;
}
