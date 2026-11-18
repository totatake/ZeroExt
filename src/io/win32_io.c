#include "win32_io.h"
#include "../common/ext4_errno.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <ext2fs/ext2_err.h>

#define WIN32_IO_DEFAULT_SECTOR_SIZE 4096U
#define WIN32_IO_DEFAULT_CACHE_BLOCKS 1024U
#define WIN32_IO_HASH_BUCKETS 2048U

typedef struct cache_entry {
    uint64_t block;
    uint8_t *data;
    struct cache_entry *hash_next;
    struct cache_entry *lru_prev;
    struct cache_entry *lru_next;
} cache_entry_t;

typedef struct win32_private {
    HANDLE   handle;
    int      read_only;
    int      is_physical_device;
    int      disable_cache;
    uint64_t partition_offset;
    uint64_t partition_size;
    uint64_t backing_size;
    uint32_t sector_size;
    uint32_t max_cache_blocks;

    SRWLOCK  cache_lock;
    uint32_t cache_count;
    int      cache_block_size;
    cache_entry_t *buckets[WIN32_IO_HASH_BUCKETS];
    cache_entry_t *lru_head; /* MRU */
    cache_entry_t *lru_tail; /* LRU */

    struct struct_io_stats ext2_stats;
    win32_io_stats_t stats;
} win32_private_t;

static __declspec(thread) win32_io_config_t tls_next_cfg;
static __declspec(thread) int tls_has_next_cfg = 0;

void win32_io_set_next_config(const win32_io_config_t *cfg) {
    if (cfg) {
        tls_next_cfg = *cfg;
        tls_has_next_cfg = 1;
    } else {
        tls_has_next_cfg = 0;
    }
}

void win32_io_clear_next_config(void) {
    tls_has_next_cfg = 0;
    memset(&tls_next_cfg, 0, sizeof(tls_next_cfg));
}

static int is_power_of_two_u32(uint32_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

static uint64_t query_handle_size_and_sector(HANDLE h, int is_device, uint32_t *out_sector_size) {
    uint64_t size = 0;
    if (out_sector_size) {
        *out_sector_size = WIN32_IO_DEFAULT_SECTOR_SIZE;
    }

    if (is_device) {
        GET_LENGTH_INFORMATION len_info;
        DWORD returned = 0;
        DISK_GEOMETRY_EX geom;
        if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                            &len_info, sizeof(len_info), &returned, NULL)) {
            size = (uint64_t)len_info.Length.QuadPart;
        }
        if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0,
                            &geom, sizeof(geom), &returned, NULL)) {
            if (out_sector_size && is_power_of_two_u32(geom.Geometry.BytesPerSector) &&
                geom.Geometry.BytesPerSector >= 512) {
                *out_sector_size = geom.Geometry.BytesPerSector;
            }
            if (size == 0) {
                size = (uint64_t)geom.DiskSize.QuadPart;
            }
        }
    } else {
        LARGE_INTEGER li;
        if (GetFileSizeEx(h, &li)) {
            size = (uint64_t)li.QuadPart;
        }
    }
    return size;
}

static void cache_clear_locked(win32_private_t *priv) {
    cache_entry_t *cur = priv->lru_head;
    while (cur) {
        cache_entry_t *next = cur->lru_next;
        free(cur->data);
        free(cur);
        cur = next;
    }
    memset(priv->buckets, 0, sizeof(priv->buckets));
    priv->lru_head = NULL;
    priv->lru_tail = NULL;
    priv->cache_count = 0;
}

static void lru_unlink(win32_private_t *priv, cache_entry_t *e) {
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    else priv->lru_head = e->lru_next;

    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    else priv->lru_tail = e->lru_prev;

    e->lru_prev = NULL;
    e->lru_next = NULL;
}

static void lru_push_front(win32_private_t *priv, cache_entry_t *e) {
    e->lru_prev = NULL;
    e->lru_next = priv->lru_head;
    if (priv->lru_head) {
        priv->lru_head->lru_prev = e;
    }
    priv->lru_head = e;
    if (!priv->lru_tail) {
        priv->lru_tail = e;
    }
}

static cache_entry_t *cache_find_locked(win32_private_t *priv, uint64_t block) {
    uint32_t bkt = (uint32_t)(block % WIN32_IO_HASH_BUCKETS);
    cache_entry_t *e = priv->buckets[bkt];
    while (e) {
        if (e->block == block) return e;
        e = e->hash_next;
    }
    return NULL;
}

static void cache_remove_from_hash_locked(win32_private_t *priv, cache_entry_t *victim) {
    uint32_t bkt = (uint32_t)(victim->block % WIN32_IO_HASH_BUCKETS);
    cache_entry_t **pp = &priv->buckets[bkt];
    while (*pp) {
        if (*pp == victim) {
            *pp = victim->hash_next;
            break;
        }
        pp = &((*pp)->hash_next);
    }
}

static void cache_put_block(win32_private_t *priv, uint64_t block, const void *data, int blk_sz) {
    cache_entry_t *e;
    if (priv->disable_cache || priv->max_cache_blocks == 0 || blk_sz <= 0) return;

    AcquireSRWLockExclusive(&priv->cache_lock);
    if (priv->cache_block_size != blk_sz) {
        cache_clear_locked(priv);
        priv->cache_block_size = blk_sz;
    }

    e = cache_find_locked(priv, block);
    if (e) {
        memcpy(e->data, data, (size_t)blk_sz);
        lru_unlink(priv, e);
        lru_push_front(priv, e);
        ReleaseSRWLockExclusive(&priv->cache_lock);
        return;
    }

    if (priv->cache_count >= priv->max_cache_blocks && priv->lru_tail) {
        cache_entry_t *victim = priv->lru_tail;
        lru_unlink(priv, victim);
        cache_remove_from_hash_locked(priv, victim);
        free(victim->data);
        free(victim);
        priv->cache_count--;
    }

    e = (cache_entry_t *)calloc(1, sizeof(*e));
    if (e) {
        e->data = (uint8_t *)malloc((size_t)blk_sz);
        if (e->data) {
            uint32_t bkt = (uint32_t)(block % WIN32_IO_HASH_BUCKETS);
            e->block = block;
            memcpy(e->data, data, (size_t)blk_sz);
            e->hash_next = priv->buckets[bkt];
            priv->buckets[bkt] = e;
            lru_push_front(priv, e);
            priv->cache_count++;
        } else {
            free(e);
        }
    }
    ReleaseSRWLockExclusive(&priv->cache_lock);
}

static int cache_get_block(win32_private_t *priv, uint64_t block, void *dst, int blk_sz) {
    cache_entry_t *e;
    int hit = 0;
    if (priv->disable_cache || priv->max_cache_blocks == 0 || blk_sz <= 0) return 0;

    AcquireSRWLockExclusive(&priv->cache_lock);
    if (priv->cache_block_size == blk_sz) {
        e = cache_find_locked(priv, block);
        if (e) {
            memcpy(dst, e->data, (size_t)blk_sz);
            lru_unlink(priv, e);
            lru_push_front(priv, e);
            priv->stats.cache_hits++;
            hit = 1;
        }
    }
    if (!hit) {
        priv->stats.cache_misses++;
    }
    ReleaseSRWLockExclusive(&priv->cache_lock);
    return hit;
}

static void cache_invalidate_range(win32_private_t *priv, uint64_t logical_off, size_t len, int blk_sz) {
    uint64_t first_blk, last_blk, b;
    if (priv->disable_cache || blk_sz <= 0 || len == 0) return;
    first_blk = logical_off / (uint64_t)blk_sz;
    last_blk = (logical_off + len - 1) / (uint64_t)blk_sz;

    AcquireSRWLockExclusive(&priv->cache_lock);
    if (priv->cache_block_size == blk_sz) {
        for (b = first_blk; b <= last_blk; b++) {
            cache_entry_t *e = cache_find_locked(priv, b);
            if (e) {
                lru_unlink(priv, e);
                cache_remove_from_hash_locked(priv, e);
                free(e->data);
                free(e);
                priv->cache_count--;
            }
        }
    }
    ReleaseSRWLockExclusive(&priv->cache_lock);
}

static DWORD raw_pread_exact(HANDLE h, uint64_t offset, void *buf, DWORD len, DWORD *out_read) {
    DWORD total = 0;
    if (out_read) *out_read = 0;
    while (total < len) {
        OVERLAPPED ov;
        DWORD got = 0;
        uint64_t pos = offset + total;
        memset(&ov, 0, sizeof(ov));
        ov.Offset = (DWORD)(pos & 0xFFFFFFFFULL);
        ov.OffsetHigh = (DWORD)(pos >> 32);
        if (!ReadFile(h, (uint8_t *)buf + total, len - total, &got, &ov)) {
            DWORD err = GetLastError();
            if (out_read) *out_read = total;
            return err;
        }
        if (got == 0) {
            if (out_read) *out_read = total;
            return ERROR_HANDLE_EOF;
        }
        total += got;
    }
    if (out_read) *out_read = total;
    return ERROR_SUCCESS;
}

static DWORD raw_pwrite_exact(HANDLE h, uint64_t offset, const void *buf, DWORD len) {
    DWORD total = 0;
    while (total < len) {
        OVERLAPPED ov;
        DWORD put = 0;
        uint64_t pos = offset + total;
        memset(&ov, 0, sizeof(ov));
        ov.Offset = (DWORD)(pos & 0xFFFFFFFFULL);
        ov.OffsetHigh = (DWORD)(pos >> 32);
        if (!WriteFile(h, (const uint8_t *)buf + total, len - total, &put, &ov)) {
            return GetLastError();
        }
        if (put == 0) {
            return ERROR_WRITE_FAULT;
        }
        total += put;
    }
    return ERROR_SUCCESS;
}

static errcode_t win32_aligned_read(win32_private_t *priv, uint64_t logical_off, void *dst, size_t len) {
    uint64_t phys_off;
    uint32_t sec = priv->sector_size;
    DWORD err;
    DWORD got = 0;

    if (len == 0) return 0;
    if (logical_off > priv->partition_size || len > priv->partition_size - logical_off) {
        return EXT2_ET_LLSEEK_FAILED;
    }

    /* Chunk reads > 64 MiB so DWORD length and aligned bounce buffers remain bounded */
    if (len > (64U * 1024U * 1024U)) {
        size_t done = 0;
        while (done < len) {
            size_t chunk = len - done;
            if (chunk > (64U * 1024U * 1024U)) chunk = (64U * 1024U * 1024U);
            errcode_t rc = win32_aligned_read(priv, logical_off + done, (uint8_t *)dst + done, chunk);
            if (rc != 0) return rc;
            done += chunk;
        }
        return 0;
    }

    phys_off = priv->partition_offset + logical_off;

    if (!priv->is_physical_device ||
        ((phys_off % sec) == 0 && (len % sec) == 0 && (((uintptr_t)dst) % sec) == 0)) {
        err = raw_pread_exact(priv->handle, phys_off, dst, (DWORD)len, &got);
        if (err != ERROR_SUCCESS) {
            return EXT2_ET_SHORT_READ;
        }
        InterlockedAdd64((LONG64 *)&priv->stats.bytes_read, (LONG64)len);
        InterlockedAdd64((LONG64 *)&priv->ext2_stats.bytes_read, (LONG64)len);
        return 0;
    }

    {
        uint64_t aligned_start = (phys_off / sec) * sec;
        uint64_t aligned_end = ((phys_off + len + sec - 1) / sec) * sec;
        size_t aligned_len = (size_t)(aligned_end - aligned_start);
        uint8_t *tmp = (uint8_t *)_aligned_malloc(aligned_len, sec);
        if (!tmp) return EXT2_ET_NO_MEMORY;

        /* Clamp read to backing device size if near end */
        if (priv->backing_size > 0 && aligned_start + aligned_len > priv->backing_size) {
            aligned_len = (size_t)(priv->backing_size - aligned_start);
        }
        err = raw_pread_exact(priv->handle, aligned_start, tmp, (DWORD)aligned_len, &got);
        if (err != ERROR_SUCCESS && (uint64_t)got < (phys_off - aligned_start) + len) {
            _aligned_free(tmp);
            return EXT2_ET_SHORT_READ;
        }
        memcpy(dst, tmp + (phys_off - aligned_start), len);
        _aligned_free(tmp);
        InterlockedAdd64((LONG64 *)&priv->stats.bytes_read, (LONG64)len);
        InterlockedAdd64((LONG64 *)&priv->ext2_stats.bytes_read, (LONG64)len);
        return 0;
    }
}

static errcode_t win32_aligned_write(win32_private_t *priv, uint64_t logical_off, const void *src, size_t len) {
    uint64_t phys_off;
    uint32_t sec = priv->sector_size;
    DWORD err;

    if (priv->read_only) {
        return EXT2_ET_RO_FILSYS;
    }
    if (len == 0) return 0;
    if (logical_off > priv->partition_size || len > priv->partition_size - logical_off) {
        return EXT2_ET_LLSEEK_FAILED;
    }
    phys_off = priv->partition_offset + logical_off;

    if ((phys_off % sec) == 0 && (len % sec) == 0 && (((uintptr_t)src) % sec) == 0) {
        err = raw_pwrite_exact(priv->handle, phys_off, src, (DWORD)len);
        if (err != ERROR_SUCCESS) {
            return EXT2_ET_SHORT_WRITE;
        }
        InterlockedAdd64((LONG64 *)&priv->stats.bytes_written, (LONG64)len);
        InterlockedAdd64((LONG64 *)&priv->ext2_stats.bytes_written, (LONG64)len);
        return 0;
    }

    {
        uint64_t aligned_start = (phys_off / sec) * sec;
        uint64_t aligned_end = ((phys_off + len + sec - 1) / sec) * sec;
        size_t aligned_len = (size_t)(aligned_end - aligned_start);
        uint8_t *tmp = (uint8_t *)_aligned_malloc(aligned_len, sec);
        DWORD got = 0;
        if (!tmp) return EXT2_ET_NO_MEMORY;
        memset(tmp, 0, aligned_len);

        if ((phys_off % sec) != 0 || (len % sec) != 0) {
            err = raw_pread_exact(priv->handle, aligned_start, tmp, (DWORD)aligned_len, &got);
            if (err != ERROR_SUCCESS && err != ERROR_HANDLE_EOF) {
                _aligned_free(tmp);
                return EXT2_ET_SHORT_READ;
            }
            InterlockedIncrement64((LONG64 *)&priv->stats.rmw_operations);
        }
        memcpy(tmp + (phys_off - aligned_start), src, len);
        err = raw_pwrite_exact(priv->handle, aligned_start, tmp, (DWORD)aligned_len);
        _aligned_free(tmp);
        if (err != ERROR_SUCCESS) {
            return EXT2_ET_SHORT_WRITE;
        }
        InterlockedAdd64((LONG64 *)&priv->stats.bytes_written, (LONG64)len);
        InterlockedAdd64((LONG64 *)&priv->ext2_stats.bytes_written, (LONG64)len);
        return 0;
    }
}

static errcode_t win32_open(const char *name, int flags, io_channel *channel) {
    io_channel io = NULL;
    win32_private_t *priv = NULL;
    wchar_t wpath[1024];
    win32_io_config_t cfg;
    DWORD desired_access;
    DWORD share_mode;
    DWORD file_flags;
    HANDLE h;
    uint32_t detected_sector = WIN32_IO_DEFAULT_SECTOR_SIZE;
    uint64_t dev_size;

    if (!name || !channel) return EXT2_ET_INVALID_ARGUMENT;

    if (tls_has_next_cfg) {
        cfg = tls_next_cfg;
        tls_has_next_cfg = 0;
    } else {
        memset(&cfg, 0, sizeof(cfg));
    }

    if (ext4_utf8_to_wide(name, wpath, sizeof(wpath) / sizeof(wpath[0])) != 0) {
        return EXT2_ET_INVALID_ARGUMENT;
    }

    io = (io_channel)calloc(1, sizeof(*io));
    priv = (win32_private_t *)calloc(1, sizeof(*priv));
    if (!io || !priv) {
        free(io);
        free(priv);
        return EXT2_ET_NO_MEMORY;
    }

    priv->read_only = cfg.read_only || !(flags & IO_FLAG_RW);
    priv->disable_cache = cfg.disable_cache;
    priv->is_physical_device = (strncmp(name, "\\\\.\\", 4) == 0);

    desired_access = priv->read_only ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
    share_mode = (flags & IO_FLAG_EXCLUSIVE) && !priv->read_only && !priv->is_physical_device
                     ? FILE_SHARE_READ
                     : (FILE_SHARE_READ | FILE_SHARE_WRITE);
    file_flags = priv->is_physical_device ? FILE_FLAG_NO_BUFFERING : FILE_FLAG_RANDOM_ACCESS;

    h = CreateFileW(wpath, desired_access, share_mode, NULL, OPEN_EXISTING, file_flags, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        free(io);
        free(priv);
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return ENOENT;
        }
        if (err == ERROR_ACCESS_DENIED || err == ERROR_WRITE_PROTECT) {
            return EACCES;
        }
        return EIO;
    }

    dev_size = query_handle_size_and_sector(h, priv->is_physical_device, &detected_sector);
    priv->handle = h;
    priv->backing_size = dev_size;
    priv->partition_offset = cfg.partition_offset_bytes;
    if (cfg.partition_size_bytes > 0) {
        if (dev_size > 0 && cfg.partition_offset_bytes + cfg.partition_size_bytes <= dev_size) {
            priv->partition_size = cfg.partition_size_bytes;
        } else if (dev_size > cfg.partition_offset_bytes) {
            priv->partition_size = dev_size - cfg.partition_offset_bytes;
        } else {
            priv->partition_size = cfg.partition_size_bytes;
        }
    } else {
        priv->partition_size = (dev_size > cfg.partition_offset_bytes) ? (dev_size - cfg.partition_offset_bytes) : 0;
    }

    if (cfg.sector_size && is_power_of_two_u32(cfg.sector_size) && cfg.sector_size >= 512 && cfg.sector_size <= 65536) {
        priv->sector_size = cfg.sector_size;
    } else if (priv->is_physical_device) {
        priv->sector_size = detected_sector;
    } else {
        priv->sector_size = WIN32_IO_DEFAULT_SECTOR_SIZE;
    }

    priv->max_cache_blocks = cfg.max_cache_blocks ? cfg.max_cache_blocks : WIN32_IO_DEFAULT_CACHE_BLOCKS;
    InitializeSRWLock(&priv->cache_lock);
    priv->cache_block_size = 1024;
    priv->ext2_stats.num_fields = 2;
    priv->stats.device_size_bytes = priv->partition_size;
    priv->stats.effective_sector_size = priv->sector_size;

    io->magic = EXT2_ET_MAGIC_IO_CHANNEL;
    io->manager = win32_io_manager;
    io->name = _strdup(name);
    io->block_size = 1024;
    io->refcount = 1;
    io->flags = flags;
    io->private_data = priv;
    io->align = (int)priv->sector_size;

    *channel = io;
    return 0;
}

static errcode_t win32_close(io_channel channel) {
    win32_private_t *priv;
    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL) return EXT2_ET_MAGIC_IO_CHANNEL;
    if (--channel->refcount > 0) return 0;

    priv = (win32_private_t *)channel->private_data;
    if (priv) {
        if (!priv->read_only && priv->handle != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(priv->handle);
        }
        AcquireSRWLockExclusive(&priv->cache_lock);
        cache_clear_locked(priv);
        ReleaseSRWLockExclusive(&priv->cache_lock);
        if (priv->handle != INVALID_HANDLE_VALUE) {
            CloseHandle(priv->handle);
        }
        free(priv);
    }
    free(channel->name);
    channel->magic = 0;
    free(channel);
    return 0;
}

static errcode_t win32_set_blksize(io_channel channel, int blksize) {
    win32_private_t *priv;
    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL) return EXT2_ET_MAGIC_IO_CHANNEL;
    if (blksize <= 0) return EXT2_ET_INVALID_ARGUMENT;
    priv = (win32_private_t *)channel->private_data;
    if (channel->block_size != blksize) {
        AcquireSRWLockExclusive(&priv->cache_lock);
        cache_clear_locked(priv);
        priv->cache_block_size = blksize;
        ReleaseSRWLockExclusive(&priv->cache_lock);
        channel->block_size = blksize;
    }
    return 0;
}

static errcode_t win32_read_blk64(io_channel channel, unsigned long long block, int count, void *buf) {
    win32_private_t *priv;
    size_t len;
    uint64_t logical_off;
    errcode_t retval;

    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL) return EXT2_ET_MAGIC_IO_CHANNEL;
    priv = (win32_private_t *)channel->private_data;
    len = (count < 0) ? (size_t)(-count) : (size_t)count * (size_t)channel->block_size;
    logical_off = (uint64_t)block * (uint64_t)channel->block_size;

    if (count == 1 && !priv->disable_cache) {
        if (cache_get_block(priv, block, buf, channel->block_size)) {
            return 0;
        }
        retval = win32_aligned_read(priv, logical_off, buf, len);
        if (retval == 0) {
            cache_put_block(priv, block, buf, channel->block_size);
        }
        return retval;
    }

    return win32_aligned_read(priv, logical_off, buf, len);
}

static errcode_t win32_read_blk(io_channel channel, unsigned long block, int count, void *buf) {
    return win32_read_blk64(channel, (unsigned long long)block, count, buf);
}

static errcode_t win32_write_blk64(io_channel channel, unsigned long long block, int count, const void *buf) {
    win32_private_t *priv;
    size_t len;
    uint64_t logical_off;
    errcode_t retval;

    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL) return EXT2_ET_MAGIC_IO_CHANNEL;
    priv = (win32_private_t *)channel->private_data;
    if (priv->read_only) return EXT2_ET_RO_FILSYS;

    len = (count < 0) ? (size_t)(-count) : (size_t)count * (size_t)channel->block_size;
    logical_off = (uint64_t)block * (uint64_t)channel->block_size;

    retval = win32_aligned_write(priv, logical_off, buf, len);
    if (retval == 0) {
        if (count == 1 && !priv->disable_cache) {
            cache_put_block(priv, block, buf, channel->block_size);
        } else {
            cache_invalidate_range(priv, logical_off, len, channel->block_size);
        }
    }
    return retval;
}

static errcode_t win32_write_blk(io_channel channel, unsigned long block, int count, const void *buf) {
    return win32_write_blk64(channel, (unsigned long long)block, count, buf);
}

static errcode_t win32_write_byte(io_channel channel, unsigned long offset, int count, const void *buf) {
    win32_private_t *priv;
    errcode_t retval;
    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL) return EXT2_ET_MAGIC_IO_CHANNEL;
    priv = (win32_private_t *)channel->private_data;
    if (priv->read_only) return EXT2_ET_RO_FILSYS;
    if (count < 0) return EXT2_ET_INVALID_ARGUMENT;

    retval = win32_aligned_write(priv, (uint64_t)offset, buf, (size_t)count);
    if (retval == 0) {
        cache_invalidate_range(priv, (uint64_t)offset, (size_t)count, channel->block_size);
    }
    return retval;
}

static errcode_t win32_flush(io_channel channel) {
    win32_private_t *priv;
    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL) return EXT2_ET_MAGIC_IO_CHANNEL;
    priv = (win32_private_t *)channel->private_data;
    if (!priv->read_only && priv->handle != INVALID_HANDLE_VALUE) {
        if (!FlushFileBuffers(priv->handle)) {
            DWORD err = GetLastError();
            if (err != ERROR_INVALID_FUNCTION && err != ERROR_ACCESS_DENIED) {
                return EIO;
            }
        }
    }
    return 0;
}

static errcode_t win32_set_option(io_channel channel, const char *option, const char *arg) {
    win32_private_t *priv;
    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL) return EXT2_ET_MAGIC_IO_CHANNEL;
    priv = (win32_private_t *)channel->private_data;
    if (!option) return EXT2_ET_INVALID_ARGUMENT;
    if (strcmp(option, "offset") == 0 && arg) {
        uint64_t new_off = _strtoui64(arg, NULL, 0);
        priv->partition_offset = new_off;
        if (priv->backing_size > new_off) {
            priv->partition_size = priv->backing_size - new_off;
        }
        AcquireSRWLockExclusive(&priv->cache_lock);
        cache_clear_locked(priv);
        ReleaseSRWLockExclusive(&priv->cache_lock);
        return 0;
    }
    if (strcmp(option, "nocache") == 0) {
        priv->disable_cache = 1;
        AcquireSRWLockExclusive(&priv->cache_lock);
        cache_clear_locked(priv);
        ReleaseSRWLockExclusive(&priv->cache_lock);
        return 0;
    }
    return EXT2_ET_INVALID_ARGUMENT;
}

static errcode_t win32_get_stats(io_channel channel, io_stats *stats) {
    win32_private_t *priv;
    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL || !stats) return EXT2_ET_INVALID_ARGUMENT;
    priv = (win32_private_t *)channel->private_data;
    *stats = &priv->ext2_stats;
    return 0;
}

static errcode_t win32_zeroout(io_channel channel, unsigned long long block, unsigned long long count) {
    win32_private_t *priv;
    uint8_t *zeros;
    unsigned long long i;
    errcode_t retval = 0;

    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL) return EXT2_ET_MAGIC_IO_CHANNEL;
    priv = (win32_private_t *)channel->private_data;
    if (priv->read_only) return EXT2_ET_RO_FILSYS;

    zeros = (uint8_t *)calloc(1, (size_t)channel->block_size);
    if (!zeros) return EXT2_ET_NO_MEMORY;
    for (i = 0; i < count; i++) {
        retval = win32_write_blk64(channel, block + i, 1, zeros);
        if (retval) break;
    }
    free(zeros);
    return retval;
}

void win32_io_get_stats(io_channel channel, win32_io_stats_t *out_stats) {
    win32_private_t *priv;
    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL || !out_stats) return;
    priv = (win32_private_t *)channel->private_data;
    *out_stats = priv->stats;
}

uint64_t win32_io_get_device_size(io_channel channel) {
    win32_private_t *priv;
    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL) return 0;
    priv = (win32_private_t *)channel->private_data;
    return priv->partition_size;
}

errcode_t win32_io_pread_direct(io_channel channel, uint64_t part_rel_offset,
                                void *dst, size_t len) {
    win32_private_t *priv;
    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL) {
        return EXT2_ET_MAGIC_IO_CHANNEL;
    }
    if (len == 0) return 0;
    if (!dst) return EXT2_ET_INVALID_ARGUMENT;
    priv = (win32_private_t *)channel->private_data;
    return win32_aligned_read(priv, part_rel_offset, dst, len);
}

struct struct_io_manager struct_win32_io_manager = {
    EXT2_ET_MAGIC_IO_MANAGER,
    "Win32 Native Overlapped I/O Manager",
    win32_open,
    win32_close,
    win32_set_blksize,
    win32_read_blk,
    win32_write_blk,
    win32_flush,
    win32_write_byte,
    win32_set_option,
    win32_get_stats,
    win32_read_blk64,
    win32_write_blk64,
    NULL,
    NULL,
    win32_zeroout,
    {0}
};
