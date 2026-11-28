#include "test_common.h"
#include "../src/driver/winfsp_driver.h"
#include "../src/vfs/ext4_vfs.h"

#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE     0x00000001
#endif
#ifndef FILE_NON_DIRECTORY_FILE
#define FILE_NON_DIRECTORY_FILE 0x00000040
#endif
#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000
#endif

static int test_winfsp_interface_e2e(void) {
    char img_path[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    FSP_FILE_SYSTEM *fs = NULL;
    const FSP_FILE_SYSTEM_INTERFACE *iface = NULL;
    FSP_FSCTL_VOLUME_INFO vol_info;
    FSP_FSCTL_FILE_INFO fi;
    PVOID dir_ctx = NULL, file_ctx = NULL, dir_ctx2 = NULL;
    ULONG transferred = 0;
    UINT32 attrs = 0;
    UINT8 sd_buf[512];
    SIZE_T sd_sz = sizeof(sd_buf);
    char write_data[] = "WinFsp Callback E2E Payload!";
    char read_buf[128];

    test_make_temp_path(img_path, sizeof(img_path), "winfsp_cb.img");
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 8ULL * 1024ULL * 1024ULL, "WINFSPVOL"));

    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.case_insensitive = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, winfsp_driver_create_test_fs(ctx, &fs));
    iface = winfsp_driver_get_interface();
    TEST_ASSERT(iface != NULL);

    /* VolumeParams check */
    {
        FSP_FSCTL_VOLUME_PARAMS vp;
        winfsp_driver_build_volume_params(ctx, &vp);
        TEST_ASSERT_EQ_INT(4096, vp.SectorSize);
        TEST_ASSERT_EQ_INT(0, vp.ReadOnlyVolume);
        TEST_ASSERT_EQ_INT(1000, vp.FileInfoTimeout);
        TEST_ASSERT_EQ_INT(1, vp.PassQueryDirectoryFileName);
        TEST_ASSERT_EQ_INT(1, vp.UmFileContextIsUserContext2);
        TEST_ASSERT(wcscmp(vp.FileSystemName, L"ext4") == 0);
    }

    /* GetVolumeInfo & SetVolumeLabel */
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->GetVolumeInfo(fs, &vol_info));
    TEST_ASSERT(vol_info.TotalSize >= 8ULL * 1024ULL * 1024ULL);
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->SetVolumeLabel(fs, L"NEW_VOL", &vol_info));
    TEST_ASSERT(wcsncmp(vol_info.VolumeLabel, L"NEW_VOL", 7) == 0);

    /* Create directory & file via Windows backslash paths */
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Create(fs, L"\\docs", FILE_DIRECTORY_FILE, FILE_GENERIC_WRITE,
                                     FILE_ATTRIBUTE_DIRECTORY, NULL, 0, &dir_ctx, &fi));
    TEST_ASSERT((fi.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    iface->Cleanup(fs, dir_ctx, L"\\docs", 0);
    iface->Close(fs, dir_ctx);

    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Create(fs, L"\\docs\\Notes.txt", FILE_NON_DIRECTORY_FILE,
                                     FILE_WRITE_DATA | FILE_READ_DATA, FILE_ATTRIBUTE_ARCHIVE,
                                     NULL, 0, &file_ctx, &fi));
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Write(fs, file_ctx, write_data, 0, (ULONG)strlen(write_data),
                                    FALSE, FALSE, &transferred, &fi));
    TEST_ASSERT_EQ_INT((long long)strlen(write_data), transferred);
    TEST_ASSERT_EQ_INT((long long)strlen(write_data), fi.FileSize);

    /* SetBasicInfo & Flush */
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->SetBasicInfo(fs, file_ctx, FILE_ATTRIBUTE_ARCHIVE,
                                           fi.CreationTime, fi.LastAccessTime, fi.LastWriteTime, fi.ChangeTime, &fi));
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->Flush(fs, file_ctx, &fi));
    iface->Cleanup(fs, file_ctx, L"\\docs\\Notes.txt", 0);
    iface->Close(fs, file_ctx);

    /* GetSecurityByName (case-insensitive) & Open & Read & EOF */
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->GetSecurityByName(fs, L"\\DOCS\\notes.txt", &attrs, sd_buf, &sd_sz));
    TEST_ASSERT((attrs & FILE_ATTRIBUTE_ARCHIVE) != 0);
    TEST_ASSERT(sd_sz > 0);

    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Open(fs, L"\\docs\\Notes.txt", FILE_NON_DIRECTORY_FILE,
                                   FILE_READ_DATA, &file_ctx, &fi));
    memset(read_buf, 0, sizeof(read_buf));
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Read(fs, file_ctx, read_buf, 0, sizeof(read_buf), &transferred));
    TEST_ASSERT_EQ_INT((long long)strlen(write_data), transferred);
    TEST_ASSERT(strcmp(read_buf, write_data) == 0);
    /* Read at EOF -> STATUS_END_OF_FILE */
    TEST_ASSERT_EQ_INT(STATUS_END_OF_FILE,
                       iface->Read(fs, file_ctx, read_buf, fi.FileSize, 64, &transferred));
    iface->Close(fs, file_ctx);

    /* SetFileSize & Overwrite */
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Open(fs, L"\\docs\\Notes.txt", FILE_NON_DIRECTORY_FILE,
                                   FILE_WRITE_DATA, &file_ctx, &fi));
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->SetFileSize(fs, file_ctx, 6, FALSE, &fi));
    TEST_ASSERT_EQ_INT(6, fi.FileSize);
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->Overwrite(fs, file_ctx, FILE_ATTRIBUTE_ARCHIVE, TRUE, 0, &fi));
    TEST_ASSERT_EQ_INT(0, fi.FileSize);
    iface->Close(fs, file_ctx);

    /* ReadDirectory with small buffer + Marker pagination */
    {
        /* Create 12 files in \docs so a tiny 512-byte buffer requires multiple ReadDirectory calls */
        for (int i = 0; i < 12; i++) {
            wchar_t fpath[64];
            PVOID fctx = NULL;
            swprintf(fpath, 64, L"\\docs\\item_%02d.dat", i);
            TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                               iface->Create(fs, fpath, FILE_NON_DIRECTORY_FILE, FILE_WRITE_DATA,
                                             FILE_ATTRIBUTE_ARCHIVE, NULL, 0, &fctx, &fi));
            iface->Close(fs, fctx);
        }

        TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                           iface->Open(fs, L"\\docs", FILE_DIRECTORY_FILE, FILE_LIST_DIRECTORY, &dir_ctx2, &fi));

        uint8_t page[512];
        wchar_t marker[256];
        wchar_t *marker_ptr = NULL;
        int total_entries = 0;
        int found_notes = 0;
        for (int round = 0; round < 32; round++) {
            ULONG bytes = 0;
            TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                               iface->ReadDirectory(fs, dir_ctx2, NULL, marker_ptr, page, sizeof(page), &bytes));
            if (bytes == 0) break;

            ULONG off = 0;
            wchar_t last_name[256] = {0};
            while (off + sizeof(UINT16) <= bytes) {
                FSP_FSCTL_DIR_INFO *di = (FSP_FSCTL_DIR_INFO *)(page + off);
                if (di->Size == 0) break;
                size_t nchars = (di->Size - sizeof(FSP_FSCTL_DIR_INFO)) / sizeof(WCHAR);
                memcpy(last_name, di->FileNameBuf, nchars * sizeof(WCHAR));
                last_name[nchars] = L'\0';
                if (wcscmp(last_name, L"Notes.txt") == 0) found_notes = 1;
                total_entries++;
                off += FSP_FSCTL_DEFAULT_ALIGN_UP(di->Size, 8);
            }
            wcscpy_s(marker, _countof(marker), last_name);
            marker_ptr = marker;
        }
        TEST_ASSERT_EQ_INT(1, found_notes);
        TEST_ASSERT(total_entries >= 15); /* ".", "..", "Notes.txt", and 12 "item_XX.dat" */
        iface->Close(fs, dir_ctx2);
    }

    /* CanDelete on non-empty directory must return STATUS_DIRECTORY_NOT_EMPTY */
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Open(fs, L"\\docs", FILE_DIRECTORY_FILE, DELETE, &dir_ctx, &fi));
    TEST_ASSERT_EQ_INT(STATUS_DIRECTORY_NOT_EMPTY, iface->CanDelete(fs, dir_ctx, L"\\docs"));
    iface->Close(fs, dir_ctx);

    /* Rename + Delete-on-Close via Cleanup */
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Open(fs, L"\\docs\\Notes.txt", FILE_NON_DIRECTORY_FILE, DELETE, &file_ctx, &fi));
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Rename(fs, file_ctx, L"\\docs\\Notes.txt", L"\\docs\\Renamed.txt", FALSE));
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->CanDelete(fs, file_ctx, L"\\docs\\Renamed.txt"));
    iface->Cleanup(fs, file_ctx, L"\\docs\\Renamed.txt", FspCleanupDelete);
    iface->Close(fs, file_ctx);
    TEST_ASSERT_EQ_INT(STATUS_OBJECT_NAME_NOT_FOUND,
                       iface->GetSecurityByName(fs, L"\\docs\\Renamed.txt", &attrs, NULL, NULL));

    /* Replacing an existing directory via Rename(ReplaceIfExists=TRUE) must fail with STATUS_ACCESS_DENIED */
    {
        PVOID d1_ctx = NULL, d2_ctx = NULL;
        TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                           iface->Create(fs, L"\\ren_d1", FILE_DIRECTORY_FILE, DELETE | FILE_GENERIC_WRITE,
                                         FILE_ATTRIBUTE_DIRECTORY, NULL, 0, &d1_ctx, &fi));
        TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                           iface->Create(fs, L"\\ren_d2", FILE_DIRECTORY_FILE, DELETE | FILE_GENERIC_WRITE,
                                         FILE_ATTRIBUTE_DIRECTORY, NULL, 0, &d2_ctx, &fi));
        TEST_ASSERT_EQ_INT(STATUS_ACCESS_DENIED,
                           iface->Rename(fs, d2_ctx, L"\\ren_d2", L"\\ren_d1", TRUE));
        iface->Cleanup(fs, d1_ctx, L"\\ren_d1", FspCleanupDelete);
        iface->Close(fs, d1_ctx);
        iface->Cleanup(fs, d2_ctx, L"\\ren_d2", FspCleanupDelete);
        iface->Close(fs, d2_ctx);
    }

    winfsp_driver_destroy_test_fs(fs);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    DeleteFileA(img_path);
    return 0;
}

static void remove_dir_tree_a(const char *dir) {
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char child[MAX_PATH];
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            snprintf(child, sizeof(child), "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                remove_dir_tree_a(child);
            } else {
                SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(child);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

static int test_winfsp_volume_params_ro_cache_timeout(void) {
    char img_path[MAX_PATH];
    char upper_dir[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    FSP_FSCTL_VOLUME_PARAMS vp;

    test_make_temp_path(img_path, sizeof(img_path), "winfsp_timeout.img");
    test_make_temp_path(upper_dir, sizeof(upper_dir), "winfsp_timeout_upper");
    remove_dir_tree_a(upper_dir);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 8ULL * 1024ULL * 1024ULL, "ROTIMEOUT"));

    /* 1. Default Read-Only mount enables INFINITE ((UINT32)-1) FileInfoTimeout for NT CcCopyRead */
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 1;
    cfg.case_insensitive = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    winfsp_driver_build_volume_params(ctx, &vp);
    TEST_ASSERT_EQ_INT(1, vp.ReadOnlyVolume);
    TEST_ASSERT(vp.FileInfoTimeout == (UINT32)-1);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    /* 2. Explicit override forces requested timeout even in Read-Only mode */
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 1;
    cfg.case_insensitive = 1;
    cfg.has_file_info_timeout = 1;
    cfg.file_info_timeout = 1000U;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    winfsp_driver_build_volume_params(ctx, &vp);
    TEST_ASSERT_EQ_INT(1, vp.ReadOnlyVolume);
    TEST_ASSERT_EQ_INT(1000, vp.FileInfoTimeout);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    /* 3. Writable Overlay mount keeps 1000 ms timeout for upperdir coherence */
    TEST_ASSERT(CreateDirectoryA(upper_dir, NULL));
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    cfg.overlay_upper_dir = upper_dir;
    cfg.case_insensitive = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    winfsp_driver_build_volume_params(ctx, &vp);
    TEST_ASSERT_EQ_INT(0, vp.ReadOnlyVolume);
    TEST_ASSERT_EQ_INT(1000, vp.FileInfoTimeout);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    remove_dir_tree_a(upper_dir);
    DeleteFileA(img_path);
    return 0;
}

static int test_winfsp_get_dir_info_by_name_and_large_readdir(void) {
    char img_path[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    FSP_FILE_SYSTEM *fs = NULL;
    const FSP_FILE_SYSTEM_INTERFACE *iface = NULL;
    FSP_FSCTL_FILE_INFO fi;
    PVOID dir_ctx = NULL, file_ctx = NULL;
    UINT8 di_buf[sizeof(FSP_FSCTL_DIR_INFO) + (EXT2_NAME_LEN + 1) * sizeof(WCHAR)];
    FSP_FSCTL_DIR_INFO *di = (FSP_FSCTL_DIR_INFO *)di_buf;
    ULONG transferred = 0;
    uint8_t payload[1234];

    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i & 0xFF);

    test_make_temp_path(img_path, sizeof(img_path), "winfsp_dirinfo.img");
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 16ULL * 1024ULL * 1024ULL, "DIRINFO"));

    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.case_insensitive = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, winfsp_driver_create_test_fs(ctx, &fs));
    iface = winfsp_driver_get_interface();
    TEST_ASSERT(iface != NULL);
    TEST_ASSERT(iface->GetDirInfoByName != NULL);

    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Create(fs, L"\\GdbDir", FILE_DIRECTORY_FILE, FILE_GENERIC_WRITE,
                                     FILE_ATTRIBUTE_DIRECTORY, NULL, 0, &dir_ctx, &fi));
    iface->Close(fs, dir_ctx);

    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Create(fs, L"\\GdbDir\\MixedCase.bin", FILE_NON_DIRECTORY_FILE,
                                     FILE_WRITE_DATA, FILE_ATTRIBUTE_ARCHIVE, NULL, 0, &file_ctx, &fi));
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Write(fs, file_ctx, payload, 0, sizeof(payload), FALSE, FALSE, &transferred, &fi));
    TEST_ASSERT_EQ_INT((long long)sizeof(payload), fi.FileSize);

    /* Non-directory handle passed to GetDirInfoByName must return STATUS_NOT_A_DIRECTORY */
    TEST_ASSERT_EQ_INT(STATUS_NOT_A_DIRECTORY,
                       iface->GetDirInfoByName(fs, file_ctx, L"child.txt", di));
    iface->Close(fs, file_ctx);

    /* Populate 120 extra entries for paginated ReadDirectory verification */
    for (int i = 0; i < 120; i++) {
        wchar_t fpath[64];
        PVOID fctx = NULL;
        swprintf(fpath, 64, L"\\GdbDir\\entry_%03d.txt", i);
        TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                           iface->Create(fs, fpath, FILE_NON_DIRECTORY_FILE, FILE_WRITE_DATA,
                                         FILE_ATTRIBUTE_ARCHIVE, NULL, 0, &fctx, &fi));
        iface->Close(fs, fctx);
    }

    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Open(fs, L"\\GdbDir", FILE_DIRECTORY_FILE, FILE_LIST_DIRECTORY, &dir_ctx, &fi));

    /* Exact & case-insensitive GetDirInfoByName */
    memset(di_buf, 0, sizeof(di_buf));
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->GetDirInfoByName(fs, dir_ctx, L"MIXEDCASE.BIN", di));
    TEST_ASSERT_EQ_INT((long long)sizeof(payload), di->FileInfo.FileSize);
    TEST_ASSERT_EQ_INT((long long)(sizeof(FSP_FSCTL_DIR_INFO) + wcslen(L"MixedCase.bin") * sizeof(WCHAR)), di->Size);
    TEST_ASSERT(wcsncmp(di->FileNameBuf, L"MixedCase.bin", wcslen(L"MixedCase.bin")) == 0);

    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->GetDirInfoByName(fs, dir_ctx, L".", di));
    TEST_ASSERT_EQ_INT(STATUS_OBJECT_NAME_NOT_FOUND,
                       iface->GetDirInfoByName(fs, dir_ctx, L"missing_probe.bin", di));
    TEST_ASSERT_EQ_INT(STATUS_OBJECT_NAME_INVALID,
                       iface->GetDirInfoByName(fs, dir_ctx, L"sub\\nested", di));

    /* Small-buffer ReadDirectory pagination with early Marker skip across 123 entries */
    {
        uint8_t page[512];
        wchar_t marker[256];
        wchar_t *marker_ptr = NULL;
        int total_entries = 0;
        int found_mixed = 0;
        int found_last = 0;

        for (int round = 0; round < 64; round++) {
            ULONG bytes = 0;
            TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                               iface->ReadDirectory(fs, dir_ctx, NULL, marker_ptr, page, sizeof(page), &bytes));
            if (bytes == 0) break;

            ULONG off = 0;
            wchar_t last_name[256] = {0};
            while (off + sizeof(UINT16) <= bytes) {
                FSP_FSCTL_DIR_INFO *item = (FSP_FSCTL_DIR_INFO *)(page + off);
                if (item->Size == 0) break;
                size_t nchars = (item->Size - sizeof(FSP_FSCTL_DIR_INFO)) / sizeof(WCHAR);
                memcpy(last_name, item->FileNameBuf, nchars * sizeof(WCHAR));
                last_name[nchars] = L'\0';
                if (wcscmp(last_name, L"MixedCase.bin") == 0) found_mixed = 1;
                if (wcscmp(last_name, L"entry_119.txt") == 0) found_last = 1;
                total_entries++;
                off += FSP_FSCTL_DEFAULT_ALIGN_UP(item->Size, 8);
            }
            wcscpy_s(marker, _countof(marker), last_name);
            marker_ptr = marker;
        }
        TEST_ASSERT_EQ_INT(1, found_mixed);
        TEST_ASSERT_EQ_INT(1, found_last);
        TEST_ASSERT_EQ_INT(123, total_entries); /* '.', '..', 'MixedCase.bin', 120 'entry_XXX.txt' */
    }

    iface->Close(fs, dir_ctx);
    winfsp_driver_destroy_test_fs(fs);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    DeleteFileA(img_path);
    return 0;
}

static int test_winfsp_safe_unmount_target(void) {
    /* Non-WinFsp / physical / subst / network targets must always be rejected */
    TEST_ASSERT_EQ_INT(0, winfsp_driver_is_safe_unmount_target(NULL, 0, NULL));
    TEST_ASSERT_EQ_INT(0, winfsp_driver_is_safe_unmount_target(L"", 0, NULL));
    TEST_ASSERT_EQ_INT(0, winfsp_driver_is_safe_unmount_target(L"\\Device\\HarddiskVolume2", 1, L"NTFS"));
    TEST_ASSERT_EQ_INT(0, winfsp_driver_is_safe_unmount_target(L"\\Device\\HarddiskVolume2", 0, NULL));
    TEST_ASSERT_EQ_INT(0, winfsp_driver_is_safe_unmount_target(L"\\Device\\CdRom0", 0, NULL));
    TEST_ASSERT_EQ_INT(0, winfsp_driver_is_safe_unmount_target(L"\\??\\C:\\mounted_dir", 1, L"NTFS"));
    TEST_ASSERT_EQ_INT(0, winfsp_driver_is_safe_unmount_target(L"\\Device\\LanmanRedirector\\;Z:0\\srv\\share", 1, L"NTFS"));

    /* Live WinFsp/Volume target with another filesystem must be rejected */
    TEST_ASSERT_EQ_INT(0, winfsp_driver_is_safe_unmount_target(L"\\Device\\Volume{11111111-2222-3333-4444-555555555555}", 1, L"NTFS"));
    TEST_ASSERT_EQ_INT(0, winfsp_driver_is_safe_unmount_target(L"\\Device\\Volume{11111111-2222-3333-4444-555555555555}", 1, L"exFAT"));
    TEST_ASSERT_EQ_INT(0, winfsp_driver_is_safe_unmount_target(L"\\Device\\WinFsp.Disk\\rclone", 1, L"FUSE"));
    TEST_ASSERT_EQ_INT(0, winfsp_driver_is_safe_unmount_target(L"\\Device\\Volume{11111111-2222-3333-4444-555555555555}", 1, NULL));

    /* Live ext4 WinFsp volume or dead WinFsp/Volume link is safe */
    TEST_ASSERT_EQ_INT(1, winfsp_driver_is_safe_unmount_target(L"\\Device\\Volume{11111111-2222-3333-4444-555555555555}", 1, L"ext4"));
    TEST_ASSERT_EQ_INT(1, winfsp_driver_is_safe_unmount_target(L"\\Device\\Volume{11111111-2222-3333-4444-555555555555}", 0, NULL));
    TEST_ASSERT_EQ_INT(1, winfsp_driver_is_safe_unmount_target(L"\\Device\\WinFsp.Disk\\ext4", 0, NULL));

    /* Live system drive C: must be refused with -EACCES without touching it */
    TEST_ASSERT_EQ_INT(-EACCES, winfsp_driver_force_unmount(L"C:"));
    return 0;
}

static void fill_deterministic_pattern(uint8_t *buf, size_t len, uint32_t seed) {
    uint32_t state = seed ? seed : 1U;
    for (size_t i = 0; i < len; i++) {
        state = state * 1664525U + 1013904223U;
        buf[i] = (uint8_t)((state >> 16) & 0xFFU);
    }
}

typedef struct rw_fast_io_e2e_res {
    char img_path[MAX_PATH];
    ext4_vfs_ctx_t *ctx;
    FSP_FILE_SYSTEM *fs;
    PVOID dir_ctx;
    PVOID file_ctx;
    uint8_t *shadow;
    uint8_t *io_buf;
    uint8_t *aux_buf;
} rw_fast_io_e2e_res_t;

static int test_winfsp_rw_fast_io_e2e_impl(rw_fast_io_e2e_res_t *r) {
    enum {
        SEQ_INIT_LEN   = 131072,
        PATCH_OFF      = 4093,
        PATCH_LEN      = 5000,
        APPEND_LEN     = 3333,
        STREAM_LEN     = 134405,
        CONSTR_TAIL    = 100,
        CONSTR_REQ_LEN = 500
    };
    const FSP_FILE_SYSTEM_INTERFACE *iface = NULL;
    ext4_mount_config_t cfg;
    FSP_FSCTL_VOLUME_PARAMS vp;
    FSP_FSCTL_FILE_INFO fi;
    UINT8 di_buf[sizeof(FSP_FSCTL_DIR_INFO) + (EXT2_NAME_LEN + 1) * sizeof(WCHAR)];
    FSP_FSCTL_DIR_INFO *di = (FSP_FSCTL_DIR_INFO *)di_buf;
    ULONG transferred = 0;
    UINT32 attrs = 0;

    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(r->img_path, 16ULL * 1024ULL * 1024ULL, "WINFSPFAST"));

    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = r->img_path;
    cfg.read_only = 0;
    cfg.enable_rw_fast_io = 1;
    cfg.case_insensitive = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &r->ctx));
    TEST_ASSERT_EQ_INT(1, r->ctx->enable_rw_fast_io);
    TEST_ASSERT_EQ_INT(0, r->ctx->enable_direct_io);

    TEST_ASSERT_EQ_INT(0, winfsp_driver_create_test_fs(r->ctx, &r->fs));
    iface = winfsp_driver_get_interface();
    TEST_ASSERT(iface != NULL);

    winfsp_driver_build_volume_params(r->ctx, &vp);
    TEST_ASSERT_EQ_INT(0, vp.ReadOnlyVolume);
    TEST_ASSERT_EQ_INT(1000, vp.FileInfoTimeout);
    TEST_ASSERT_EQ_INT(1, vp.UmFileContextIsUserContext2);

    /* Create \fast directory & \fast\stream.bin */
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Create(r->fs, L"\\fast", FILE_DIRECTORY_FILE, FILE_GENERIC_WRITE,
                                     FILE_ATTRIBUTE_DIRECTORY, NULL, 0, &r->dir_ctx, &fi));
    iface->Cleanup(r->fs, r->dir_ctx, L"\\fast", 0);
    iface->Close(r->fs, r->dir_ctx);
    r->dir_ctx = NULL;

    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Create(r->fs, L"\\fast\\stream.bin", FILE_NON_DIRECTORY_FILE,
                                     FILE_WRITE_DATA | FILE_READ_DATA, FILE_ATTRIBUTE_ARCHIVE,
                                     NULL, 0, &r->file_ctx, &fi));

    /* Sequential 128 KB write */
    fill_deterministic_pattern(r->shadow, SEQ_INIT_LEN, 0x11111111U);
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Write(r->fs, r->file_ctx, r->shadow, 0, SEQ_INIT_LEN,
                                    FALSE, FALSE, &transferred, &fi));
    TEST_ASSERT_EQ_INT(SEQ_INIT_LEN, transferred);
    TEST_ASSERT_EQ_INT(SEQ_INIT_LEN, fi.FileSize);

    /* Unaligned partial overwrite at offset 4093, length 5000 */
    fill_deterministic_pattern(r->io_buf, PATCH_LEN, 0x22222222U);
    memcpy(r->shadow + PATCH_OFF, r->io_buf, PATCH_LEN);
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Write(r->fs, r->file_ctx, r->io_buf, PATCH_OFF, PATCH_LEN,
                                    FALSE, FALSE, &transferred, &fi));
    TEST_ASSERT_EQ_INT(PATCH_LEN, transferred);
    TEST_ASSERT_EQ_INT(SEQ_INIT_LEN, fi.FileSize);

    /* WriteToEndOfFile = TRUE append of 3333 bytes (Offset ignored) */
    fill_deterministic_pattern(r->io_buf, APPEND_LEN, 0x33333333U);
    memcpy(r->shadow + SEQ_INIT_LEN, r->io_buf, APPEND_LEN);
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Write(r->fs, r->file_ctx, r->io_buf, 0, APPEND_LEN,
                                    TRUE, FALSE, &transferred, &fi));
    TEST_ASSERT_EQ_INT(APPEND_LEN, transferred);
    TEST_ASSERT_EQ_INT(STREAM_LEN, fi.FileSize);

    /* ConstrainedIo = TRUE crossing EOF (must clamp to remaining 100 bytes) */
    fill_deterministic_pattern(r->io_buf, CONSTR_REQ_LEN, 0x44444444U);
    memcpy(r->shadow + (STREAM_LEN - CONSTR_TAIL), r->io_buf, CONSTR_TAIL);
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Write(r->fs, r->file_ctx, r->io_buf,
                                    (UINT64)(STREAM_LEN - CONSTR_TAIL), CONSTR_REQ_LEN,
                                    FALSE, TRUE, &transferred, &fi));
    TEST_ASSERT_EQ_INT(CONSTR_TAIL, transferred);
    TEST_ASSERT_EQ_INT(STREAM_LEN, fi.FileSize);

    /* ConstrainedIo = TRUE starting at EOF (must write 0 bytes and succeed) */
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Write(r->fs, r->file_ctx, r->io_buf,
                                    (UINT64)STREAM_LEN, 256,
                                    FALSE, TRUE, &transferred, &fi));
    TEST_ASSERT_EQ_INT(0, transferred);
    TEST_ASSERT_EQ_INT(STREAM_LEN, fi.FileSize);

    /* Unaligned chunked readback (13000-byte chunks) + EOF check */
    {
        size_t off = 0;
        while (off < STREAM_LEN) {
            transferred = 0;
            TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                               iface->Read(r->fs, r->file_ctx, r->io_buf, (UINT64)off, 13000, &transferred));
            TEST_ASSERT(transferred > 0 && transferred <= 13000);
            TEST_ASSERT(off + transferred <= STREAM_LEN);
            TEST_ASSERT_EQ_INT(0, memcmp(r->io_buf, r->shadow + off, transferred));
            off += transferred;
        }
        TEST_ASSERT_EQ_INT(STREAM_LEN, off);
        TEST_ASSERT_EQ_INT(STATUS_END_OF_FILE,
                           iface->Read(r->fs, r->file_ctx, r->io_buf, (UINT64)STREAM_LEN, 64, &transferred));
    }
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->Flush(r->fs, r->file_ctx, &fi));
    iface->Cleanup(r->fs, r->file_ctx, L"\\fast\\stream.bin", 0);
    iface->Close(r->fs, r->file_ctx);
    r->file_ctx = NULL;

    /* Sparse hole + SetFileSize + Overwrite on \fast\sparse.bin */
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Create(r->fs, L"\\fast\\sparse.bin", FILE_NON_DIRECTORY_FILE,
                                     FILE_WRITE_DATA | FILE_READ_DATA, FILE_ATTRIBUTE_ARCHIVE,
                                     NULL, 0, &r->file_ctx, &fi));
    fill_deterministic_pattern(r->aux_buf, 5000, 0x55555555U);
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Write(r->fs, r->file_ctx, r->aux_buf, 0, 5000,
                                    FALSE, FALSE, &transferred, &fi));
    TEST_ASSERT_EQ_INT(5000, transferred);

    fill_deterministic_pattern(r->io_buf, 6000, 0x66666666U);
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Write(r->fs, r->file_ctx, r->io_buf, 131072, 6000,
                                    FALSE, FALSE, &transferred, &fi));
    TEST_ASSERT_EQ_INT(6000, transferred);

    /* Read 131072 bytes starting at 4096 spanning [4096..5000), hole [5000..131072), [131072..135168) */
    memset(r->io_buf, 0xAA, 131072);
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Read(r->fs, r->file_ctx, r->io_buf, 4096, 131072, &transferred));
    TEST_ASSERT_EQ_INT(131072, transferred);
    TEST_ASSERT_EQ_INT(0, memcmp(r->io_buf, r->aux_buf + 4096, 5000 - 4096));
    for (size_t i = 5000 - 4096; i < 131072 - 4096; i++) {
        if (r->io_buf[i] != 0) {
            TEST_ASSERT_EQ_INT(0, r->io_buf[i]);
        }
    }
    fill_deterministic_pattern(r->aux_buf, 6000, 0x66666666U);
    TEST_ASSERT_EQ_INT(0, memcmp(r->io_buf + (131072 - 4096), r->aux_buf, 4096));

    /* SetFileSize truncate, allocation grow (no size change), EOF extend (zero-fill), allocation shrink */
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->SetFileSize(r->fs, r->file_ctx, 65536, FALSE, &fi));
    TEST_ASSERT_EQ_INT(65536, fi.FileSize);

    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->SetFileSize(r->fs, r->file_ctx, 90000, TRUE, &fi));
    TEST_ASSERT_EQ_INT(65536, fi.FileSize);

    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->SetFileSize(r->fs, r->file_ctx, 90000, FALSE, &fi));
    TEST_ASSERT_EQ_INT(90000, fi.FileSize);
    memset(r->io_buf, 0xCC, 24464);
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Read(r->fs, r->file_ctx, r->io_buf, 65536, 24464, &transferred));
    TEST_ASSERT_EQ_INT(24464, transferred);
    for (size_t i = 0; i < 24464; i++) {
        if (r->io_buf[i] != 0) {
            TEST_ASSERT_EQ_INT(0, r->io_buf[i]);
        }
    }

    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->SetFileSize(r->fs, r->file_ctx, 4000, TRUE, &fi));
    TEST_ASSERT_EQ_INT(4000, fi.FileSize);

    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Overwrite(r->fs, r->file_ctx, FILE_ATTRIBUTE_ARCHIVE, TRUE, 0, &fi));
    TEST_ASSERT_EQ_INT(0, fi.FileSize);
    TEST_ASSERT_EQ_INT(STATUS_END_OF_FILE,
                       iface->Read(r->fs, r->file_ctx, r->io_buf, 0, 64, &transferred));

    fill_deterministic_pattern(r->io_buf, 8192, 0x77777777U);
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Write(r->fs, r->file_ctx, r->io_buf, 0, 8192,
                                    FALSE, FALSE, &transferred, &fi));
    TEST_ASSERT_EQ_INT(8192, transferred);
    TEST_ASSERT_EQ_INT(8192, fi.FileSize);
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->Flush(r->fs, r->file_ctx, &fi));
    iface->Cleanup(r->fs, r->file_ctx, L"\\fast\\sparse.bin", 0);
    iface->Close(r->fs, r->file_ctx);
    r->file_ctx = NULL;

    /* Rename + GetDirInfoByName + SetDelete cancel & commit */
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Open(r->fs, L"\\fast\\sparse.bin", FILE_NON_DIRECTORY_FILE,
                                   DELETE, &r->file_ctx, &fi));
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Rename(r->fs, r->file_ctx, L"\\fast\\sparse.bin",
                                     L"\\fast\\sparse_renamed.bin", FALSE));
    iface->Close(r->fs, r->file_ctx);
    r->file_ctx = NULL;

    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Open(r->fs, L"\\fast", FILE_DIRECTORY_FILE, FILE_LIST_DIRECTORY,
                                   &r->dir_ctx, &fi));
    memset(di_buf, 0, sizeof(di_buf));
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->GetDirInfoByName(r->fs, r->dir_ctx, L"SPARSE_RENAMED.BIN", di));
    TEST_ASSERT_EQ_INT(8192, di->FileInfo.FileSize);
    iface->Close(r->fs, r->dir_ctx);
    r->dir_ctx = NULL;

    /* SetDelete(TRUE) followed by SetDelete(FALSE) cancels delete on Cleanup */
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Open(r->fs, L"\\fast\\sparse_renamed.bin", FILE_NON_DIRECTORY_FILE,
                                   DELETE, &r->file_ctx, &fi));
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->SetDelete(r->fs, r->file_ctx, L"\\fast\\sparse_renamed.bin", TRUE));
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->SetDelete(r->fs, r->file_ctx, L"\\fast\\sparse_renamed.bin", FALSE));
    iface->Cleanup(r->fs, r->file_ctx, L"\\fast\\sparse_renamed.bin", 0);
    iface->Close(r->fs, r->file_ctx);
    r->file_ctx = NULL;
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->GetSecurityByName(r->fs, L"\\fast\\sparse_renamed.bin", &attrs, NULL, NULL));

    /* SetDelete(TRUE) followed by Cleanup(..., 0) commits delete via handle->delete_on_close */
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Open(r->fs, L"\\fast\\sparse_renamed.bin", FILE_NON_DIRECTORY_FILE,
                                   DELETE, &r->file_ctx, &fi));
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->SetDelete(r->fs, r->file_ctx, L"\\fast\\sparse_renamed.bin", TRUE));
    iface->Cleanup(r->fs, r->file_ctx, L"\\fast\\sparse_renamed.bin", 0);
    iface->Close(r->fs, r->file_ctx);
    r->file_ctx = NULL;
    TEST_ASSERT_EQ_INT(STATUS_OBJECT_NAME_NOT_FOUND,
                       iface->GetSecurityByName(r->fs, L"\\fast\\sparse_renamed.bin", &attrs, NULL, NULL));

    /* Unmount RW and remount Read-Only (default Direct Extent I/O ON) */
    winfsp_driver_destroy_test_fs(r->fs);
    r->fs = NULL;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(r->ctx));
    r->ctx = NULL;

    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = r->img_path;
    cfg.read_only = 1;
    cfg.enable_rw_fast_io = 0;
    cfg.case_insensitive = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &r->ctx));
    TEST_ASSERT_EQ_INT(1, r->ctx->enable_direct_io);
    TEST_ASSERT_EQ_INT(0, r->ctx->enable_rw_fast_io);
    TEST_ASSERT_EQ_INT(0, winfsp_driver_create_test_fs(r->ctx, &r->fs));

    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Open(r->fs, L"\\fast\\stream.bin", FILE_NON_DIRECTORY_FILE,
                                   FILE_READ_DATA, &r->file_ctx, &fi));
    TEST_ASSERT_EQ_INT(STREAM_LEN, fi.FileSize);

    {
        size_t off = 0;
        memset(r->io_buf, 0, STREAM_LEN);
        while (off < STREAM_LEN) {
            transferred = 0;
            TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                               iface->Read(r->fs, r->file_ctx, r->io_buf + off,
                                           (UINT64)off, (ULONG)(STREAM_LEN - off), &transferred));
            TEST_ASSERT(transferred > 0);
            off += transferred;
        }
        TEST_ASSERT_EQ_INT(STREAM_LEN, off);
        TEST_ASSERT_EQ_INT(0, memcmp(r->io_buf, r->shadow, STREAM_LEN));
    }

    TEST_ASSERT_EQ_INT(STATUS_MEDIA_WRITE_PROTECTED,
                       iface->Write(r->fs, r->file_ctx, r->io_buf, 0, 16,
                                    FALSE, FALSE, &transferred, &fi));
    iface->Close(r->fs, r->file_ctx);
    r->file_ctx = NULL;

    TEST_ASSERT_EQ_INT(STATUS_OBJECT_NAME_NOT_FOUND,
                       iface->GetSecurityByName(r->fs, L"\\fast\\sparse_renamed.bin", &attrs, NULL, NULL));

    winfsp_driver_destroy_test_fs(r->fs);
    r->fs = NULL;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(r->ctx));
    r->ctx = NULL;
    return 0;
}

static int test_winfsp_rw_fast_io_e2e(void) {
    rw_fast_io_e2e_res_t res;
    const FSP_FILE_SYSTEM_INTERFACE *iface = winfsp_driver_get_interface();
    int rc;

    memset(&res, 0, sizeof(res));
    test_make_temp_path(res.img_path, sizeof(res.img_path), "winfsp_rw_fast_io.img");
    res.shadow = (uint8_t *)malloc(134405);
    res.io_buf = (uint8_t *)malloc(134405);
    res.aux_buf = (uint8_t *)malloc(6000);
    if (!res.shadow || !res.io_buf || !res.aux_buf) {
        int alloc_ok = (res.shadow != NULL && res.io_buf != NULL && res.aux_buf != NULL);
        free(res.shadow);
        free(res.io_buf);
        free(res.aux_buf);
        TEST_ASSERT(alloc_ok);
        return 1;
    }

    rc = test_winfsp_rw_fast_io_e2e_impl(&res);

    if (iface) {
        if (res.file_ctx) iface->Close(res.fs, res.file_ctx);
        if (res.dir_ctx) iface->Close(res.fs, res.dir_ctx);
    }
    if (res.fs) winfsp_driver_destroy_test_fs(res.fs);
    if (res.ctx) ext4_vfs_unmount(res.ctx);
    DeleteFileA(res.img_path);
    free(res.shadow);
    free(res.io_buf);
    free(res.aux_buf);
    return rc;
}

static int test_winfsp_symlink_reparse_points_and_namespace_guards(void) {
    char img_path[MAX_PATH];
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    FSP_FILE_SYSTEM *fs = NULL;
    const FSP_FILE_SYSTEM_INTERFACE *iface = winfsp_driver_get_interface();
    FSP_FSCTL_VOLUME_PARAMS vp;
    FSP_FSCTL_FILE_INFO fi;
    PVOID fctx = NULL, dctx = NULL;
    UINT32 attrs = 0, reparse_idx = 0;
    uint8_t *buf = (uint8_t *)calloc(1, 4096);
    SIZE_T sz;
    FSP_IO_STATUS_BLOCK iosb;
    int alloc_ok = (buf != NULL);

    TEST_ASSERT(alloc_ok);
    test_make_temp_path(img_path, sizeof(img_path), "winfsp_reparse.img");
    DeleteFileA(img_path);

    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 8ULL * 1024ULL * 1024ULL, "REPARSE"));
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    cfg.case_insensitive = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/target.txt", 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkdir(ctx, "/subdir", 0755, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_create(ctx, "/subdir/nested.txt", 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_symlink(ctx, "/sym_rel", "target.txt"));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_symlink(ctx, "/subdir/sym_abs", "/target.txt"));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_symlink(ctx, "/dir_link", "subdir"));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_symlink(ctx, "/cycle_a", "cycle_b"));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_symlink(ctx, "/cycle_b", "cycle_a"));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_symlink(ctx, "/escape_link", "../../outside.txt"));
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    /* Mount RO and inspect via WinFsp callbacks */
    cfg.read_only = 1;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, winfsp_driver_create_test_fs(ctx, &fs));
    winfsp_driver_build_volume_params(ctx, &vp);
    TEST_ASSERT_EQ_INT(1, vp.ReparsePoints);

    /* File symlink has REPARSE_POINT without DIRECTORY */
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->GetSecurityByName(fs, L"\\sym_rel", &attrs, NULL, NULL));
    TEST_ASSERT((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
    TEST_ASSERT((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0);

    /* Directory symlink has REPARSE_POINT | DIRECTORY; cycle symlink does not hang */
    attrs = 0;
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->GetSecurityByName(fs, L"\\dir_link", &attrs, NULL, NULL));
    TEST_ASSERT((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
    TEST_ASSERT((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);

    attrs = 0;
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->GetSecurityByName(fs, L"\\cycle_a", &attrs, NULL, NULL));
    TEST_ASSERT((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
    TEST_ASSERT((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0);

    /* Path traversing directory symlink returns STATUS_REPARSE and resolves relative target */
    reparse_idx = 0;
    TEST_ASSERT_EQ_INT(STATUS_REPARSE,
                       iface->GetSecurityByName(fs, L"\\dir_link\\nested.txt", &reparse_idx, NULL, NULL));
    TEST_ASSERT_EQ_INT(1, (int)reparse_idx);
    memset(&iosb, 0, sizeof(iosb));
    sz = 4096;
    TEST_ASSERT_EQ_INT(STATUS_REPARSE,
                       iface->ResolveReparsePoints(fs, L"\\dir_link\\nested.txt", reparse_idx, FALSE,
                                                   &iosb, buf, &sz));
    TEST_ASSERT_EQ_INT((long long)IO_REPARSE_TAG_SYMLINK, (long long)(UINT32)iosb.Information);
    {
        const FSP_SYMLINK_REPARSE_DATA_BUFFER *rb = (const FSP_SYMLINK_REPARSE_DATA_BUFFER *)buf;
        TEST_ASSERT_EQ_INT(IO_REPARSE_TAG_SYMLINK, rb->ReparseTag);
        TEST_ASSERT_EQ_INT(SYMLINK_FLAG_RELATIVE, rb->Flags);
        TEST_ASSERT_EQ_INT((int)(wcslen(L"subdir") * sizeof(WCHAR)), rb->SubstituteNameLength);
        TEST_ASSERT(wcsncmp(rb->PathBuffer, L"subdir", wcslen(L"subdir")) == 0);
    }

    /* POSIX absolute symlink in subdirectory translates to volume-relative "..\target.txt" */
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Open(fs, L"\\subdir\\sym_abs", FILE_OPEN_REPARSE_POINT, FILE_READ_ATTRIBUTES, &fctx, &fi));
    TEST_ASSERT((fi.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
    sz = 4096;
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->GetReparsePoint(fs, fctx, L"\\subdir\\sym_abs", buf, &sz));
    {
        const FSP_SYMLINK_REPARSE_DATA_BUFFER *rb = (const FSP_SYMLINK_REPARSE_DATA_BUFFER *)buf;
        TEST_ASSERT_EQ_INT((int)(wcslen(L"..\\target.txt") * sizeof(WCHAR)), rb->SubstituteNameLength);
        TEST_ASSERT(wcsncmp(rb->PathBuffer, L"..\\target.txt", wcslen(L"..\\target.txt")) == 0);
    }
    iface->Close(fs, fctx);
    fctx = NULL;

    /* Escaping symlink target must not resolve to host relative path */
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Open(fs, L"\\escape_link", FILE_OPEN_REPARSE_POINT, FILE_READ_ATTRIBUTES, &fctx, &fi));
    sz = 4096;
    TEST_ASSERT_EQ_INT(STATUS_REPARSE_POINT_NOT_RESOLVED,
                       iface->GetReparsePoint(fs, fctx, L"\\escape_link", buf, &sz));
    iface->Close(fs, fctx);
    fctx = NULL;

    /* ReadDirectory post-pass augments FILE_ATTRIBUTE_DIRECTORY on directory symlinks */
    TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->Open(fs, L"\\", FILE_DIRECTORY_FILE, FILE_LIST_DIRECTORY, &dctx, &fi));
    {
        ULONG transferred = 0;
        ULONG off = 0;
        int saw_dir_link_as_dir = 0;
        int saw_sym_rel_not_dir = 0;
        TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->ReadDirectory(fs, dctx, NULL, NULL, buf, 4096, &transferred));
        while (off + sizeof(UINT16) <= transferred) {
            FSP_FSCTL_DIR_INFO *di = (FSP_FSCTL_DIR_INFO *)(buf + off);
            size_t nchars;
            WCHAR name_w[128];
            if (di->Size < sizeof(FSP_FSCTL_DIR_INFO)) break;
            nchars = (di->Size - sizeof(FSP_FSCTL_DIR_INFO)) / sizeof(WCHAR);
            if (nchars < _countof(name_w)) {
                memcpy(name_w, di->FileNameBuf, nchars * sizeof(WCHAR));
                name_w[nchars] = L'\0';
                if (wcscmp(name_w, L"dir_link") == 0 &&
                    (di->FileInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                    (di->FileInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    saw_dir_link_as_dir = 1;
                }
                if (wcscmp(name_w, L"sym_rel") == 0 &&
                    (di->FileInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                    !(di->FileInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    saw_sym_rel_not_dir = 1;
                }
            }
            off += FSP_FSCTL_DEFAULT_ALIGN_UP(di->Size, 8);
        }
        TEST_ASSERT_EQ_INT(1, saw_dir_link_as_dir);
        TEST_ASSERT_EQ_INT(1, saw_sym_rel_not_dir);
    }
    iface->Close(fs, dctx);
    dctx = NULL;

    winfsp_driver_destroy_test_fs(fs);
    fs = NULL;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    /* RW mount: SetReparsePoint creates symlink + Windows namespace guards reject reserved/invalid names */
    cfg.read_only = 0;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mount(&cfg, &ctx));
    TEST_ASSERT_EQ_INT(0, winfsp_driver_create_test_fs(ctx, &fs));

    TEST_ASSERT_EQ_INT(STATUS_OBJECT_NAME_INVALID,
                       iface->Create(fs, L"\\CON", FILE_NON_DIRECTORY_FILE, FILE_WRITE_DATA,
                                     FILE_ATTRIBUTE_ARCHIVE, NULL, 0, &fctx, &fi));
    TEST_ASSERT_EQ_INT(STATUS_OBJECT_NAME_INVALID,
                       iface->Create(fs, L"\\nul.txt", FILE_NON_DIRECTORY_FILE, FILE_WRITE_DATA,
                                     FILE_ATTRIBUTE_ARCHIVE, NULL, 0, &fctx, &fi));
    TEST_ASSERT_EQ_INT(STATUS_OBJECT_NAME_INVALID,
                       iface->Create(fs, L"\\bad:colon", FILE_NON_DIRECTORY_FILE, FILE_WRITE_DATA,
                                     FILE_ATTRIBUTE_ARCHIVE, NULL, 0, &fctx, &fi));
    TEST_ASSERT_EQ_INT(STATUS_OBJECT_NAME_INVALID,
                       iface->Create(fs, L"\\trail.", FILE_NON_DIRECTORY_FILE, FILE_WRITE_DATA,
                                     FILE_ATTRIBUTE_ARCHIVE, NULL, 0, &fctx, &fi));
    TEST_ASSERT_EQ_INT(STATUS_OBJECT_NAME_INVALID,
                       iface->Rename(fs, NULL, L"\\target.txt", L"\\AUX", FALSE));

    TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                       iface->Create(fs, L"\\created_sym", FILE_NON_DIRECTORY_FILE, FILE_WRITE_DATA,
                                     FILE_ATTRIBUTE_ARCHIVE, NULL, 0, &fctx, &fi));
    {
        const WCHAR *tgt = L"subdir\\nested.txt";
        UINT16 tbytes = (UINT16)(wcslen(tgt) * sizeof(WCHAR));
        FSP_SYMLINK_REPARSE_DATA_BUFFER *rb = (FSP_SYMLINK_REPARSE_DATA_BUFFER *)buf;
        memset(buf, 0, 512);
        rb->ReparseTag = IO_REPARSE_TAG_SYMLINK;
        rb->ReparseDataLength = (UINT16)(12 + 2 * tbytes);
        rb->SubstituteNameOffset = 0;
        rb->SubstituteNameLength = tbytes;
        rb->PrintNameOffset = tbytes;
        rb->PrintNameLength = tbytes;
        rb->Flags = SYMLINK_FLAG_RELATIVE;
        memcpy(rb->PathBuffer, tgt, tbytes);
        memcpy(rb->PathBuffer + wcslen(tgt), tgt, tbytes);
        TEST_ASSERT_EQ_INT(STATUS_SUCCESS,
                           iface->SetReparsePoint(fs, fctx, L"\\created_sym", buf, 20 + 2 * (size_t)tbytes));
        sz = 4096;
        TEST_ASSERT_EQ_INT(STATUS_SUCCESS, iface->GetReparsePoint(fs, fctx, L"\\created_sym", buf, &sz));
    }
    iface->Close(fs, fctx);
    fctx = NULL;

    winfsp_driver_destroy_test_fs(fs);
    fs = NULL;
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    ctx = NULL;

    DeleteFileA(img_path);
    free(buf);
    return 0;
}

int run_suite_winfsp_callbacks(void) {
    int suite_failures = 0;
    RUN_TEST(test_winfsp_interface_e2e);
    RUN_TEST(test_winfsp_rw_fast_io_e2e);
    RUN_TEST(test_winfsp_volume_params_ro_cache_timeout);
    RUN_TEST(test_winfsp_get_dir_info_by_name_and_large_readdir);
    RUN_TEST(test_winfsp_safe_unmount_target);
    RUN_TEST(test_winfsp_symlink_reparse_points_and_namespace_guards);
    return suite_failures;
}
