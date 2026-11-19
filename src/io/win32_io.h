#ifndef ZEROEXT_WIN32_IO_H
#define ZEROEXT_WIN32_IO_H

#include <stdint.h>
#include <ext2fs/ext2_io.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct win32_io_config {
    uint64_t partition_offset_bytes;
    uint64_t partition_size_bytes; /* 0 = auto-detect full device/file length */
    uint32_t sector_size;          /* 0 = default 4096 / auto-detect */
    uint32_t max_cache_blocks;     /* 0 = default 1024 */
    int      read_only;            /* 1 = GENERIC_READ only, reject writes */
    int      disable_cache;        /* 1 = bypass block cache */
} win32_io_config_t;

typedef struct win32_io_stats {
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t rmw_operations;
    uint64_t device_size_bytes;
    uint32_t effective_sector_size;
} win32_io_stats_t;

extern struct struct_io_manager struct_win32_io_manager;
#define win32_io_manager (&struct_win32_io_manager)

/* Thread-local config applied to the next win32_io_manager->open() call */
void win32_io_set_next_config(const win32_io_config_t *cfg);
void win32_io_clear_next_config(void);
void win32_io_get_stats(io_channel channel, win32_io_stats_t *out_stats);
uint64_t win32_io_get_device_size(io_channel channel);

/* Lockless direct partition-relative read bypassing the block cache and cache_lock.
 * Handles 4KB/sector alignment automatically when reading from physical block devices. */
errcode_t win32_io_pread_direct(io_channel channel, uint64_t part_rel_offset,
                                void *dst, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZEROEXT_WIN32_IO_H */
