#include "partition_table.h"
#include "../io/win32_io.h"
#include "../common/ext4_errno.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p) {
    return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

static int read_bytes_at(io_channel io, uint64_t offset, void *dst, size_t len) {
    uint32_t blk_sz = 512;
    uint64_t first_blk = offset / blk_sz;
    uint32_t off_in_blk = (uint32_t)(offset % blk_sz);
    uint32_t nblks = (uint32_t)((off_in_blk + len + blk_sz - 1) / blk_sz);
    uint8_t *tmp = (uint8_t *)malloc((size_t)nblks * blk_sz);
    errcode_t err;
    if (!tmp) return -1;
    io_channel_set_blksize(io, (int)blk_sz);
    err = io_channel_read_blk64(io, first_blk, (int)nblks, tmp);
    if (err == 0) {
        memcpy(dst, tmp + off_in_blk, len);
    }
    free(tmp);
    return err == 0 ? 0 : -1;
}

static void format_gpt_guid(const uint8_t g[16], char out[48]) {
    uint32_t d1 = read_le32(g);
    uint16_t d2 = read_le16(g + 4);
    uint16_t d3 = read_le16(g + 6);
    snprintf(out, 48,
             "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             d1, d2, d3, g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static const char *describe_gpt_guid(const char *guid) {
    if (_stricmp(guid, "0FC63DAF-8483-4772-8E79-3D69D8477DE4") == 0) return "Linux Filesystem Data";
    if (_stricmp(guid, "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F") == 0) return "Linux Swap";
    if (_stricmp(guid, "E6D6D379-F507-44C2-A23C-238F2A3DF928") == 0) return "Linux LVM";
    if (_stricmp(guid, "A19D880F-05FC-4D3B-A006-743F0F84911E") == 0) return "Linux RAID";
    if (_stricmp(guid, "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709") == 0) return "Linux Root (x86-64)";
    if (_stricmp(guid, "933AC7E1-2EB4-4F13-B844-0E14E2AEF915") == 0) return "Linux /home";
    if (_stricmp(guid, "C12A7328-F81F-11D2-BA4B-00A0C93EC93B") == 0) return "EFI System Partition";
    if (_stricmp(guid, "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7") == 0) return "Microsoft Basic Data";
    if (_stricmp(guid, "E3C9E316-0B5C-4DB8-817D-F92DF00215AE") == 0) return "Microsoft Reserved";
    return "GPT Partition";
}

static const char *describe_mbr_type(uint8_t type_id) {
    switch (type_id) {
    case 0x07: return "NTFS / exFAT";
    case 0x0B:
    case 0x0C: return "FAT32";
    case 0x82: return "Linux Swap";
    case 0x83: return "Linux Filesystem";
    case 0x8E: return "Linux LVM";
    case 0xEF: return "EFI System Partition";
    case 0xFD: return "Linux RAID";
    default:   return "MBR Partition";
    }
}

static int has_protective_gpt_mbr(const uint8_t mbr[512]) {
    int i;
    for (i = 0; i < 4; i++) {
        if (mbr[446 + i * 16 + 4] == 0xEE) return 1;
    }
    return 0;
}

static int parse_gpt(io_channel io, const char *path_utf8, uint32_t sector_size, pt_table_result_t *out) {
    uint8_t hdr[92];
    uint64_t entries_lba;
    uint32_t entry_count, entry_size;
    uint64_t table_off, table_bytes;
    uint8_t *table_buf;
    uint32_t i;

    if (out->total_size_bytes < (uint64_t)sector_size + 92) return 0;
    if (read_bytes_at(io, sector_size, hdr, sizeof(hdr)) != 0) return 0;
    if (memcmp(hdr, "EFI PART", 8) != 0) return 0;

    entries_lba = read_le64(hdr + 0x48);
    entry_count = read_le32(hdr + 0x50);
    entry_size = read_le32(hdr + 0x54);
    if (entry_size < 128 || entry_size > 4096 || entry_count == 0 || entry_count > 4096) return 0;

    table_off = entries_lba * sector_size;
    table_bytes = (uint64_t)entry_count * entry_size;
    if (table_off >= out->total_size_bytes) return 0;
    if (table_off + table_bytes > out->total_size_bytes) {
        table_bytes = out->total_size_bytes - table_off;
    }

    table_buf = (uint8_t *)malloc((size_t)table_bytes);
    if (!table_buf) return 0;
    if (read_bytes_at(io, table_off, table_buf, (size_t)table_bytes) != 0) {
        free(table_buf);
        return 0;
    }

    strcpy_s(out->style, sizeof(out->style), "GPT");
    out->effective_sector_size = sector_size;

    uint32_t actual_entries = (uint32_t)(table_bytes / entry_size);
    for (i = 0; i < actual_entries && out->count < PT_MAX_PARTITIONS; i++) {
        const uint8_t *e = table_buf + (size_t)i * entry_size;
        int empty_guid = 1;
        int k;
        for (k = 0; k < 16; k++) {
            if (e[k] != 0) {
                empty_guid = 0;
                break;
            }
        }
        if (empty_guid) continue;

        uint64_t first_lba = read_le64(e + 0x20);
        uint64_t last_lba = read_le64(e + 0x28);
        if (last_lba < first_lba) continue;

        pt_partition_entry_t *pe = &out->partitions[out->count++];
        memset(pe, 0, sizeof(*pe));
        pe->partition_number = i + 1;
        pe->start_sector = first_lba;
        pe->sector_count = last_lba - first_lba + 1;
        pe->offset_bytes = first_lba * sector_size;
        pe->size_bytes = pe->sector_count * sector_size;
        format_gpt_guid(e, pe->type_id);
        strcpy_s(pe->type_description, sizeof(pe->type_description), describe_gpt_guid(pe->type_id));

        {
            wchar_t wname[37];
            int c;
            for (c = 0; c < 36; c++) {
                wname[c] = (wchar_t)read_le16(e + 0x38 + c * 2);
                if (wname[c] == 0) break;
            }
            wname[36] = 0;
            ext4_wide_to_utf8(wname, pe->partition_name, sizeof(pe->partition_name));
        }

        ext4_probe_partition(path_utf8, pe->offset_bytes, pe->size_bytes, sector_size, &pe->probe);
    }

    free(table_buf);
    return 1;
}

static void parse_ebr_chain(io_channel io, const char *path_utf8, uint32_t sector_size,
                            uint64_t base_ext_lba, pt_table_result_t *out) {
    uint64_t visited[64];
    int visited_cnt = 0;
    uint64_t cur_ebr = base_ext_lba;
    uint32_t logical_num = 5;
    uint8_t ebr[512];

    for (int step = 0; step < 64 && out->count < PT_MAX_PARTITIONS; step++) {
        for (int v = 0; v < visited_cnt; v++) {
            if (visited[v] == cur_ebr) return;
        }
        visited[visited_cnt++] = cur_ebr;

        uint64_t ebr_off = cur_ebr * sector_size;
        if (ebr_off + 512 > out->total_size_bytes) break;
        if (read_bytes_at(io, ebr_off, ebr, sizeof(ebr)) != 0) break;
        if (ebr[510] != 0x55 || ebr[511] != 0xAA) break;

        uint8_t type0 = ebr[446 + 4];
        uint32_t rel0 = read_le32(ebr + 446 + 8);
        uint32_t cnt0 = read_le32(ebr + 446 + 12);
        if (type0 != 0 && cnt0 > 0) {
            pt_partition_entry_t *pe = &out->partitions[out->count++];
            memset(pe, 0, sizeof(*pe));
            pe->partition_number = logical_num++;
            pe->start_sector = cur_ebr + rel0;
            pe->sector_count = cnt0;
            pe->offset_bytes = pe->start_sector * sector_size;
            pe->size_bytes = pe->sector_count * sector_size;
            snprintf(pe->type_id, sizeof(pe->type_id), "0x%02X", type0);
            strcpy_s(pe->type_description, sizeof(pe->type_description), describe_mbr_type(type0));
            ext4_probe_partition(path_utf8, pe->offset_bytes, pe->size_bytes, sector_size, &pe->probe);
        }

        uint8_t type1 = ebr[462 + 4];
        uint32_t rel_next = read_le32(ebr + 462 + 8);
        if ((type1 == 0x05 || type1 == 0x0F || type1 == 0x85) && rel_next > 0) {
            cur_ebr = base_ext_lba + rel_next;
        } else {
            break;
        }
    }
}

int pt_read_partitions(const char *path_utf8, uint32_t sector_size_hint, pt_table_result_t *out) {
    win32_io_config_t cfg;
    win32_io_stats_t io_stats;
    io_channel io = NULL;
    uint8_t mbr[512];
    uint32_t sector_size;
    int has_mbr_sig;
    int has_prot;

    if (!path_utf8 || !out) return -1;
    memset(out, 0, sizeof(*out));
    strcpy_s(out->style, sizeof(out->style), "UNKNOWN");

    memset(&cfg, 0, sizeof(cfg));
    cfg.read_only = 1;
    cfg.disable_cache = 1;
    cfg.sector_size = sector_size_hint ? sector_size_hint : 512;

    win32_io_set_next_config(&cfg);
    if (io_channel_open(win32_io_manager, path_utf8, 0, &io) != 0) {
        win32_io_clear_next_config();
        return -1;
    }

    win32_io_get_stats(io, &io_stats);
    out->total_size_bytes = io_stats.device_size_bytes;
    sector_size = sector_size_hint ? sector_size_hint : (io_stats.effective_sector_size ? io_stats.effective_sector_size : 512);
    out->effective_sector_size = sector_size;

    if (out->total_size_bytes < 512) {
        io_channel_close(io);
        strcpy_s(out->style, sizeof(out->style), "RAW");
        return 0;
    }

    if (read_bytes_at(io, 0, mbr, sizeof(mbr)) != 0) {
        io_channel_close(io);
        return -1;
    }

    has_mbr_sig = (mbr[510] == 0x55 && mbr[511] == 0xAA);
    has_prot = has_mbr_sig && has_protective_gpt_mbr(mbr);

    if (parse_gpt(io, path_utf8, sector_size, out)) {
        io_channel_close(io);
        return 0;
    }
    if (has_prot && sector_size != 4096 && parse_gpt(io, path_utf8, 4096, out)) {
        io_channel_close(io);
        return 0;
    }

    if (has_mbr_sig && !has_prot) {
        int found_ext = 0;
        uint64_t ext_lba = 0;
        for (int i = 0; i < 4 && out->count < PT_MAX_PARTITIONS; i++) {
            const uint8_t *e = mbr + 446 + i * 16;
            uint8_t type_id = e[4];
            uint32_t start_lba = read_le32(e + 8);
            uint32_t sec_cnt = read_le32(e + 12);
            if (type_id == 0 || sec_cnt == 0) continue;
            if (type_id == 0x05 || type_id == 0x0F || type_id == 0x85) {
                if (!found_ext) {
                    found_ext = 1;
                    ext_lba = start_lba;
                }
                continue;
            }
            pt_partition_entry_t *pe = &out->partitions[out->count++];
            memset(pe, 0, sizeof(*pe));
            pe->partition_number = (uint32_t)(i + 1);
            pe->start_sector = start_lba;
            pe->sector_count = sec_cnt;
            pe->offset_bytes = (uint64_t)start_lba * sector_size;
            pe->size_bytes = (uint64_t)sec_cnt * sector_size;
            snprintf(pe->type_id, sizeof(pe->type_id), "0x%02X", type_id);
            strcpy_s(pe->type_description, sizeof(pe->type_description), describe_mbr_type(type_id));
            ext4_probe_partition(path_utf8, pe->offset_bytes, pe->size_bytes, sector_size, &pe->probe);
        }
        if (found_ext) {
            parse_ebr_chain(io, path_utf8, sector_size, ext_lba, out);
        }
        if (out->count > 0) {
            strcpy_s(out->style, sizeof(out->style), "MBR");
            io_channel_close(io);
            return 0;
        }
    }

    io_channel_close(io);

    /* Fallback: inspect raw unpartitioned filesystem at offset 0 */
    {
        ext4_probe_result_t raw_probe;
        if (ext4_probe_partition(path_utf8, 0, out->total_size_bytes, sector_size, &raw_probe) == 0) {
            pt_partition_entry_t *pe = &out->partitions[out->count++];
            memset(pe, 0, sizeof(*pe));
            pe->partition_number = 0;
            pe->start_sector = 0;
            pe->sector_count = out->total_size_bytes / sector_size;
            pe->offset_bytes = 0;
            pe->size_bytes = out->total_size_bytes;
            strcpy_s(pe->type_id, sizeof(pe->type_id), "RAW");
            strcpy_s(pe->type_description, sizeof(pe->type_description), "Raw Unpartitioned Filesystem");
            pe->probe = raw_probe;
            strcpy_s(out->style, sizeof(out->style), "RAW");
            return 0;
        }
    }

    strcpy_s(out->style, sizeof(out->style), has_mbr_sig ? "MBR" : "RAW");
    return 0;
}

int pt_resolve_partition(const char *path_utf8, uint32_t partition_number,
                         uint64_t *out_offset, uint64_t *out_size, uint32_t *out_sector_size) {
    pt_table_result_t *res = (pt_table_result_t *)calloc(1, sizeof(*res));
    uint32_t i;
    int found = -1;
    if (!res) return -1;

    if (pt_read_partitions(path_utf8, 0, res) == 0) {
        if (partition_number == 0) {
            if (strcmp(res->style, "RAW") == 0 && res->count == 1) {
                if (out_offset) *out_offset = 0;
                if (out_size) *out_size = res->partitions[0].size_bytes;
                if (out_sector_size) *out_sector_size = res->effective_sector_size;
                found = 0;
            } else {
                for (i = 0; i < res->count; i++) {
                    if (res->partitions[i].probe.fs_kind != EXT4_FS_UNKNOWN) {
                        if (out_offset) *out_offset = res->partitions[i].offset_bytes;
                        if (out_size) *out_size = res->partitions[i].size_bytes;
                        if (out_sector_size) *out_sector_size = res->effective_sector_size;
                        found = 0;
                        break;
                    }
                }
            }
        } else {
            for (i = 0; i < res->count; i++) {
                if (res->partitions[i].partition_number == partition_number) {
                    if (out_offset) *out_offset = res->partitions[i].offset_bytes;
                    if (out_size) *out_size = res->partitions[i].size_bytes;
                    if (out_sector_size) *out_sector_size = res->effective_sector_size;
                    found = 0;
                    break;
                }
            }
        }
    }

    free(res);
    return found;
}
