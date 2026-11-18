#ifndef LIBEXT2FS_WIN_EXTENT_MAP_H
#define LIBEXT2FS_WIN_EXTENT_MAP_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <ext2fs/ext2fs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ext4_storage_kind {
    EXT4_STORAGE_EXTENTS = 0,
    EXT4_STORAGE_INDIRECT = 1,
    EXT4_STORAGE_INLINE = 2
} ext4_storage_kind_t;

typedef struct ext4_extent_run {
    uint64_t logical_offset;  /* Byte offset from start of file */
    uint64_t physical_offset; /* Byte offset relative to partition start */
    uint64_t length;          /* Contiguous byte length */
    int      is_uninit;       /* 1 if unwritten / preallocated (must read as 0) */
} ext4_extent_run_t;

typedef struct ext4_extent_map {
    ext2_ino_t          ino;
    uint64_t            file_size;
    uint32_t            block_size;
    ext4_storage_kind_t kind;
    size_t              run_count;
    size_t              run_capacity;
    ext4_extent_run_t  *runs;
    uint8_t            *inline_data; /* Non-NULL when kind == EXT4_STORAGE_INLINE */
    size_t              inline_size;
    volatile LONG       ref_count;
} ext4_extent_map_t;

typedef struct ext4_extent_cache ext4_extent_cache_t;

/* Extract logical-to-physical extent runs (or inline bytes) for an inode.
 * Returned map has ref_count == 1; caller must release with ext4_extent_map_release(). */
int ext4_extent_extract(ext2_filsys fs, ext2_ino_t ino, ext4_extent_map_t **out_map);

void ext4_extent_map_retain(ext4_extent_map_t *map);
void ext4_extent_map_release(ext4_extent_map_t *map);

/* O(log N) binary search mapping (req_offset, req_len) -> partition-relative physical span.
 * Returns 1 if mapped to initialized physical blocks, 0 if sparse hole / uninitialized / EOF. */
int ext4_extent_resolve(const ext4_extent_map_t *map, uint64_t req_offset, uint64_t req_len,
                        uint64_t *out_phys_offset, uint64_t *out_io_len, int *out_is_sparse);

/* Lockless direct read across extent runs using win32_io_pread_direct(). */
int ext4_extent_pread(io_channel io, const ext4_extent_map_t *map, void *buf,
                      uint64_t offset, uint32_t length, uint32_t *out_read);

/* Thread-safe LRU cache keyed by ext2_ino_t (holds its own ref on each cached map). */
ext4_extent_cache_t *ext4_extent_cache_create(size_t max_entries);
void ext4_extent_cache_destroy(ext4_extent_cache_t *cache);

/* Returns retained map (caller must call ext4_extent_map_release()) or NULL on miss. */
ext4_extent_map_t *ext4_extent_cache_get(ext4_extent_cache_t *cache, ext2_ino_t ino);
void ext4_extent_cache_put(ext4_extent_cache_t *cache, ext4_extent_map_t *map);
void ext4_extent_cache_invalidate(ext4_extent_cache_t *cache, ext2_ino_t ino);
void ext4_extent_cache_clear(ext4_extent_cache_t *cache);

#ifdef __cplusplus
}
#endif

#endif /* LIBEXT2FS_WIN_EXTENT_MAP_H */
