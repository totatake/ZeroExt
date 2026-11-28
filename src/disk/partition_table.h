#ifndef ZEROEXT4_PARTITION_TABLE_H
#define ZEROEXT4_PARTITION_TABLE_H

#include <stdint.h>
#include "superblock_probe.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PT_MAX_PARTITIONS 128

typedef struct pt_partition_entry {
    uint32_t            partition_number; /* 0 = RAW unpartitioned, 1..4 = primary/GPT, 5+ = EBR logical */
    uint64_t            start_sector;
    uint64_t            sector_count;
    uint64_t            offset_bytes;
    uint64_t            size_bytes;
    char                type_id[48];
    char                type_description[64];
    char                partition_name[72];
    char                partition_uuid[48];
    ext4_probe_result_t probe;
} pt_partition_entry_t;

typedef struct pt_table_result {
    char                 style[16]; /* "GPT", "MBR", "RAW", "UNKNOWN" */
    uint32_t             effective_sector_size;
    uint64_t             total_size_bytes;
    uint32_t             count;
    pt_partition_entry_t partitions[PT_MAX_PARTITIONS];
} pt_table_result_t;

int pt_read_partitions(const char *path_utf8, uint32_t sector_size_hint, pt_table_result_t *out);
int pt_resolve_partition(const char *path_utf8, uint32_t partition_number,
                         uint64_t *out_offset, uint64_t *out_size, uint32_t *out_sector_size);
int pt_resolve_partition_ex(const char *path_utf8, uint32_t partition_number,
                            uint64_t *out_offset, uint64_t *out_size, uint32_t *out_sector_size,
                            char *out_part_uuid, size_t out_part_uuid_sz);

#ifdef __cplusplus
}
#endif

#endif /* ZEROEXT4_PARTITION_TABLE_H */
