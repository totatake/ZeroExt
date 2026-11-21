#ifndef ZEROEXT_SUPERBLOCK_PROBE_H
#define ZEROEXT_SUPERBLOCK_PROBE_H

#include <stdint.h>

#ifndef JBD2_MAGIC_NUMBER
#define JBD2_MAGIC_NUMBER  0xc03b3998U
#define JBD2_SUPERBLOCK_V1 3
#define JBD2_SUPERBLOCK_V2 4
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ext4_fs_kind {
    EXT4_FS_UNKNOWN = 0,
    EXT4_FS_EXT2    = 2,
    EXT4_FS_EXT3    = 3,
    EXT4_FS_EXT4    = 4,
} ext4_fs_kind_t;

typedef enum ext4_journal_state {
    EXT4_JOURNAL_NONE = 0,
    EXT4_JOURNAL_CLEAN = 1,
    EXT4_JOURNAL_NEEDS_RECOVERY = 2,
    EXT4_JOURNAL_HAS_ERRORS = 3,
} ext4_journal_state_t;

typedef struct ext4_probe_result {
    ext4_fs_kind_t       fs_kind;
    ext4_journal_state_t journal_state;
    uint32_t             block_size;
    uint32_t             inode_size;
    uint64_t             blocks_count;
    uint64_t             total_bytes;
    uint64_t             free_bytes;
    uint32_t             feature_compat;
    uint32_t             feature_incompat;
    uint32_t             feature_ro_compat;
    uint32_t             journal_inum;
    uint32_t             journal_start;
    int32_t              journal_errno;
    uint16_t             state_flags;
    char                 volume_label[17];
    char                 uuid[37];
} ext4_probe_result_t;

int ext4_probe_superblock_buffer(const uint8_t *sb_1024, ext4_probe_result_t *out);
int ext4_probe_partition(const char *path_utf8, uint64_t offset_bytes, uint64_t size_bytes,
                         uint32_t sector_size, ext4_probe_result_t *out);
const char *ext4_fs_kind_name(ext4_fs_kind_t kind);
const char *ext4_journal_state_name(ext4_journal_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* ZEROEXT_SUPERBLOCK_PROBE_H */
