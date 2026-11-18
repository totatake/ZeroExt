#include "test_common.h"
#include "../src/driver/winfsp_driver.h"
#include "../src/vfs/ext4_vfs.h"

#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE     0x00000001
#endif
#ifndef FILE_NON_DIRECTORY_FILE
#define FILE_NON_DIRECTORY_FILE 0x00000040
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

    winfsp_driver_destroy_test_fs(fs);
    TEST_ASSERT_EQ_INT(0, ext4_vfs_unmount(ctx));
    DeleteFileA(img_path);
    return 0;
}

int run_suite_winfsp_callbacks(void) {
    int suite_failures = 0;
    RUN_TEST(test_winfsp_interface_e2e);
    return suite_failures;
}
