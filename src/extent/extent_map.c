#include "extent_map.h"
#include "../common/ext4_errno.h"
#include "../io/win32_io.h"
#include "../trace/perfetto_trace.h"
#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#define EXTENT_CACHE_SHARDS        16U
#define EXTENT_CACHE_SHARD_BUCKETS 256U /* 16 * 256 = 4096 total buckets */

typedef struct extent_cache_node {
    ext2_ino_t                 ino;
    ext4_extent_map_t         *map;
    volatile LONG              referenced;
    struct extent_cache_node  *hash_next;
    struct extent_cache_node  *lru_prev;
    struct extent_cache_node  *lru_next;
} extent_cache_node_t;

#pragma warning(push)
#pragma warning(disable: 4324)
typedef struct __declspec(align(64)) ext4_extent_cache_shard {
    SRWLOCK               lock;
    size_t                max_entries;
    size_t                count;
    volatile LONG64       hits;
    volatile LONG64       misses;
    extent_cache_node_t  *buckets[EXTENT_CACHE_SHARD_BUCKETS];
    extent_cache_node_t  *lru_head;
    extent_cache_node_t  *lru_tail;
} ext4_extent_cache_shard_t;

struct __declspec(align(64)) ext4_extent_cache {
    ext4_extent_cache_shard_t shards[EXTENT_CACHE_SHARDS];
};
#pragma warning(pop)

static int extent_append_run(ext4_extent_map_t *map, uint64_t logical_off,
                             uint64_t physical_off, uint64_t length, int is_uninit) {
    if (length == 0) return 0;

    if (map->run_count > 0) {
        ext4_extent_run_t *prev = &map->runs[map->run_count - 1];
        if (prev->is_uninit == is_uninit &&
            prev->logical_offset + prev->length == logical_off) {
            if (is_uninit || prev->physical_offset + prev->length == physical_off) {
                prev->length += length;
                return 0;
            }
        }
    }

    if (map->run_count == map->run_capacity) {
        size_t new_cap = map->run_capacity ? (map->run_capacity * 2) : 16;
        ext4_extent_run_t *grown = (ext4_extent_run_t *)realloc(
            map->runs, new_cap * sizeof(ext4_extent_run_t));
        if (!grown) return -ENOMEM;
        map->runs = grown;
        map->run_capacity = new_cap;
    }

    map->runs[map->run_count].logical_offset = logical_off;
    map->runs[map->run_count].physical_offset = physical_off;
    map->runs[map->run_count].length = length;
    map->runs[map->run_count].is_uninit = is_uninit;
    map->run_count++;
    return 0;
}

static int is_extent_end_err(errcode_t retval) {
    if (retval == EXT2_ET_EXTENT_NOT_FOUND) return 1;
#ifdef EXT2_ET_EXTENT_NO_NEXT
    if (retval == EXT2_ET_EXTENT_NO_NEXT) return 1;
#endif
    return 0;
}

static int extract_from_extent_tree(ext2_filsys fs, ext2_ino_t ino,
                                    struct ext2_inode *inode, ext4_extent_map_t *map) {
    ext2_extent_handle_t handle = NULL;
    struct ext2fs_extent ext;
    errcode_t retval;
    int rc = 0;

    retval = ext2fs_extent_open2(fs, ino, inode, &handle);
    if (retval) return ext4_errcode_to_errno(retval);

    retval = ext2fs_extent_get(handle, EXT2_EXTENT_ROOT, &ext);
    if (is_extent_end_err(retval)) {
        ext2fs_extent_free(handle);
        return 0;
    }
    if (retval) {
        ext2fs_extent_free(handle);
        return ext4_errcode_to_errno(retval);
    }

    /* When EXT2_EXTENT_ROOT lands on an interior node (depth >= 1), EXT2_EXTENT_NEXT_LEAF
     * descends into child branch 0 automatically while advancing path->visit_num. */
    while (retval == 0) {
        if ((ext.e_flags & EXT2_EXTENT_FLAGS_LEAF) && ext.e_len > 0) {
            uint64_t l_off = (uint64_t)ext.e_lblk * (uint64_t)map->block_size;
            uint64_t p_off = (uint64_t)ext.e_pblk * (uint64_t)map->block_size;
            uint64_t len = (uint64_t)ext.e_len * (uint64_t)map->block_size;
            int uninit = (ext.e_flags & EXT2_EXTENT_FLAGS_UNINIT) ? 1 : 0;
            rc = extent_append_run(map, l_off, p_off, len, uninit);
            if (rc != 0) break;
        }
        retval = ext2fs_extent_get(handle, EXT2_EXTENT_NEXT_LEAF, &ext);
    }

    ext2fs_extent_free(handle);
    if (rc != 0) return rc;
    if (retval != 0 && !is_extent_end_err(retval)) {
        return ext4_errcode_to_errno(retval);
    }
    return 0;
}

typedef struct indirect_iter_ctx {
    ext4_extent_map_t *map;
    int                err;
} indirect_iter_ctx_t;

static int indirect_block_cb(ext2_filsys fs, blk64_t *blocknr, e2_blkcnt_t blockcnt,
                             blk64_t ref_blk, int ref_offset, void *priv_data) {
    indirect_iter_ctx_t *ictx = (indirect_iter_ctx_t *)priv_data;
    uint64_t bsz;
    (void)ref_blk;
    (void)ref_offset;

    if (blockcnt < 0 || !blocknr || *blocknr == 0) {
        return 0;
    }

    bsz = (uint64_t)fs->blocksize;
    ictx->err = extent_append_run(ictx->map,
                                  (uint64_t)blockcnt * bsz,
                                  (uint64_t)(*blocknr) * bsz,
                                  bsz, 0);
    return ictx->err ? BLOCK_ABORT : 0;
}

static int extract_from_indirect(ext2_filsys fs, ext2_ino_t ino, ext4_extent_map_t *map) {
    indirect_iter_ctx_t ictx;
    errcode_t retval;

    memset(&ictx, 0, sizeof(ictx));
    ictx.map = map;

    retval = ext2fs_block_iterate3(fs, ino, BLOCK_FLAG_READ_ONLY, NULL,
                                   indirect_block_cb, &ictx);
    if (ictx.err != 0) return ictx.err;
    if (retval != 0) return ext4_errcode_to_errno(retval);
    return 0;
}

static int ext4_extent_extract_impl(ext2_filsys fs, ext2_ino_t ino, ext4_extent_map_t **out_map) {
    struct ext2_inode_large ino_st;
    ext4_extent_map_t *map;
    errcode_t retval;
    int rc = 0;

    if (!fs || ino == 0 || !out_map) return -EINVAL;
    *out_map = NULL;

    memset(&ino_st, 0, sizeof(ino_st));
    retval = ext2fs_read_inode_full(fs, ino, (struct ext2_inode *)&ino_st, sizeof(ino_st));
    if (retval != 0) return ext4_errcode_to_errno(retval);

    if (LINUX_S_ISDIR(ino_st.i_mode)) return -EISDIR;

    map = (ext4_extent_map_t *)calloc(1, sizeof(*map));
    if (!map) return -ENOMEM;

    map->ino = ino;
    map->file_size = EXT2_I_SIZE(&ino_st);
    map->block_size = fs->blocksize;
    map->ref_count = 1;

    if (ino_st.i_flags & EXT4_INLINE_DATA_FL) {
        map->kind = EXT4_STORAGE_INLINE;
        if (map->file_size > 0) {
            size_t alloc_sz = 0;
            size_t got_sz = 0;
            retval = ext2fs_inline_data_size(fs, ino, &alloc_sz);
            if (retval != 0) {
                ext4_extent_map_release(map);
                return ext4_errcode_to_errno(retval);
            }
            if (alloc_sz < EXT4_MIN_INLINE_DATA_SIZE) {
                alloc_sz = EXT4_MIN_INLINE_DATA_SIZE;
            }
            map->inline_data = (uint8_t *)malloc(alloc_sz);
            if (!map->inline_data) {
                ext4_extent_map_release(map);
                return -ENOMEM;
            }
            retval = ext2fs_inline_data_get(fs, ino, (struct ext2_inode *)&ino_st,
                                            map->inline_data, &got_sz);
            if (retval != 0) {
                ext4_extent_map_release(map);
                return ext4_errcode_to_errno(retval);
            }
            map->inline_size = ((uint64_t)got_sz > map->file_size) ? (size_t)map->file_size : got_sz;
        }
    } else if (LINUX_S_ISLNK(ino_st.i_mode) && ext2fs_is_fast_symlink((struct ext2_inode *)&ino_st)) {
        size_t sz = (size_t)map->file_size;
        if (sz > sizeof(ino_st.i_block)) sz = sizeof(ino_st.i_block);
        map->kind = EXT4_STORAGE_INLINE;
        if (sz > 0) {
            map->inline_data = (uint8_t *)malloc(sz);
            if (!map->inline_data) {
                ext4_extent_map_release(map);
                return -ENOMEM;
            }
            memcpy(map->inline_data, ino_st.i_block, sz);
            map->inline_size = sz;
        }
    } else if (ino_st.i_flags & EXT4_EXTENTS_FL) {
        map->kind = EXT4_STORAGE_EXTENTS;
        rc = extract_from_extent_tree(fs, ino, (struct ext2_inode *)&ino_st, map);
    } else {
        map->kind = EXT4_STORAGE_INDIRECT;
        rc = extract_from_indirect(fs, ino, map);
    }

    if (rc != 0) {
        ext4_extent_map_release(map);
        return rc;
    }

    *out_map = map;
    return 0;
}

int ext4_extent_extract(ext2_filsys fs, ext2_ino_t ino, ext4_extent_map_t **out_map) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    int rc = ext4_extent_extract_impl(fs, ino, out_map);
    if (_zt0) {
        uint64_t runs = (rc == 0 && out_map && *out_map) ? (uint64_t)(*out_map)->run_count : 0ULL;
        ztrace_record_complete("extent", "ext4_extent_extract", _zt0, ztrace_now_qpc() - _zt0,
                               (uint64_t)ino, runs, (int64_t)rc, ZTRACE_ARGS_EXTENT);
    }
    return rc;
}

void ext4_extent_map_retain(ext4_extent_map_t *map) {
    if (map) {
        InterlockedIncrement(&map->ref_count);
    }
}

void ext4_extent_map_release(ext4_extent_map_t *map) {
    if (!map) return;
    if (InterlockedDecrement(&map->ref_count) == 0) {
        free(map->runs);
        free(map->inline_data);
        free(map);
    }
}

int ext4_extent_resolve(const ext4_extent_map_t *map, uint64_t req_offset, uint64_t req_len,
                        uint64_t *out_phys_offset, uint64_t *out_io_len, int *out_is_sparse) {
    size_t lo, hi;
    uint64_t max_len;

    if (out_phys_offset) *out_phys_offset = 0;
    if (out_io_len) *out_io_len = 0;
    if (out_is_sparse) *out_is_sparse = 1;

    if (!map || req_len == 0 || req_offset >= map->file_size) {
        return 0;
    }

    max_len = map->file_size - req_offset;
    if (req_len < max_len) max_len = req_len;

    /* Upper bound: first run with logical_offset > req_offset */
    lo = 0;
    hi = map->run_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (map->runs[mid].logical_offset <= req_offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (lo > 0) {
        const ext4_extent_run_t *prev = &map->runs[lo - 1];
        uint64_t prev_end = prev->logical_offset + prev->length;
        if (req_offset < prev_end) {
            uint64_t in_run_off = req_offset - prev->logical_offset;
            uint64_t avail = prev->length - in_run_off;
            uint64_t take = (max_len < avail) ? max_len : avail;
            if (out_io_len) *out_io_len = take;
            if (prev->is_uninit) {
                if (out_is_sparse) *out_is_sparse = 1;
                return 0;
            }
            if (out_phys_offset) *out_phys_offset = prev->physical_offset + in_run_off;
            if (out_is_sparse) *out_is_sparse = 0;
            return 1;
        }
    }

    if (lo < map->run_count) {
        uint64_t gap = map->runs[lo].logical_offset - req_offset;
        if (out_io_len) *out_io_len = (max_len < gap) ? max_len : gap;
    } else {
        if (out_io_len) *out_io_len = max_len;
    }
    if (out_is_sparse) *out_is_sparse = 1;
    return 0;
}

static int ext4_extent_pread_impl(io_channel io, const ext4_extent_map_t *map, void *buf,
                                  uint64_t offset, uint32_t length, uint32_t *out_read) {
    uint32_t done = 0;

    if (!map || !out_read) return -EINVAL;
    *out_read = 0;
    if (length == 0 || offset >= map->file_size) return 0;
    if (!buf) return -EINVAL;

    if (map->kind == EXT4_STORAGE_INLINE) {
        uint64_t avail = map->file_size - offset;
        uint32_t to_copy = (avail < (uint64_t)length) ? (uint32_t)avail : length;
        if (offset < map->inline_size) {
            size_t in_avail = map->inline_size - (size_t)offset;
            size_t cpy = ((size_t)to_copy < in_avail) ? (size_t)to_copy : in_avail;
            memcpy(buf, map->inline_data + offset, cpy);
            if (to_copy > cpy) {
                memset((uint8_t *)buf + cpy, 0, to_copy - cpy);
            }
        } else {
            memset(buf, 0, to_copy);
        }
        *out_read = to_copy;
        return 0;
    }

    if (!io) return -EINVAL;

    while (done < length) {
        uint64_t phys_off = 0;
        uint64_t io_len = 0;
        int is_sparse = 0;
        int mapped = ext4_extent_resolve(map, offset + done, (uint64_t)(length - done),
                                         &phys_off, &io_len, &is_sparse);
        if (io_len == 0) break;

        if (!mapped || is_sparse) {
            memset((uint8_t *)buf + done, 0, (size_t)io_len);
        } else {
            errcode_t retval = win32_io_pread_direct(io, phys_off,
                                                     (uint8_t *)buf + done, (size_t)io_len);
            if (retval != 0) {
                if (done > 0) {
                    *out_read = done;
                    return 0;
                }
                return ext4_errcode_to_errno(retval);
            }
        }
        done += (uint32_t)io_len;
    }

    *out_read = done;
    return 0;
}

int ext4_extent_pread(io_channel io, const ext4_extent_map_t *map, void *buf,
                      uint64_t offset, uint32_t length, uint32_t *out_read) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    int rc = ext4_extent_pread_impl(io, map, buf, offset, length, out_read);
    if (_zt0) {
        uint32_t got = out_read ? *out_read : 0;
        ztrace_record_complete("extent", "ext4_extent_pread", _zt0, ztrace_now_qpc() - _zt0,
                               offset, (uint64_t)got, (int64_t)rc, ZTRACE_ARGS_IO);
    }
    return rc;
}

/* ---- Thread-safe Sharded Extent Map CLOCK Cache ---- */

static __inline uint32_t ext_shard_idx(ext2_ino_t ino) {
    return (uint32_t)(ino & (EXTENT_CACHE_SHARDS - 1U));
}

static __inline uint32_t ext_shard_bkt(ext2_ino_t ino) {
    return (uint32_t)((ino >> 4) & (EXTENT_CACHE_SHARD_BUCKETS - 1U));
}

static void cache_lru_unlink(ext4_extent_cache_shard_t *sh, extent_cache_node_t *n) {
    if (n->lru_prev) n->lru_prev->lru_next = n->lru_next;
    else sh->lru_head = n->lru_next;
    if (n->lru_next) n->lru_next->lru_prev = n->lru_prev;
    else sh->lru_tail = n->lru_prev;
    n->lru_prev = NULL;
    n->lru_next = NULL;
}

static void cache_lru_push_front(ext4_extent_cache_shard_t *sh, extent_cache_node_t *n) {
    n->lru_prev = NULL;
    n->lru_next = sh->lru_head;
    if (sh->lru_head) sh->lru_head->lru_prev = n;
    sh->lru_head = n;
    if (!sh->lru_tail) sh->lru_tail = n;
}

static void cache_hash_remove(ext4_extent_cache_shard_t *sh, extent_cache_node_t *n) {
    uint32_t bkt = ext_shard_bkt(n->ino);
    extent_cache_node_t **pp = &sh->buckets[bkt];
    while (*pp) {
        if (*pp == n) {
            *pp = n->hash_next;
            n->hash_next = NULL;
            break;
        }
        pp = &((*pp)->hash_next);
    }
}

static extent_cache_node_t *cache_find_locked(ext4_extent_cache_shard_t *sh, ext2_ino_t ino) {
    uint32_t bkt = ext_shard_bkt(ino);
    extent_cache_node_t *n = sh->buckets[bkt];
    while (n) {
        if (n->ino == ino) return n;
        n = n->hash_next;
    }
    return NULL;
}

static void collect_extent_cache_totals(const ext4_extent_cache_t *c,
                                        uint64_t *out_hits, uint64_t *out_misses) {
    uint64_t h = 0, m = 0;
    for (uint32_t i = 0; i < EXTENT_CACHE_SHARDS; i++) {
        h += (uint64_t)InterlockedCompareExchange64((LONG64 *)&c->shards[i].hits, 0, 0);
        m += (uint64_t)InterlockedCompareExchange64((LONG64 *)&c->shards[i].misses, 0, 0);
    }
    *out_hits = h;
    *out_misses = m;
}

ext4_extent_cache_t *ext4_extent_cache_create(size_t max_entries) {
    ext4_extent_cache_t *c = (ext4_extent_cache_t *)_aligned_malloc(sizeof(*c), 64);
    size_t total;
    size_t base, rem;
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    total = max_entries ? max_entries : 4096U;
    base = total / EXTENT_CACHE_SHARDS;
    rem = total % EXTENT_CACHE_SHARDS;
    for (uint32_t i = 0; i < EXTENT_CACHE_SHARDS; i++) {
        InitializeSRWLock(&c->shards[i].lock);
        c->shards[i].max_entries = base + (i < rem ? 1U : 0U);
        if (c->shards[i].max_entries == 0U) {
            c->shards[i].max_entries = 1U;
        }
    }
    return c;
}

void ext4_extent_cache_clear(ext4_extent_cache_t *cache) {
    if (!cache) return;
    for (uint32_t i = 0; i < EXTENT_CACHE_SHARDS; i++) {
        ext4_extent_cache_shard_t *sh = &cache->shards[i];
        extent_cache_node_t *cur;
        ztrace_lock_guard_t lg = ztrace_srw_acquire_exclusive(
            &sh->lock, "extent_cache.lock wait (X)", "extent_cache.lock hold (X)");
        cur = sh->lru_head;
        memset(sh->buckets, 0, sizeof(sh->buckets));
        sh->lru_head = NULL;
        sh->lru_tail = NULL;
        sh->count = 0;
        ztrace_srw_release_exclusive(&sh->lock, &lg);
        while (cur) {
            extent_cache_node_t *next = cur->lru_next;
            ext4_extent_map_release(cur->map);
            free(cur);
            cur = next;
        }
    }
}

void ext4_extent_cache_destroy(ext4_extent_cache_t *cache) {
    if (!cache) return;
    ext4_extent_cache_clear(cache);
    _aligned_free(cache);
}

ext4_extent_map_t *ext4_extent_cache_get(ext4_extent_cache_t *cache, ext2_ino_t ino) {
    ext4_extent_cache_shard_t *sh;
    extent_cache_node_t *n;
    ext4_extent_map_t *map = NULL;
    uint64_t sh_cnt;
    ztrace_lock_guard_t lg;
    if (!cache || ino == 0) return NULL;

    sh = &cache->shards[ext_shard_idx(ino)];
    lg = ztrace_srw_acquire_shared(&sh->lock, "extent_cache.lock wait (S)", "extent_cache.lock hold (S)");
    n = cache_find_locked(sh, ino);
    if (n) {
        map = n->map;
        ext4_extent_map_retain(map);
        if (!n->referenced) n->referenced = 1;
    }
    ztrace_srw_release_shared(&sh->lock, &lg);

    sh_cnt = map
        ? (uint64_t)InterlockedIncrement64(&sh->hits)
        : (uint64_t)InterlockedIncrement64(&sh->misses);

    if (ztrace_is_enabled()) {
        if (!map || sh_cnt == 1 || (sh_cnt & 63ULL) == 0ULL) {
            uint64_t hits = 0, misses = 0;
            collect_extent_cache_totals(cache, &hits, &misses);
            ztrace_record_counter("extent", "Extent Cache", hits, misses, 0, ZTRACE_ARGS_CACHE_COUNTER);
        }
    }
    return map;
}

void ext4_extent_cache_put(ext4_extent_cache_t *cache, ext4_extent_map_t *map) {
    ext4_extent_cache_shard_t *sh;
    extent_cache_node_t *n;
    extent_cache_node_t *pre_node = NULL;
    ext4_extent_map_t *old_map = NULL;
    ztrace_lock_guard_t lg;
    if (!cache || !map || map->ino == 0) return;

    sh = &cache->shards[ext_shard_idx(map->ino)];
    if (sh->count < sh->max_entries) {
        pre_node = (extent_cache_node_t *)calloc(1, sizeof(*pre_node));
    }

    for (;;) {
        lg = ztrace_srw_acquire_exclusive(&sh->lock, "extent_cache.lock wait (X)", "extent_cache.lock hold (X)");
        n = cache_find_locked(sh, map->ino);
        if (n) {
            old_map = n->map;
            ext4_extent_map_retain(map);
            n->map = map;
            n->referenced = 1;
            cache_lru_unlink(sh, n);
            cache_lru_push_front(sh, n);
            ztrace_srw_release_exclusive(&sh->lock, &lg);
            break;
        }

        if (sh->count >= sh->max_entries && sh->lru_tail) {
            size_t scanned = 0;
            extent_cache_node_t *victim = sh->lru_tail;
            while (victim && victim->referenced && scanned < sh->count) {
                victim->referenced = 0;
                cache_lru_unlink(sh, victim);
                cache_lru_push_front(sh, victim);
                victim = sh->lru_tail;
                scanned++;
            }
            if (victim) {
                uint32_t bkt = ext_shard_bkt(map->ino);
                cache_lru_unlink(sh, victim);
                cache_hash_remove(sh, victim);
                old_map = victim->map;
                victim->ino = map->ino;
                victim->map = map;
                victim->referenced = 0;
                ext4_extent_map_retain(map);
                victim->hash_next = sh->buckets[bkt];
                sh->buckets[bkt] = victim;
                cache_lru_push_front(sh, victim);
            }
            ztrace_srw_release_exclusive(&sh->lock, &lg);
            break;
        }

        if (!pre_node) {
            ztrace_srw_release_exclusive(&sh->lock, &lg);
            pre_node = (extent_cache_node_t *)calloc(1, sizeof(*pre_node));
            if (!pre_node) return;
            continue;
        }

        {
            uint32_t bkt = ext_shard_bkt(map->ino);
            pre_node->ino = map->ino;
            pre_node->map = map;
            pre_node->referenced = 0;
            ext4_extent_map_retain(map);
            pre_node->hash_next = sh->buckets[bkt];
            sh->buckets[bkt] = pre_node;
            cache_lru_push_front(sh, pre_node);
            sh->count++;
            pre_node = NULL;
        }
        ztrace_srw_release_exclusive(&sh->lock, &lg);
        break;
    }

    ext4_extent_map_release(old_map);
    free(pre_node);
}

void ext4_extent_cache_invalidate(ext4_extent_cache_t *cache, ext2_ino_t ino) {
    ext4_extent_cache_shard_t *sh;
    extent_cache_node_t *n;
    ztrace_lock_guard_t lg;
    if (!cache || ino == 0) return;

    sh = &cache->shards[ext_shard_idx(ino)];
    lg = ztrace_srw_acquire_exclusive(&sh->lock, "extent_cache.lock wait (X)", "extent_cache.lock hold (X)");
    n = cache_find_locked(sh, ino);
    if (n) {
        cache_lru_unlink(sh, n);
        cache_hash_remove(sh, n);
        sh->count--;
    }
    ztrace_srw_release_exclusive(&sh->lock, &lg);
    if (n) {
        ext4_extent_map_release(n->map);
        free(n);
    }
}
