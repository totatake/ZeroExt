#include "test_common.h"
#include "../src/disk/partition_table.h"
#include "../src/disk/superblock_probe.h"
#include "../src/vfs/ext4_vfs.h"
#include "../src/io/win32_io.h"
#include <ext2fs/ext2fs.h>

static void write_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void write_le64(uint8_t *p, uint64_t v) {
    write_le32(p, (uint32_t)(v & 0xFFFFFFFFULL));
    write_le32(p + 4, (uint32_t)(v >> 32));
}

static void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

static int copy_file_into_offset(const char *dst_path, uint64_t offset, const char *src_path) {
    FILE *src = fopen(src_path, "rb");
    HANDLE dst;
    wchar_t wdst[MAX_PATH];
    uint8_t buf[65536];
    uint64_t pos = offset;
    size_t n;

    if (!src) return -1;
    MultiByteToWideChar(CP_UTF8, 0, dst_path, -1, wdst, MAX_PATH);
    dst = CreateFileW(wdst, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (dst == INVALID_HANDLE_VALUE) {
        fclose(src);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        OVERLAPPED ov;
        DWORD put = 0;
        memset(&ov, 0, sizeof(ov));
        ov.Offset = (DWORD)(pos & 0xFFFFFFFFULL);
        ov.OffsetHigh = (DWORD)(pos >> 32);
        WriteFile(dst, buf, (DWORD)n, &put, &ov);
        pos += put;
    }
    CloseHandle(dst);
    fclose(src);
    return 0;
}

static int test_gpt_disk_and_embedded_ext4(void) {
    char disk_path[MAX_PATH], part_img[MAX_PATH];
    uint8_t disk_hdr[64 * 512];
    pt_table_result_t res;
    FILE *fp;
    const uint64_t part_size = 4ULL * 1024ULL * 1024ULL;
    const uint64_t part1_lba = 2048;
    const uint64_t part1_sectors = 1024;
    const uint64_t part2_lba = 4096;
    const uint64_t part2_sectors = part_size / 512;

    test_make_temp_path(disk_path, sizeof(disk_path), "gpt_disk.img");
    test_make_temp_path(part_img, sizeof(part_img), "gpt_part2.img");

    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(part_img, part_size, "GPT_DATA"));

    /* Allocate 8 MB GPT disk image */
    fp = fopen(disk_path, "wb");
    TEST_ASSERT(fp != NULL);
    {
        uint8_t zero[65536];
        memset(zero, 0, sizeof(zero));
        for (int i = 0; i < 128; i++) fwrite(zero, 1, sizeof(zero), fp);
    }
    fclose(fp);

    memset(disk_hdr, 0, sizeof(disk_hdr));
    /* Protective MBR */
    disk_hdr[446 + 4] = 0xEE;
    write_le32(disk_hdr + 446 + 8, 1);
    write_le32(disk_hdr + 446 + 12, 16383);
    disk_hdr[510] = 0x55;
    disk_hdr[511] = 0xAA;

    /* GPT Header at LBA 1 (offset 512) */
    memcpy(disk_hdr + 512, "EFI PART", 8);
    write_le64(disk_hdr + 512 + 0x48, 2);   /* Entries start LBA 2 */
    write_le32(disk_hdr + 512 + 0x50, 128); /* 128 entries */
    write_le32(disk_hdr + 512 + 0x54, 128); /* 128 bytes/entry */

    /* Entry 1 at LBA 2 (offset 1024): EFI System Partition */
    {
        uint8_t *e1 = disk_hdr + 1024;
        /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
        uint8_t efi_guid[16] = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };
        memcpy(e1, efi_guid, 16);
        write_le64(e1 + 0x20, part1_lba);
        write_le64(e1 + 0x28, part1_lba + part1_sectors - 1);
        /* Name "EFI" */
        e1[0x38] = 'E'; e1[0x3A] = 'F'; e1[0x3C] = 'I';
    }
    /* Entry 2 at offset 1024 + 128: Linux Filesystem Data */
    {
        uint8_t *e2 = disk_hdr + 1024 + 128;
        /* 0FC63DAF-8483-4772-8E79-3D69D8477DE4 */
        uint8_t linux_guid[16] = { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 };
        memcpy(e2, linux_guid, 16);
        write_le64(e2 + 0x20, part2_lba);
        write_le64(e2 + 0x28, part2_lba + part2_sectors - 1);
        e2[0x38] = 'r'; e2[0x3A] = 'o'; e2[0x3C] = 'o'; e2[0x3E] = 't';
    }

    fp = fopen(disk_path, "r+b");
    TEST_ASSERT(fp != NULL);
    fwrite(disk_hdr, 1, sizeof(disk_hdr), fp);
    fclose(fp);

    TEST_ASSERT_EQ_INT(0, copy_file_into_offset(disk_path, part2_lba * 512, part_img));

    TEST_ASSERT_EQ_INT(0, pt_read_partitions(disk_path, 512, &res));
    TEST_ASSERT(strcmp(res.style, "GPT") == 0);
    TEST_ASSERT_EQ_INT(2, res.count);
    TEST_ASSERT_EQ_INT(1, res.partitions[0].partition_number);
    TEST_ASSERT_EQ_INT(2, res.partitions[1].partition_number);
    TEST_ASSERT(strcmp(res.partitions[1].partition_name, "root") == 0);
    TEST_ASSERT_EQ_INT(EXT4_FS_EXT4, res.partitions[1].probe.fs_kind);
    TEST_ASSERT(strcmp(res.partitions[1].probe.volume_label, "GPT_DATA") == 0);
    TEST_ASSERT_EQ_INT(EXT4_JOURNAL_NONE, res.partitions[1].probe.journal_state);

    DeleteFileA(disk_path);
    DeleteFileA(part_img);
    return 0;
}

static int test_mbr_and_ebr_logical_partitions(void) {
    char disk_path[MAX_PATH], part_img[MAX_PATH];
    uint8_t mbr[512], ebr[512];
    pt_table_result_t res;
    FILE *fp;
    const uint64_t part_size = 2ULL * 1024ULL * 1024ULL;
    const uint64_t ext_lba = 2048;
    const uint64_t part5_abs_lba = ext_lba + 64;

    test_make_temp_path(disk_path, sizeof(disk_path), "mbr_ebr_disk.img");
    test_make_temp_path(part_img, sizeof(part_img), "mbr_part5.img");

    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(part_img, part_size, "EBR_EXT4"));

    fp = fopen(disk_path, "wb");
    TEST_ASSERT(fp != NULL);
    {
        uint8_t zero[65536];
        memset(zero, 0, sizeof(zero));
        for (int i = 0; i < 64; i++) fwrite(zero, 1, sizeof(zero), fp);
    }
    fclose(fp);

    memset(mbr, 0, sizeof(mbr));
    /* Primary partition 1: NTFS */
    mbr[446 + 4] = 0x07;
    write_le32(mbr + 446 + 8, 64);
    write_le32(mbr + 446 + 12, 1024);
    /* Partition 2: Extended 0x05 at LBA 2048 */
    mbr[462 + 4] = 0x05;
    write_le32(mbr + 462 + 8, (uint32_t)ext_lba);
    write_le32(mbr + 462 + 12, 6000);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    memset(ebr, 0, sizeof(ebr));
    /* EBR Entry 0: Linux 0x83 at relative +64 -> abs 2112 */
    ebr[446 + 4] = 0x83;
    write_le32(ebr + 446 + 8, 64);
    write_le32(ebr + 446 + 12, (uint32_t)(part_size / 512));
    ebr[510] = 0x55;
    ebr[511] = 0xAA;

    fp = fopen(disk_path, "r+b");
    TEST_ASSERT(fp != NULL);
    fwrite(mbr, 1, sizeof(mbr), fp);
    fseek(fp, (long)(ext_lba * 512), SEEK_SET);
    fwrite(ebr, 1, sizeof(ebr), fp);
    fclose(fp);

    TEST_ASSERT_EQ_INT(0, copy_file_into_offset(disk_path, part5_abs_lba * 512, part_img));

    TEST_ASSERT_EQ_INT(0, pt_read_partitions(disk_path, 512, &res));
    TEST_ASSERT(strcmp(res.style, "MBR") == 0);
    TEST_ASSERT_EQ_INT(2, res.count);
    TEST_ASSERT_EQ_INT(1, res.partitions[0].partition_number);
    TEST_ASSERT_EQ_INT(5, res.partitions[1].partition_number);
    TEST_ASSERT_EQ_INT(EXT4_FS_EXT4, res.partitions[1].probe.fs_kind);
    TEST_ASSERT(strcmp(res.partitions[1].probe.volume_label, "EBR_EXT4") == 0);

    DeleteFileA(disk_path);
    DeleteFileA(part_img);
    return 0;
}

static int test_dirty_journal_and_jbd2_probe(void) {
    char img_path[MAX_PATH];
    ext4_probe_result_t probe;
    win32_io_config_t cfg;
    ext2_filsys fs = NULL;
    blk64_t jblk = 0;
    uint8_t jbuf[4096];

    test_make_temp_path(img_path, sizeof(img_path), "dirty_jbd2.img");
    TEST_ASSERT_EQ_INT(0, ext4_vfs_mkfs(img_path, 8ULL * 1024ULL * 1024ULL, "JBD2TEST"));

    /* Test 1: Set HAS_JOURNAL + JBD2 superblock in inode 8 with s_start = 42 */
    memset(&cfg, 0, sizeof(cfg));
    cfg.sector_size = 4096;
    win32_io_set_next_config(&cfg);
    TEST_ASSERT_EQ_INT(0, ext2fs_open2(img_path, NULL, EXT2_FLAG_RW | EXT2_FLAG_64BITS, 0, 0, win32_io_manager, &fs));
    TEST_ASSERT_EQ_INT(0, ext2fs_read_bitmaps(fs));

    TEST_ASSERT_EQ_INT(0, ext2fs_alloc_block2(fs, 10, NULL, &jblk));
    memset(jbuf, 0, sizeof(jbuf));
    write_be32(jbuf + 0x00, JBD2_MAGIC_NUMBER);
    write_be32(jbuf + 0x04, JBD2_SUPERBLOCK_V2);
    write_be32(jbuf + 0x08, 1);
    write_be32(jbuf + 0x0C, fs->blocksize);
    write_be32(jbuf + 0x10, 1024);
    write_be32(jbuf + 0x14, 1);
    write_be32(jbuf + 0x18, 1);
    write_be32(jbuf + 0x1C, 42); /* s_start != 0 -> dirty journal! */
    TEST_ASSERT_EQ_INT(0, io_channel_write_blk64(fs->io, jblk, 1, jbuf));

    {
        struct ext2_inode_large j_ino;
        struct ext3_extent_header *eh;
        struct ext3_extent *ex;
        memset(&j_ino, 0, sizeof(j_ino));
        j_ino.i_mode = LINUX_S_IFREG | 0600;
        j_ino.i_links_count = 1;
        j_ino.i_size = fs->blocksize;
        j_ino.i_extra_isize = 32;
        j_ino.i_flags = EXT4_EXTENTS_FL;
        ext2fs_iblk_add_blocks(fs, (struct ext2_inode *)&j_ino, 1);
        eh = (struct ext3_extent_header *)j_ino.i_block;
        eh->eh_magic = EXT3_EXT_MAGIC;
        eh->eh_entries = 1;
        eh->eh_max = 4;
        eh->eh_depth = 0;
        ex = (struct ext3_extent *)((char *)j_ino.i_block + sizeof(*eh));
        ex->ee_block = 0;
        ex->ee_len = 1;
        ex->ee_start = (__u32)jblk;
        TEST_ASSERT_EQ_INT(0, ext2fs_write_inode_full(fs, EXT2_JOURNAL_INO, (struct ext2_inode *)&j_ino, sizeof(j_ino)));
    }

    fs->super->s_feature_compat |= EXT3_FEATURE_COMPAT_HAS_JOURNAL;
    fs->super->s_journal_inum = EXT2_JOURNAL_INO;
    ext2fs_mark_super_dirty(fs);
    TEST_ASSERT_EQ_INT(0, ext2fs_close_free(&fs));

    TEST_ASSERT_EQ_INT(0, ext4_probe_partition(img_path, 0, 0, 4096, &probe));
    TEST_ASSERT_EQ_INT(EXT4_JOURNAL_NEEDS_RECOVERY, probe.journal_state);
    TEST_ASSERT_EQ_INT(42, probe.journal_start);

    /* Test 2: Clear s_start -> CLEAN, then set EXT2_ERROR_FS -> HAS_ERRORS */
    win32_io_set_next_config(&cfg);
    TEST_ASSERT_EQ_INT(0, ext2fs_open2(img_path, NULL, EXT2_FLAG_RW | EXT2_FLAG_64BITS, 0, 0, win32_io_manager, &fs));
    write_be32(jbuf + 0x1C, 0);
    TEST_ASSERT_EQ_INT(0, io_channel_write_blk64(fs->io, jblk, 1, jbuf));
    TEST_ASSERT_EQ_INT(0, ext2fs_close_free(&fs));

    TEST_ASSERT_EQ_INT(0, ext4_probe_partition(img_path, 0, 0, 4096, &probe));
    TEST_ASSERT_EQ_INT(EXT4_JOURNAL_CLEAN, probe.journal_state);

    win32_io_set_next_config(&cfg);
    TEST_ASSERT_EQ_INT(0, ext2fs_open2(img_path, NULL, EXT2_FLAG_RW | EXT2_FLAG_64BITS, 0, 0, win32_io_manager, &fs));
    fs->super->s_state |= EXT2_ERROR_FS;
    ext2fs_mark_super_dirty(fs);
    TEST_ASSERT_EQ_INT(0, ext2fs_close_free(&fs));

    TEST_ASSERT_EQ_INT(0, ext4_probe_partition(img_path, 0, 0, 4096, &probe));
    TEST_ASSERT_EQ_INT(EXT4_JOURNAL_HAS_ERRORS, probe.journal_state);

    DeleteFileA(img_path);
    return 0;
}

int run_suite_partition_and_probe(void) {
    int suite_failures = 0;
    RUN_TEST(test_gpt_disk_and_embedded_ext4);
    RUN_TEST(test_mbr_and_ebr_logical_partitions);
    RUN_TEST(test_dirty_journal_and_jbd2_probe);
    return suite_failures;
}
