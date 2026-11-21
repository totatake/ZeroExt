#include "win32_io.h"
#include "../common/ext4_errno.h"
#include "../trace/perfetto_trace.h"
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
#define WIN32_IO_CACHE_SHARDS 32U
#define WIN32_IO_SHARD_BUCKETS 64U /* 32 * 64 = 2048 total buckets */

typedef struct cache_entry {
    uint64_t block;
    uint8_t *data;
    volatile LONG referenced;
    struct cache_entry *hash_next;
    struct cache_entry *lru_prev;
    struct cache_entry *lru_next;
} cache_entry_t;

#pragma warning(push)
#pragma warning(disable: 4324)
typedef struct __declspec(align(64)) win32_cache_shard {
    SRWLOCK        lock;
    uint32_t       count;
    uint32_t       max_blocks;
    int            block_size;
    cache_entry_t *buckets[WIN32_IO_SHARD_BUCKETS];
    cache_entry_t *lru_head; /* MRU */
    cache_entry_t *lru_tail; /* LRU / CLOCK hand */
    volatile LONG64 hits;
    volatile LONG64 misses;
    volatile LONG64 bytes_read;
} win32_cache_shard_t;

typedef struct __declspec(align(64)) win32_private {
    HANDLE   handle;
    int      read_only;
    int      is_physical_device;
    int      disable_cache;
    uint64_t partition_offset;
    uint64_t partition_size;
    uint64_t backing_size;
    uint32_t sector_size;
    uint32_t max_cache_blocks;

    win32_cache_shard_t shards[WIN32_IO_CACHE_SHARDS];

    struct struct_io_stats ext2_stats;
    win32_io_stats_t stats;
} win32_private_t;
#pragma warning(pop)

static __declspec(thread) win32_io_config_t tls_next_cfg;
static __declspec(thread) int tls_has_next_cfg = 0;

static INIT_ONCE g_io_fls_once = INIT_ONCE_STATIC_INIT;
static DWORD g_io_fls_index = FLS_OUT_OF_INDEXES;

static VOID WINAPI io_fls_cleanup_cb(PVOID lpFlsData) {
    if (lpFlsData) {
        CloseHandle((HANDLE)lpFlsData);
    }
}

static BOOL CALLBACK init_io_fls_slot(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    g_io_fls_index = FlsAlloc(io_fls_cleanup_cb);
    return TRUE;
}

static HANDLE acquire_thread_io_event(int *out_owned_temp) {
    HANDLE ev = NULL;
    *out_owned_temp = 0;
    InitOnceExecuteOnce(&g_io_fls_once, init_io_fls_slot, NULL, NULL);
    if (g_io_fls_index != FLS_OUT_OF_INDEXES) {
        ev = (HANDLE)FlsGetValue(g_io_fls_index);
        if (!ev) {
            ev = CreateEventW(NULL, TRUE, FALSE, NULL);
            if (ev && !FlsSetValue(g_io_fls_index, ev)) {
                *out_owned_temp = 1;
            }
        }
        return ev;
    }
    ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ev) {
        *out_owned_temp = 1;
    }
    return ev;
}

static BOOL overlapped_ioctl(HANDLE h, DWORD code, void *out_buf, DWORD out_sz, DWORD *out_ret) {
    int temp_ev = 0;
    HANDLE ev = acquire_thread_io_event(&temp_ev);
    OVERLAPPED ov;
    DWORD ret = 0;
    BOOL ok;
    if (!ev) return FALSE;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = ev;
    ok = DeviceIoControl(h, code, NULL, 0, out_buf, out_sz, &ret, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = GetOverlappedResult(h, &ov, &ret, TRUE);
    }
    if (temp_ev) CloseHandle(ev);
    if (out_ret) *out_ret = ret;
    return ok;
}

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
        if (overlapped_ioctl(h, IOCTL_DISK_GET_LENGTH_INFO,
                             &len_info, sizeof(len_info), &returned)) {
            size = (uint64_t)len_info.Length.QuadPart;
        }
        if (overlapped_ioctl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                             &geom, sizeof(geom), &returned)) {
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

static __inline uint32_t shard_idx(uint64_t block) {
    return (uint32_t)(block & (WIN32_IO_CACHE_SHARDS - 1U));
}

static __inline uint32_t shard_bucket_idx(uint64_t block) {
    return (uint32_t)((block >> 5) & (WIN32_IO_SHARD_BUCKETS - 1U));
}

static void collect_cache_totals(const win32_private_t *priv, uint64_t *out_hits,
                                 uint64_t *out_misses, uint64_t *out_bytes_read) {
    uint64_t h = 0, m = 0, br = 0;
    for (uint32_t i = 0; i < WIN32_IO_CACHE_SHARDS; i++) {
        h += (uint64_t)InterlockedCompareExchange64((LONG64 *)&priv->shards[i].hits, 0, 0);
        m += (uint64_t)InterlockedCompareExchange64((LONG64 *)&priv->shards[i].misses, 0, 0);
        br += (uint64_t)InterlockedCompareExchange64((LONG64 *)&priv->shards[i].bytes_read, 0, 0);
    }
    if (out_hits) *out_hits = h;
    if (out_misses) *out_misses = m;
    if (out_bytes_read) *out_bytes_read = br;
}

static void shard_clear_locked(win32_cache_shard_t *sh) {
    cache_entry_t *cur = sh->lru_head;
    while (cur) {
        cache_entry_t *next = cur->lru_next;
        free(cur->data);
        free(cur);
        cur = next;
    }
    memset(sh->buckets, 0, sizeof(sh->buckets));
    sh->lru_head = NULL;
    sh->lru_tail = NULL;
    sh->count = 0;
}

static void cache_clear_all_shards(win32_private_t *priv, int new_blk_size) {
    for (uint32_t i = 0; i < WIN32_IO_CACHE_SHARDS; i++) {
        win32_cache_shard_t *sh = &priv->shards[i];
        cache_entry_t *head;
        ztrace_lock_guard_t lg = ztrace_srw_acquire_exclusive(
            &sh->lock, "win32_io.cache_lock wait (X)", "win32_io.cache_lock hold (X)");
        head = sh->lru_head;
        memset(sh->buckets, 0, sizeof(sh->buckets));
        sh->lru_head = NULL;
        sh->lru_tail = NULL;
        sh->count = 0;
        if (new_blk_size > 0) {
            sh->block_size = new_blk_size;
        }
        ztrace_srw_release_exclusive(&sh->lock, &lg);
        while (head) {
            cache_entry_t *next = head->lru_next;
            free(head->data);
            free(head);
            head = next;
        }
    }
}

static void shard_lru_unlink(win32_cache_shard_t *sh, cache_entry_t *e) {
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    else sh->lru_head = e->lru_next;

    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    else sh->lru_tail = e->lru_prev;

    e->lru_prev = NULL;
    e->lru_next = NULL;
}

static void shard_lru_push_front(win32_cache_shard_t *sh, cache_entry_t *e) {
    e->lru_prev = NULL;
    e->lru_next = sh->lru_head;
    if (sh->lru_head) {
        sh->lru_head->lru_prev = e;
    }
    sh->lru_head = e;
    if (!sh->lru_tail) {
        sh->lru_tail = e;
    }
}

static cache_entry_t *shard_find_locked(win32_cache_shard_t *sh, uint64_t block) {
    uint32_t bkt = shard_bucket_idx(block);
    cache_entry_t *e = sh->buckets[bkt];
    while (e) {
        if (e->block == block) return e;
        e = e->hash_next;
    }
    return NULL;
}

static void shard_remove_from_hash_locked(win32_cache_shard_t *sh, cache_entry_t *victim) {
    uint32_t bkt = shard_bucket_idx(victim->block);
    cache_entry_t **pp = &sh->buckets[bkt];
    while (*pp) {
        if (*pp == victim) {
            *pp = victim->hash_next;
            victim->hash_next = NULL;
            break;
        }
        pp = &((*pp)->hash_next);
    }
}

static void cache_put_block(win32_private_t *priv, uint64_t block, const void *data, int blk_sz) {
    win32_cache_shard_t *sh;
    cache_entry_t *e;
    cache_entry_t *pre_e = NULL;
    uint8_t *pre_data = NULL;
    ztrace_lock_guard_t lg;
    if (priv->disable_cache || priv->max_cache_blocks == 0 || blk_sz <= 0) return;

    sh = &priv->shards[shard_idx(block)];
    if (sh->count < sh->max_blocks) {
        pre_e = (cache_entry_t *)calloc(1, sizeof(*pre_e));
        pre_data = pre_e ? (uint8_t *)malloc((size_t)blk_sz) : NULL;
        if (pre_e && !pre_data) {
            free(pre_e);
            pre_e = NULL;
        }
    }

    for (;;) {
        lg = ztrace_srw_acquire_exclusive(&sh->lock, "win32_io.cache_lock wait (X)", "win32_io.cache_lock hold (X)");
        if (sh->block_size != blk_sz) {
            shard_clear_locked(sh);
            sh->block_size = blk_sz;
        }

        e = shard_find_locked(sh, block);
        if (e) {
            memcpy(e->data, data, (size_t)blk_sz);
            e->referenced = 1;
            shard_lru_unlink(sh, e);
            shard_lru_push_front(sh, e);
            ztrace_srw_release_exclusive(&sh->lock, &lg);
            break;
        }

        if (sh->count >= sh->max_blocks && sh->lru_tail) {
            uint32_t scanned = 0;
            cache_entry_t *victim = sh->lru_tail;
            while (victim && victim->referenced && scanned < sh->count) {
                victim->referenced = 0;
                shard_lru_unlink(sh, victim);
                shard_lru_push_front(sh, victim);
                victim = sh->lru_tail;
                scanned++;
            }
            if (victim) {
                uint32_t bkt = shard_bucket_idx(block);
                shard_lru_unlink(sh, victim);
                shard_remove_from_hash_locked(sh, victim);
                victim->block = block;
                victim->referenced = 0;
                memcpy(victim->data, data, (size_t)blk_sz);
                victim->hash_next = sh->buckets[bkt];
                sh->buckets[bkt] = victim;
                shard_lru_push_front(sh, victim);
            }
            ztrace_srw_release_exclusive(&sh->lock, &lg);
            break;
        }

        if (!pre_e) {
            ztrace_srw_release_exclusive(&sh->lock, &lg);
            pre_e = (cache_entry_t *)calloc(1, sizeof(*pre_e));
            pre_data = pre_e ? (uint8_t *)malloc((size_t)blk_sz) : NULL;
            if (!pre_e || !pre_data) {
                free(pre_data);
                free(pre_e);
                return;
            }
            continue;
        }

        {
            uint32_t bkt = shard_bucket_idx(block);
            pre_e->block = block;
            pre_e->data = pre_data;
            pre_e->referenced = 0;
            memcpy(pre_e->data, data, (size_t)blk_sz);
            pre_e->hash_next = sh->buckets[bkt];
            sh->buckets[bkt] = pre_e;
            shard_lru_push_front(sh, pre_e);
            sh->count++;
            pre_e = NULL;
            pre_data = NULL;
        }
        ztrace_srw_release_exclusive(&sh->lock, &lg);
        break;
    }

    free(pre_data);
    free(pre_e);
}

static int cache_get_block(win32_private_t *priv, uint64_t block, void *dst, int blk_sz) {
    win32_cache_shard_t *sh;
    cache_entry_t *e;
    int hit = 0;
    uint64_t sh_cnt;
    ztrace_lock_guard_t lg;
    if (priv->disable_cache || priv->max_cache_blocks == 0 || blk_sz <= 0) return 0;

    sh = &priv->shards[shard_idx(block)];
    lg = ztrace_srw_acquire_shared(&sh->lock, "win32_io.cache_lock wait (S)", "win32_io.cache_lock hold (S)");
    if (sh->block_size == blk_sz) {
        e = shard_find_locked(sh, block);
        if (e) {
            memcpy(dst, e->data, (size_t)blk_sz);
            if (!e->referenced) e->referenced = 1;
            hit = 1;
        }
    }
    ztrace_srw_release_shared(&sh->lock, &lg);

    sh_cnt = hit
        ? (uint64_t)InterlockedIncrement64(&sh->hits)
        : (uint64_t)InterlockedIncrement64(&sh->misses);

    if (ztrace_is_enabled()) {
        if (!hit || sh_cnt == 1 || (sh_cnt & 63ULL) == 0ULL) {
            uint64_t total_hits = 0, total_misses = 0;
            uint64_t rmw = (uint64_t)InterlockedCompareExchange64((LONG64 *)&priv->stats.rmw_operations, 0, 0);
            collect_cache_totals(priv, &total_hits, &total_misses, NULL);
            ztrace_record_counter("io", "LRU Block Cache", total_hits, total_misses, (int64_t)rmw, ZTRACE_ARGS_LRU_COUNTER);
        }
    }
    return hit;
}

static void cache_invalidate_range(win32_private_t *priv, uint64_t logical_off, size_t len, int blk_sz) {
    uint64_t first_blk, last_blk;
    if (priv->disable_cache || blk_sz <= 0 || len == 0) return;
    first_blk = logical_off / (uint64_t)blk_sz;
    last_blk = (logical_off + len - 1) / (uint64_t)blk_sz;

    if ((last_blk - first_blk) < WIN32_IO_CACHE_SHARDS) {
        for (uint64_t b = first_blk; b <= last_blk; b++) {
            win32_cache_shard_t *sh = &priv->shards[shard_idx(b)];
            cache_entry_t *evicted = NULL;
            ztrace_lock_guard_t lg = ztrace_srw_acquire_exclusive(
                &sh->lock, "win32_io.cache_lock wait (X)", "win32_io.cache_lock hold (X)");
            if (sh->block_size == blk_sz) {
                evicted = shard_find_locked(sh, b);
                if (evicted) {
                    shard_lru_unlink(sh, evicted);
                    shard_remove_from_hash_locked(sh, evicted);
                    sh->count--;
                }
            }
            ztrace_srw_release_exclusive(&sh->lock, &lg);
            if (evicted) {
                free(evicted->data);
                free(evicted);
            }
        }
        return;
    }

    for (uint32_t s = 0; s < WIN32_IO_CACHE_SHARDS; s++) {
        win32_cache_shard_t *sh = &priv->shards[s];
        cache_entry_t *to_free = NULL;
        ztrace_lock_guard_t lg = ztrace_srw_acquire_exclusive(
            &sh->lock, "win32_io.cache_lock wait (X)", "win32_io.cache_lock hold (X)");
        if (sh->block_size == blk_sz) {
            cache_entry_t *cur = sh->lru_head;
            while (cur) {
                cache_entry_t *next = cur->lru_next;
                if (cur->block >= first_blk && cur->block <= last_blk) {
                    shard_lru_unlink(sh, cur);
                    shard_remove_from_hash_locked(sh, cur);
                    sh->count--;
                    cur->lru_next = to_free;
                    to_free = cur;
                }
                cur = next;
            }
        }
        ztrace_srw_release_exclusive(&sh->lock, &lg);
        while (to_free) {
            cache_entry_t *next = to_free->lru_next;
            free(to_free->data);
            free(to_free);
            to_free = next;
        }
    }
}

static DWORD raw_pread_exact(HANDLE h, uint64_t offset, void *buf, DWORD len, DWORD *out_read) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    DWORD total = 0;
    DWORD res = ERROR_SUCCESS;
    int temp_ev = 0;
    HANDLE ev = acquire_thread_io_event(&temp_ev);
    if (out_read) *out_read = 0;
    if (!ev) {
        res = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }
    while (total < len) {
        OVERLAPPED ov;
        DWORD got = 0;
        uint64_t pos = offset + total;
        memset(&ov, 0, sizeof(ov));
        ov.Offset = (DWORD)(pos & 0xFFFFFFFFULL);
        ov.OffsetHigh = (DWORD)(pos >> 32);
        ov.hEvent = ev;
        if (!ReadFile(h, (uint8_t *)buf + total, len - total, &got, &ov)) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                if (!GetOverlappedResult(h, &ov, &got, TRUE)) {
                    res = GetLastError();
                    if (out_read) *out_read = total;
                    goto done;
                }
            } else {
                res = err;
                if (out_read) *out_read = total;
                goto done;
            }
        }
        if (got == 0) {
            if (out_read) *out_read = total;
            res = ERROR_HANDLE_EOF;
            goto done;
        }
        total += got;
    }
    if (out_read) *out_read = total;
done:
    if (temp_ev && ev) {
        CloseHandle(ev);
    }
    if (_zt0) {
        ztrace_record_complete("io", "ReadFile", _zt0, ztrace_now_qpc() - _zt0,
                               offset, (uint64_t)total, (int64_t)res, ZTRACE_ARGS_WIN32_IO);
    }
    return res;
}

static DWORD raw_pwrite_exact(HANDLE h, uint64_t offset, const void *buf, DWORD len) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    DWORD total = 0;
    DWORD res = ERROR_SUCCESS;
    int temp_ev = 0;
    HANDLE ev = acquire_thread_io_event(&temp_ev);
    if (!ev) {
        res = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }
    while (total < len) {
        OVERLAPPED ov;
        DWORD put = 0;
        uint64_t pos = offset + total;
        memset(&ov, 0, sizeof(ov));
        ov.Offset = (DWORD)(pos & 0xFFFFFFFFULL);
        ov.OffsetHigh = (DWORD)(pos >> 32);
        ov.hEvent = ev;
        if (!WriteFile(h, (const uint8_t *)buf + total, len - total, &put, &ov)) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                if (!GetOverlappedResult(h, &ov, &put, TRUE)) {
                    res = GetLastError();
                    goto done;
                }
            } else {
                res = err;
                goto done;
            }
        }
        if (put == 0) {
            res = ERROR_WRITE_FAULT;
            goto done;
        }
        total += put;
    }
done:
    if (temp_ev && ev) {
        CloseHandle(ev);
    }
    if (_zt0) {
        ztrace_record_complete("io", "WriteFile", _zt0, ztrace_now_qpc() - _zt0,
                               offset, (uint64_t)total, (int64_t)res, ZTRACE_ARGS_WIN32_IO);
    }
    return res;
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
        InterlockedAdd64(&priv->shards[(phys_off >> 12) & (WIN32_IO_CACHE_SHARDS - 1U)].bytes_read, (LONG64)len);
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
        InterlockedAdd64(&priv->shards[(phys_off >> 12) & (WIN32_IO_CACHE_SHARDS - 1U)].bytes_read, (LONG64)len);
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
            uint64_t _zt_rmw = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
            LONG64 rmw_cnt;
            err = raw_pread_exact(priv->handle, aligned_start, tmp, (DWORD)aligned_len, &got);
            if (_zt_rmw) {
                ztrace_record_complete("io", "win32_rmw_read", _zt_rmw, ztrace_now_qpc() - _zt_rmw,
                                       aligned_start, (uint64_t)aligned_len, (int64_t)err, ZTRACE_ARGS_WIN32_IO);
            }
            if (err != ERROR_SUCCESS && err != ERROR_HANDLE_EOF) {
                _aligned_free(tmp);
                return EXT2_ET_SHORT_READ;
            }
            rmw_cnt = InterlockedIncrement64((LONG64 *)&priv->stats.rmw_operations);
            if (ztrace_is_enabled()) {
                uint64_t total_hits = 0, total_misses = 0;
                collect_cache_totals(priv, &total_hits, &total_misses, NULL);
                ztrace_record_counter("io", "LRU Block Cache",
                                      total_hits, total_misses,
                                      (int64_t)rmw_cnt, ZTRACE_ARGS_LRU_COUNTER);
            }
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
    priv = (win32_private_t *)_aligned_malloc(sizeof(*priv), 64);
    if (!io || !priv) {
        free(io);
        _aligned_free(priv);
        return EXT2_ET_NO_MEMORY;
    }
    memset(priv, 0, sizeof(*priv));

    priv->read_only = cfg.read_only || !(flags & IO_FLAG_RW);
    priv->disable_cache = cfg.disable_cache;
    priv->is_physical_device = (strncmp(name, "\\\\.\\", 4) == 0);

    desired_access = priv->read_only ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
    share_mode = (flags & IO_FLAG_EXCLUSIVE) && !priv->read_only && !priv->is_physical_device
                     ? FILE_SHARE_READ
                     : (FILE_SHARE_READ | FILE_SHARE_WRITE);
    file_flags = (priv->is_physical_device ? FILE_FLAG_NO_BUFFERING : FILE_FLAG_RANDOM_ACCESS) |
                 FILE_FLAG_OVERLAPPED;

    h = CreateFileW(wpath, desired_access, share_mode, NULL, OPEN_EXISTING, file_flags, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        free(io);
        _aligned_free(priv);
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return ENOENT;
        }
        if (err == ERROR_ACCESS_DENIED || err == ERROR_WRITE_PROTECT) {
            return EACCES;
        }
        return EIO;
    }

    (void)SetFileCompletionNotificationModes(h, FILE_SKIP_SET_EVENT_ON_HANDLE);

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
    {
        uint32_t base = priv->max_cache_blocks / WIN32_IO_CACHE_SHARDS;
        uint32_t rem = priv->max_cache_blocks % WIN32_IO_CACHE_SHARDS;
        for (uint32_t i = 0; i < WIN32_IO_CACHE_SHARDS; i++) {
            InitializeSRWLock(&priv->shards[i].lock);
            priv->shards[i].block_size = 1024;
            priv->shards[i].max_blocks = base + (i < rem ? 1U : 0U);
            if (priv->max_cache_blocks > 0 && priv->shards[i].max_blocks == 0) {
                priv->shards[i].max_blocks = 1U;
            }
        }
    }
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
        cache_clear_all_shards(priv, 0);
        if (priv->handle != INVALID_HANDLE_VALUE) {
            CloseHandle(priv->handle);
        }
        _aligned_free(priv);
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
        cache_clear_all_shards(priv, blksize);
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
        cache_clear_all_shards(priv, 0);
        return 0;
    }
    if (strcmp(option, "nocache") == 0) {
        priv->disable_cache = 1;
        cache_clear_all_shards(priv, 0);
        return 0;
    }
    return EXT2_ET_INVALID_ARGUMENT;
}

static errcode_t win32_get_stats(io_channel channel, io_stats *stats) {
    win32_private_t *priv;
    uint64_t bytes_read = 0;
    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL || !stats) return EXT2_ET_INVALID_ARGUMENT;
    priv = (win32_private_t *)channel->private_data;
    collect_cache_totals(priv, NULL, NULL, &bytes_read);
    priv->ext2_stats.bytes_read = (unsigned long long)bytes_read;
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
    uint64_t hits = 0, misses = 0, bytes_read = 0;
    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL || !out_stats) return;
    priv = (win32_private_t *)channel->private_data;
    collect_cache_totals(priv, &hits, &misses, &bytes_read);
    *out_stats = priv->stats;
    out_stats->cache_hits = hits;
    out_stats->cache_misses = misses;
    out_stats->bytes_read = bytes_read;
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
    uint64_t _zt0;
    errcode_t rc;
    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL) {
        return EXT2_ET_MAGIC_IO_CHANNEL;
    }
    if (len == 0) return 0;
    if (!dst) return EXT2_ET_INVALID_ARGUMENT;
    priv = (win32_private_t *)channel->private_data;
    _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    rc = win32_aligned_read(priv, part_rel_offset, dst, len);
    if (_zt0) {
        ztrace_record_complete("io", "win32_io_pread_direct", _zt0, ztrace_now_qpc() - _zt0,
                               part_rel_offset, (uint64_t)len, (int64_t)rc, ZTRACE_ARGS_IO);
    }
    return rc;
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
