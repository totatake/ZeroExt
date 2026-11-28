#include "ext4_vfs.h"
#include "../io/win32_io.h"
#include "../disk/partition_table.h"
#include "../common/ext4_errno.h"
#include "../extent/extent_map.h"
#include "../overlay/overlay_vfs.h"
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DENTRY_SHARDS        16
#define DENTRY_SHARD_SLOTS   256
#define DENTRY_SHARD_BUCKETS 256
#define DENTRY_MAX_KEY       256

typedef struct dentry_slot {
    uint64_t   hash;
    ext2_ino_t ino; /* 0 = negative (-ENOENT), >0 = positive inode */
    uint16_t   key_len;
    uint8_t    valid;
    uint8_t    ref;
    uint8_t    exact_only;
    int16_t    bucket_idx;
    int16_t    next;
    int16_t    prev;
    char       key[DENTRY_MAX_KEY];
} dentry_slot_t;

#pragma warning(push)
#pragma warning(disable: 4324)
typedef struct __declspec(align(64)) dentry_shard {
    SRWLOCK         lock;
    LONG64          epoch;
    int             count;
    int             clock_hand;
    volatile LONG64 pos_hits;
    volatile LONG64 neg_hits;
    volatile LONG64 misses;
    int16_t         buckets[DENTRY_SHARD_BUCKETS];
    dentry_slot_t   slots[DENTRY_SHARD_SLOTS];
} dentry_shard_t;

struct ext4_dentry_cache {
    volatile LONG64 epoch;
    volatile LONG64 invalidations;
    dentry_shard_t  shards[DENTRY_SHARDS];
};
#pragma warning(pop)

static uint64_t dentry_hash(const char *path, size_t len, int ci) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        if (ci && c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        h ^= (uint64_t)c;
        h *= 1099511628211ULL;
    }
    return h;
}

static int dentry_key_eq(const char *a, const char *b, size_t len, int ci) {
    if (!ci) return memcmp(a, b, len) == 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
    }
    return 1;
}

static size_t dentry_shard_idx(uint64_t h) {
    return (size_t)(((h >> 4) ^ (h >> 36)) & (DENTRY_SHARDS - 1));
}

static void dentry_shard_reset_locked(dentry_shard_t *sh, LONG64 epoch) {
    sh->epoch = epoch;
    sh->count = 0;
    sh->clock_hand = 0;
    for (int i = 0; i < DENTRY_SHARD_BUCKETS; i++) {
        sh->buckets[i] = -1;
    }
    for (int i = 0; i < DENTRY_SHARD_SLOTS; i++) {
        sh->slots[i].valid = 0;
        sh->slots[i].ref = 0;
        sh->slots[i].bucket_idx = -1;
        sh->slots[i].next = -1;
        sh->slots[i].prev = -1;
    }
}

static struct ext4_dentry_cache *ext4_dentry_cache_create(void) {
    struct ext4_dentry_cache *c =
        (struct ext4_dentry_cache *)_aligned_malloc(sizeof(*c), 64);
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->epoch = 1;
    for (int s = 0; s < DENTRY_SHARDS; s++) {
        InitializeSRWLock(&c->shards[s].lock);
        dentry_shard_reset_locked(&c->shards[s], 1);
    }
    return c;
}

static void ext4_dentry_cache_destroy(struct ext4_dentry_cache *c) {
    if (c) _aligned_free(c);
}

void ext4_dentry_cache_invalidate(ext4_vfs_ctx_t *ctx) {
    if (!ctx || !ctx->dentry_cache) return;
    InterlockedIncrement64(&ctx->dentry_cache->epoch);
    InterlockedIncrement64(&ctx->dentry_cache->invalidations);
}

void ext4_vfs_get_dentry_cache_stats(ext4_vfs_ctx_t *ctx, ext4_dentry_cache_stats_t *out_stats) {
    uint64_t pos = 0, neg = 0, miss = 0;
    if (!out_stats) return;
    memset(out_stats, 0, sizeof(*out_stats));
    if (!ctx || !ctx->dentry_cache) return;
    for (int s = 0; s < DENTRY_SHARDS; s++) {
        dentry_shard_t *sh = &ctx->dentry_cache->shards[s];
        pos += (uint64_t)InterlockedCompareExchange64(&sh->pos_hits, 0, 0);
        neg += (uint64_t)InterlockedCompareExchange64(&sh->neg_hits, 0, 0);
        miss += (uint64_t)InterlockedCompareExchange64(&sh->misses, 0, 0);
    }
    out_stats->pos_hits = pos;
    out_stats->neg_hits = neg;
    out_stats->misses = miss;
    out_stats->invalidations = (uint64_t)InterlockedCompareExchange64(&ctx->dentry_cache->invalidations, 0, 0);
}

uint64_t ext4_vfs_get_ci_collision_count(ext4_vfs_ctx_t *ctx) {
    if (!ctx) return 0;
    return (uint64_t)InterlockedCompareExchange64(&ctx->stat_ci_collisions, 0, 0);
}

static void dentry_unlink_slot(dentry_shard_t *sh, int16_t idx) {
    dentry_slot_t *s = &sh->slots[idx];
    if (s->prev >= 0) {
        sh->slots[s->prev].next = s->next;
    } else if (s->bucket_idx >= 0) {
        sh->buckets[s->bucket_idx] = s->next;
    }
    if (s->next >= 0) {
        sh->slots[s->next].prev = s->prev;
    }
    s->bucket_idx = -1;
    s->prev = -1;
    s->next = -1;
    s->valid = 0;
}

static void dentry_link_slot(dentry_shard_t *sh, int16_t idx, int bidx) {
    dentry_slot_t *s = &sh->slots[idx];
    int16_t head = sh->buckets[bidx];
    s->bucket_idx = (int16_t)bidx;
    s->prev = -1;
    s->next = head;
    if (head >= 0) {
        sh->slots[head].prev = idx;
    }
    sh->buckets[bidx] = idx;
}

/* Returns: 1 = positive hit, -1 = negative hit (-ENOENT), 0 = miss */
static int dentry_cache_lookup(struct ext4_dentry_cache *c, const char *norm_path, size_t len,
                               int ci, ext2_ino_t *out_ino, char *out_leaf, size_t out_leaf_sz) {
    uint64_t h;
    dentry_shard_t *sh;
    int bidx;
    LONG64 cur_epoch;
    dentry_slot_t *ci_match = NULL;

    if (!c || len == 0 || len >= DENTRY_MAX_KEY) return 0;

    h = dentry_hash(norm_path, len, ci);
    sh = &c->shards[dentry_shard_idx(h)];
    bidx = (int)(h & (DENTRY_SHARD_BUCKETS - 1));

    AcquireSRWLockShared(&sh->lock);
    cur_epoch = ReadAcquire64(&c->epoch);
    if (sh->epoch == cur_epoch) {
        for (int16_t idx = sh->buckets[bidx]; idx >= 0; idx = sh->slots[idx].next) {
            dentry_slot_t *s = &sh->slots[idx];
            if (!s->valid || s->hash != h || s->key_len != len) continue;
            if (memcmp(s->key, norm_path, len) == 0) {
                ext2_ino_t found_ino = s->ino;
                s->ref = 1;
                if (found_ino == 0) {
                    ReleaseSRWLockShared(&sh->lock);
                    InterlockedIncrement64(&sh->neg_hits);
                    return -1;
                }
                *out_ino = found_ino;
                if (out_leaf && out_leaf_sz > 0) {
                    const char *slash = strrchr(s->key, '/');
                    const char *leaf = slash ? (slash + 1) : s->key;
                    strcpy_s(out_leaf, out_leaf_sz, leaf);
                }
                ReleaseSRWLockShared(&sh->lock);
                InterlockedIncrement64(&sh->pos_hits);
                return 1;
            }
            if (ci && !s->exact_only && !ci_match && dentry_key_eq(s->key, norm_path, len, 1)) {
                ci_match = s;
            }
        }
        if (ci_match) {
            ext2_ino_t found_ino = ci_match->ino;
            ci_match->ref = 1;
            if (found_ino == 0) {
                ReleaseSRWLockShared(&sh->lock);
                InterlockedIncrement64(&sh->neg_hits);
                return -1;
            }
            *out_ino = found_ino;
            if (out_leaf && out_leaf_sz > 0) {
                const char *slash = strrchr(ci_match->key, '/');
                const char *leaf = slash ? (slash + 1) : ci_match->key;
                strcpy_s(out_leaf, out_leaf_sz, leaf);
            }
            ReleaseSRWLockShared(&sh->lock);
            InterlockedIncrement64(&sh->pos_hits);
            return 1;
        }
    }
    ReleaseSRWLockShared(&sh->lock);
    InterlockedIncrement64(&sh->misses);
    return 0;
}

static void dentry_cache_put(struct ext4_dentry_cache *c, const char *store_path, size_t len,
                             int ci, ext2_ino_t ino, int exact_only) {
    uint64_t h;
    dentry_shard_t *sh;
    int bidx;
    LONG64 cur_epoch;
    int16_t idx = -1;

    if (!c || len == 0 || len >= DENTRY_MAX_KEY) return;

    h = dentry_hash(store_path, len, ci);
    sh = &c->shards[dentry_shard_idx(h)];
    bidx = (int)(h & (DENTRY_SHARD_BUCKETS - 1));

    AcquireSRWLockExclusive(&sh->lock);
    cur_epoch = ReadAcquire64(&c->epoch);
    if (sh->epoch != cur_epoch) {
        dentry_shard_reset_locked(sh, cur_epoch);
    }

    for (int16_t cur = sh->buckets[bidx]; cur >= 0; cur = sh->slots[cur].next) {
        dentry_slot_t *s = &sh->slots[cur];
        if (s->valid && s->hash == h && s->key_len == len &&
            memcmp(s->key, store_path, len) == 0) {
            s->ino = ino;
            s->exact_only = exact_only ? 1 : 0;
            memcpy(s->key, store_path, len + 1);
            s->ref = 1;
            ReleaseSRWLockExclusive(&sh->lock);
            return;
        }
    }

    if (sh->count < DENTRY_SHARD_SLOTS) {
        idx = (int16_t)(sh->count++);
    } else {
        int hand = sh->clock_hand;
        for (;;) {
            if (!sh->slots[hand].valid || sh->slots[hand].ref == 0) {
                idx = (int16_t)hand;
                if (sh->slots[hand].valid) {
                    dentry_unlink_slot(sh, idx);
                }
                sh->clock_hand = (hand + 1) % DENTRY_SHARD_SLOTS;
                break;
            }
            sh->slots[hand].ref = 0;
            hand = (hand + 1) % DENTRY_SHARD_SLOTS;
        }
    }

    {
        dentry_slot_t *s = &sh->slots[idx];
        s->hash = h;
        s->ino = ino;
        s->key_len = (uint16_t)len;
        s->valid = 1;
        s->ref = 1;
        s->exact_only = exact_only ? 1 : 0;
        memcpy(s->key, store_path, len + 1);
        dentry_link_slot(sh, idx, bidx);
    }
    ReleaseSRWLockExclusive(&sh->lock);
}

int ext4_vfs_normalize_path(const char *in_path, char *out_path, size_t out_sz) {
    size_t i = 0, j = 0;
    if (!in_path || !out_path || out_sz < 2) return -EINVAL;

    out_path[j++] = '/';
    while (in_path[i] != '\0' && j + 1 < out_sz) {
        char ch = in_path[i++];
        if (ch == '\\') ch = '/';
        if (ch == '/' && out_path[j - 1] == '/') {
            continue;
        }
        out_path[j++] = ch;
    }
    while (j > 1 && out_path[j - 1] == '/') {
        j--;
    }
    out_path[j] = '\0';
    return 0;
}

int ext4_vfs_split_parent(const char *norm_path, char *out_parent, size_t parent_sz,
                          char *out_name, size_t name_sz) {
    const char *last_slash;
    size_t plen, nlen;

    if (!norm_path || !out_parent || !out_name || norm_path[0] != '/') return -EINVAL;
    if (strcmp(norm_path, "/") == 0) return -EINVAL;

    last_slash = strrchr(norm_path, '/');
    if (!last_slash) return -EINVAL;

    if (last_slash == norm_path) {
        if (parent_sz < 2) return -ENAMETOOLONG;
        strcpy_s(out_parent, parent_sz, "/");
    } else {
        plen = (size_t)(last_slash - norm_path);
        if (plen >= parent_sz) return -ENAMETOOLONG;
        memcpy(out_parent, norm_path, plen);
        out_parent[plen] = '\0';
    }

    nlen = strlen(last_slash + 1);
    if (nlen == 0 || nlen >= name_sz) return -ENAMETOOLONG;
    memcpy(out_name, last_slash + 1, nlen + 1);
    return 0;
}

struct ci_lookup_ctx {
    const char *target;
    ext2_ino_t  found_ino;
    int         found;
    int         match_count;
    char        actual_name[EXT2_NAME_LEN + 1];
};

static int ci_lookup_cb(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent,
                        int offset, int blocksize, char *buf, void *priv_data) {
    struct ci_lookup_ctx *c = (struct ci_lookup_ctx *)priv_data;
    struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)dirent;
    char name_buf[EXT2_NAME_LEN + 1];
    (void)dir;
    (void)entry;
    (void)offset;
    (void)blocksize;
    (void)buf;

    if (de->inode == 0) return 0;
    if (de->name_len > EXT2_NAME_LEN) return 0;
    memcpy(name_buf, de->name, de->name_len);
    name_buf[de->name_len] = '\0';

    if (_stricmp(name_buf, c->target) == 0) {
        c->match_count++;
        /* Deterministic tie-breaker across case collisions: lexicographically smallest
         * UTF-8/ASCII name, then lowest inode number. Do not abort early so collisions are detected. */
        if (!c->found) {
            c->found_ino = de->inode;
            c->found = 1;
            strcpy_s(c->actual_name, sizeof(c->actual_name), name_buf);
        } else {
            int cmp = strcmp(name_buf, c->actual_name);
            if (cmp < 0 || (cmp == 0 && de->inode < c->found_ino)) {
                c->found_ino = de->inode;
                strcpy_s(c->actual_name, sizeof(c->actual_name), name_buf);
            }
        }
    }
    return 0;
}

int ext4_vfs_lookup_locked_ex(ext4_vfs_ctx_t *ctx, const char *norm_path, ext2_ino_t *out_ino,
                              char *out_leaf, size_t out_leaf_sz) {
    errcode_t retval;
    ext2_ino_t cur;
    const char *p;
    size_t norm_len;
    int cache_rc;
    char canon_path[1024];
    size_t canon_len = 0;
    char last_leaf[EXT2_NAME_LEN + 1];

    if (!ctx || !norm_path || !out_ino) return -EINVAL;
    if (strcmp(norm_path, "/") == 0) {
        *out_ino = EXT2_ROOT_INO;
        if (out_leaf && out_leaf_sz > 0) out_leaf[0] = '\0';
        return 0;
    }

    norm_len = strlen(norm_path);
    cache_rc = dentry_cache_lookup(ctx->dentry_cache, norm_path, norm_len,
                                   ctx->case_insensitive, &cur, out_leaf, out_leaf_sz);
    if (cache_rc > 0) {
        *out_ino = cur;
        return 0;
    }
    if (cache_rc < 0) {
        return -ENOENT;
    }

    retval = ext2fs_namei(ctx->fs, EXT2_ROOT_INO, EXT2_ROOT_INO, norm_path, &cur);
    if (retval == 0) {
        dentry_cache_put(ctx->dentry_cache, norm_path, norm_len, ctx->case_insensitive, cur,
                         ctx->case_insensitive ? 1 : 0);
        *out_ino = cur;
        if (out_leaf && out_leaf_sz > 0) {
            const char *slash = strrchr(norm_path, '/');
            const char *leaf = slash ? (slash + 1) : norm_path;
            strcpy_s(out_leaf, out_leaf_sz, leaf);
        }
        return 0;
    }
    if (!ctx->case_insensitive) {
        int err_rc = ext4_errcode_to_errno(retval);
        if (err_rc == -ENOENT) {
            dentry_cache_put(ctx->dentry_cache, norm_path, norm_len, 0, 0, 0);
        }
        return err_rc;
    }

    /* Component-wise exact then case-insensitive fallback, recording canonical disk names */
    cur = EXT2_ROOT_INO;
    p = norm_path + 1;
    canon_path[0] = '\0';
    canon_len = 0;
    last_leaf[0] = '\0';
    {
        int had_collision = 0;
        while (*p) {
            const char *slash = strchr(p, '/');
            char comp[EXT2_NAME_LEN + 1];
            size_t clen = slash ? (size_t)(slash - p) : strlen(p);
            ext2_ino_t next_ino = 0;
            const char *resolved_comp = comp;
            size_t rlen;

            if (clen == 0) {
                p++;
                continue;
            }
            if (clen > EXT2_NAME_LEN) return -ENAMETOOLONG;
            memcpy(comp, p, clen);
            comp[clen] = '\0';

            retval = ext2fs_lookup(ctx->fs, cur, comp, (int)clen, NULL, &next_ino);
            if (retval != 0) {
                struct ci_lookup_ctx c;
                errcode_t dret;
                c.target = comp;
                c.found_ino = 0;
                c.found = 0;
                c.match_count = 0;
                c.actual_name[0] = '\0';
                dret = ext2fs_dir_iterate2(ctx->fs, cur, 0, NULL, ci_lookup_cb, &c);
                if (!c.found) {
                    if (dret == 0) {
                        dentry_cache_put(ctx->dentry_cache, norm_path, norm_len, 1, 0, 0);
                    }
                    return -ENOENT;
                }
                if (c.match_count > 1) {
                    LONG64 n = InterlockedIncrement64(&ctx->stat_ci_collisions);
                    had_collision = 1;
                    if (n <= 16) {
                        ext4_log(EXT4_LOG_WARN,
                                 "Case-insensitive collision for '%s' (%d matches); selected '%s' (use --case-sensitive for exact POSIX access).",
                                 comp, c.match_count, c.actual_name);
                    }
                }
                next_ino = c.found_ino;
                resolved_comp = c.actual_name;
            }

            rlen = strlen(resolved_comp);
            if (canon_len + 1 + rlen < sizeof(canon_path)) {
                canon_path[canon_len++] = '/';
                memcpy(canon_path + canon_len, resolved_comp, rlen);
                canon_len += rlen;
                canon_path[canon_len] = '\0';
            }
            strcpy_s(last_leaf, sizeof(last_leaf), resolved_comp);

            cur = next_ino;
            if (!slash) break;
            p = slash + 1;
        }

        if (had_collision) {
            dentry_cache_put(ctx->dentry_cache, norm_path, norm_len, 1, cur, 1);
        } else if (canon_len > 0) {
            dentry_cache_put(ctx->dentry_cache, canon_path, canon_len, 1, cur, 0);
        }
    }
    *out_ino = cur;
    if (out_leaf && out_leaf_sz > 0) {
        strcpy_s(out_leaf, out_leaf_sz, last_leaf);
    }
    return 0;
}

int ext4_vfs_lookup_locked(ext4_vfs_ctx_t *ctx, const char *norm_path, ext2_ino_t *out_ino) {
    return ext4_vfs_lookup_locked_ex(ctx, norm_path, out_ino, NULL, 0);
}

int ext4_vfs_mount(const ext4_mount_config_t *cfg, ext4_vfs_ctx_t **out_ctx) {
    ext4_vfs_ctx_t *ctx = NULL;
    win32_io_config_t io_cfg;
    ext4_probe_result_t probe;
    uint64_t part_offset = 0;
    uint64_t part_size = 0;
    uint32_t sector_size = 0;
    int read_only;
    int flags;
    errcode_t retval;

    if (!cfg || !cfg->source_path || !out_ctx) return -EINVAL;
    *out_ctx = NULL;

    part_offset = cfg->partition_offset;
    part_size = cfg->partition_size;
    sector_size = cfg->sector_size;
    char part_uuid[48] = {0};

    if (part_offset == 0 && part_size == 0) {
        if (cfg->partition_number > 0) {
            if (pt_resolve_partition_ex(cfg->source_path, cfg->partition_number,
                                        &part_offset, &part_size, &sector_size,
                                        part_uuid, sizeof(part_uuid)) != 0) {
                return -ENOENT;
            }
        } else {
            pt_resolve_partition_ex(cfg->source_path, 0, &part_offset, &part_size, &sector_size,
                                    part_uuid, sizeof(part_uuid));
        }
    }

    if (sector_size == 0) sector_size = cfg->sector_size ? cfg->sector_size : 4096;

    memset(&probe, 0, sizeof(probe));
    if (ext4_probe_partition(cfg->source_path, part_offset, part_size, sector_size, &probe) != 0) {
        return -EINVAL;
    }

    if (probe.feature_incompat & EXT3_FEATURE_INCOMPAT_JOURNAL_DEV) {
        ext4_log(EXT4_LOG_ERROR,
                 "Source '%s' is an external JBD2 journal device (journal_dev) and cannot be mounted.",
                 cfg->source_path);
        return -ENOTSUP;
    }
    if (probe.unsupported_incompat != 0) {
        ext4_log(EXT4_LOG_ERROR,
                 "Unsupported ext4 incompat features (0x%08X) on '%s'. Cannot mount safely.",
                 probe.unsupported_incompat, cfg->source_path);
        return -ENOTSUP;
    }

    int has_overlay = (cfg->overlay_upper_dir && cfg->overlay_upper_dir[0] != '\0');
    int lower_ro = has_overlay ? 1 : cfg->read_only;
    read_only = cfg->read_only;
    int dirty = (probe.journal_state == EXT4_JOURNAL_NEEDS_RECOVERY ||
                 probe.journal_state == EXT4_JOURNAL_HAS_ERRORS);
    int fell_back = 0;

    if (cfg->enable_direct_io && cfg->disable_direct_io) {
        ext4_log(EXT4_LOG_ERROR,
                 "Cannot specify both --direct-io and --no-direct-io.");
        return -EINVAL;
    }

    if (cfg->enable_direct_io && !lower_ro) {
        ext4_log(EXT4_LOG_ERROR,
                 "Direct extent I/O (--direct-io) is only supported in Read-Only (default) or --overlay mode.");
        return -EINVAL;
    }

    if (cfg->enable_rw_fast_io && lower_ro) {
        ext4_log(EXT4_LOG_ERROR,
                 "--rw-fast-io is only supported in direct Read-Write (--rw) mode.");
        return -EINVAL;
    }

    if (!lower_ro) {
        if (cfg->ignore_csum_errors) {
            ext4_log(EXT4_LOG_ERROR,
                     "--ignore-csum-errors is only allowed in Read-Only or --overlay mode (refusing RW).");
            return -EINVAL;
        }
        if (probe.feature_incompat & EXT4_FEATURE_INCOMPAT_ENCRYPT) {
            ext4_log(EXT4_LOG_ERROR,
                     "fscrypt (encrypt) is enabled on '%s'; Read-Write (--rw) is blocked. Use Read-Only or --overlay.",
                     cfg->source_path);
            return -ENOTSUP;
        }
        if (probe.feature_ro_compat & EXT4_FEATURE_RO_COMPAT_VERITY) {
            ext4_log(EXT4_LOG_ERROR,
                     "fs-verity (verity) is enabled on '%s'; Read-Write (--rw) is blocked to protect Merkle trees. Use Read-Only or --overlay.",
                     cfg->source_path);
            return -ENOTSUP;
        }
        if (ext4_probe_has_external_journal(&probe)) {
            ext4_log(EXT4_LOG_ERROR,
                     "External journal device (s_journal_dev=0x%X) on '%s'; Read-Write (--rw) is blocked. Replay on Linux or use Read-Only/--overlay.",
                     probe.journal_dev, cfg->source_path);
            return -ENOTSUP;
        }
        if (probe.unsupported_ro_compat != 0) {
            ext4_log(EXT4_LOG_ERROR,
                     "Unsupported ext4 ro_compat features (0x%08X) on '%s'; Read-Write (--rw) is blocked.",
                     probe.unsupported_ro_compat, cfg->source_path);
            return -ENOTSUP;
        }
        if (probe.feature_ro_compat & EXT4_FEATURE_RO_COMPAT_READONLY) {
            ext4_log(EXT4_LOG_ERROR,
                     "Volume '%s' has RO_COMPAT_READONLY set; Read-Write (--rw) is blocked.",
                     cfg->source_path);
            return -EROFS;
        }
        if (probe.feature_ro_compat & EXT4_FEATURE_RO_COMPAT_SHARED_BLOCKS) {
            ext4_log(EXT4_LOG_ERROR,
                     "Volume '%s' uses shared_blocks; Read-Write (--rw) is blocked.",
                     cfg->source_path);
            return -ENOTSUP;
        }
    } else {
        if (probe.feature_incompat & EXT4_FEATURE_INCOMPAT_ENCRYPT) {
            ext4_log(EXT4_LOG_WARN,
                     "Volume '%s' uses fscrypt (encrypt): encrypted files/names will appear as ciphertext or fail without Linux keys.",
                     cfg->source_path);
        }
        if (probe.feature_ro_compat & EXT4_FEATURE_RO_COMPAT_VERITY) {
            ext4_log(EXT4_LOG_WARN,
                     "Volume '%s' uses fs-verity: Read-Only mount does not verify fs-verity Merkle digests.",
                     cfg->source_path);
        }
        if (ext4_probe_has_external_journal(&probe)) {
            ext4_log(EXT4_LOG_WARN,
                     "Volume '%s' references an external journal (s_journal_dev=0x%X), which is not attached on Windows.",
                     cfg->source_path, probe.journal_dev);
        }
        if (cfg->ignore_csum_errors) {
            ext4_log(EXT4_LOG_WARN,
                     "Mounting '%s' with --ignore-csum-errors (metadata CRC32C checks bypassed in read-only lower).",
                     cfg->source_path);
        }
    }

    if (!lower_ro && dirty && !cfg->force_rw_on_dirty) {
        if (cfg->ro_fallback_on_dirty) {
            ext4_log(EXT4_LOG_WARN,
                     "Dirty journal / FS error detected (%s) on '%s'; falling back to READ-ONLY mount.",
                     ext4_journal_state_name(probe.journal_state), cfg->source_path);
            lower_ro = 1;
            read_only = 1;
            fell_back = 1;
        } else {
            ext4_log(EXT4_LOG_ERROR,
                     "Refusing RW mount on dirty ext4 volume (%s) without --force-dirty.",
                     ext4_journal_state_name(probe.journal_state));
            return -EROFS;
        }
    }

    ctx = (ext4_vfs_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return -ENOMEM;

    InitializeSRWLock(&ctx->rwlock);
    InitializeCriticalSection(&ctx->fs_lock);
    ctx->read_only = read_only;
    ctx->lower_read_only = lower_ro;
    ctx->dirty_detected = dirty;
    ctx->fell_back_to_ro = fell_back;
    ctx->case_insensitive = cfg->case_insensitive;
    /* Direct Extent I/O is enabled by default whenever Lower ext4 is Read-Only unless explicitly disabled */
    ctx->enable_direct_io = (lower_ro && !cfg->disable_direct_io) ? 1 : 0;
    ctx->enable_rw_fast_io = (!lower_ro && cfg->enable_rw_fast_io) ? 1 : 0;
    ctx->has_file_info_timeout = cfg->has_file_info_timeout ? 1 : 0;
    ctx->file_info_timeout = cfg->file_info_timeout;
    ctx->journal_state = probe.journal_state;
    ctx->partition_offset = part_offset;
    ctx->partition_size = part_size;
    strcpy_s(ctx->partition_uuid, sizeof(ctx->partition_uuid), part_uuid);
    strcpy_s(ctx->source_path, sizeof(ctx->source_path), cfg->source_path);
    strcpy_s(ctx->volume_label, sizeof(ctx->volume_label), probe.volume_label);

    memset(&io_cfg, 0, sizeof(io_cfg));
    io_cfg.partition_offset_bytes = part_offset;
    io_cfg.partition_size_bytes = part_size;
    io_cfg.sector_size = sector_size;
    io_cfg.max_cache_blocks = cfg->max_cache_blocks;
    io_cfg.read_only = lower_ro;

    flags = EXT2_FLAG_64BITS;
    if (!lower_ro) {
        flags |= EXT2_FLAG_RW;
    }
    if (cfg->ignore_csum_errors) {
        flags |= EXT2_FLAG_IGNORE_CSUM_ERRORS;
    }
    if (cfg->force_rw_on_dirty) {
        flags |= EXT2_FLAG_FORCE;
    }

    win32_io_set_next_config(&io_cfg);
    retval = ext2fs_open2(cfg->source_path, NULL, flags, 0, 0, win32_io_manager, &ctx->fs);
    if (retval) {
        DeleteCriticalSection(&ctx->fs_lock);
        free(ctx);
        return ext4_errcode_to_errno(retval);
    }

    (void)ext2fs_create_inode_cache(ctx->fs, 64);

    retval = ext2fs_read_bitmaps(ctx->fs);
    if (retval) {
        ext2fs_free(ctx->fs);
        DeleteCriticalSection(&ctx->fs_lock);
        free(ctx);
        return ext4_errcode_to_errno(retval);
    }

    if (ctx->enable_direct_io && ctx->lower_read_only) {
        ctx->extent_cache = ext4_extent_cache_create(4096);
    }
    ctx->dentry_cache = ext4_dentry_cache_create();
    if (has_overlay) {
        int orc = overlay_vfs_create(ctx, cfg->overlay_upper_dir, &ctx->overlay);
        if (orc != 0) {
            ext4_dentry_cache_destroy(ctx->dentry_cache);
            ext4_extent_cache_destroy(ctx->extent_cache);
            ext2fs_close_free(&ctx->fs);
            DeleteCriticalSection(&ctx->fs_lock);
            free(ctx);
            return orc;
        }
    }

    *out_ctx = ctx;
    return 0;
}

int ext4_vfs_unmount(ext4_vfs_ctx_t *ctx) {
    errcode_t retval = 0;
    if (!ctx) return 0;

    if (ctx->overlay) {
        overlay_vfs_destroy(ctx->overlay);
        ctx->overlay = NULL;
    }
    if (ctx->dentry_cache) {
        ext4_dentry_cache_destroy(ctx->dentry_cache);
        ctx->dentry_cache = NULL;
    }
    if (ctx->extent_cache) {
        ext4_extent_cache_destroy(ctx->extent_cache);
        ctx->extent_cache = NULL;
    }

    AcquireSRWLockExclusive(&ctx->rwlock);
    if (ctx->fs) {
        retval = ext2fs_close_free(&ctx->fs);
    }
    ReleaseSRWLockExclusive(&ctx->rwlock);
    DeleteCriticalSection(&ctx->fs_lock);
    free(ctx);
    return ext4_errcode_to_errno(retval);
}

int ext4_vfs_mkfs(const char *path_utf8, uint64_t size_bytes, const char *label) {
    wchar_t wpath[1024];
    HANDLE h;
    LARGE_INTEGER li;
    struct ext2_super_block param;
    win32_io_config_t io_cfg;
    ext2_filsys fs = NULL;
    errcode_t retval;
    unsigned int blocksize;
    unsigned int log_bsize;
    blk64_t blocks_count;
    ext2_ino_t r;
    dgrp_t g;
    uint8_t fs_uuid[16] = {0};
    uint32_t hash_seed[4] = {0};
    char vol_label[16];

    if (!path_utf8 || size_bytes < (1024ULL * 1024ULL)) {
        return -EINVAL;
    }
    if (ext4_utf8_to_wide(path_utf8, wpath, sizeof(wpath) / sizeof(wpath[0])) != 0) {
        return -EINVAL;
    }

    blocksize = (size_bytes >= 4ULL * 1024ULL * 1024ULL) ? 4096U : 1024U;
    log_bsize = (blocksize == 4096U) ? 2U : 0U;
    size_bytes = (size_bytes / blocksize) * blocksize;
    blocks_count = size_bytes / blocksize;

    h = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return -EIO;
    }
    li.QuadPart = (LONGLONG)size_bytes;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN) || !SetEndOfFile(h)) {
        CloseHandle(h);
        return -EIO;
    }
    CloseHandle(h);

    memset(&param, 0, sizeof(param));
    param.s_rev_level = EXT2_DYNAMIC_REV;
    param.s_log_block_size = log_bsize;
    param.s_inode_size = 256;
    param.s_desc_size = EXT2_MIN_DESC_SIZE_64BIT;
    param.s_feature_compat = EXT2_FEATURE_COMPAT_DIR_INDEX | EXT2_FEATURE_COMPAT_EXT_ATTR;
    param.s_feature_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE |
                               EXT3_FEATURE_INCOMPAT_EXTENTS |
                               EXT4_FEATURE_INCOMPAT_64BIT;
    param.s_feature_ro_compat = EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER |
                                EXT2_FEATURE_RO_COMPAT_LARGE_FILE |
                                EXT4_FEATURE_RO_COMPAT_HUGE_FILE |
                                EXT4_FEATURE_RO_COMPAT_DIR_NLINK |
                                EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE |
                                EXT4_FEATURE_RO_COMPAT_METADATA_CSUM;
    ext2fs_blocks_count_set(&param, blocks_count);
    {
        static volatile LONG64 s_uuid_seq = 0;
        LARGE_INTEGER qpc;
        ULONGLONG ticks = GetTickCount64();
        LONG64 seq = InterlockedIncrement64(&s_uuid_seq);
        uint64_t a, b, c, d;
        QueryPerformanceCounter(&qpc);
        a = (uint64_t)qpc.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32) ^ (uint64_t)seq;
        b = (uint64_t)ticks ^ ((uint64_t)seq * 0x9E3779B97F4A7C15ULL);
        a = (a ^ (a >> 30)) * 0xBF58476D1CE4E5B9ULL;
        a = (a ^ (a >> 27)) * 0x94D049BB133111EBULL;
        a ^= (a >> 31);
        b = (b ^ (b >> 30)) * 0xBF58476D1CE4E5B9ULL;
        b = (b ^ (b >> 27)) * 0x94D049BB133111EBULL;
        b ^= (b >> 31);
        c = a ^ 0xA5A5A5A55A5A5A5AULL;
        d = b ^ 0x5A5A5A5AA5A5A5A5ULL;
        {
            uint64_t uuid_words[2];
            uint64_t seed_words[2];

            uuid_words[0] = a;
            uuid_words[1] = b;
            memcpy(fs_uuid, uuid_words, sizeof(fs_uuid));
            fs_uuid[6] = (uint8_t)((fs_uuid[6] & 0x0F) | 0x40); /* UUID v4 */
            fs_uuid[8] = (uint8_t)((fs_uuid[8] & 0x3F) | 0x80); /* RFC 4122 variant */
            seed_words[0] = c;
            seed_words[1] = d;
            memcpy(hash_seed, seed_words, sizeof(hash_seed));
        }
    }

    memset(vol_label, 0, sizeof(vol_label));
    if (label && *label) {
        size_t llen = strlen(label);
        if (llen > sizeof(vol_label)) {
            llen = sizeof(vol_label);
            while (llen > 0 && (((unsigned char)label[llen]) & 0xC0) == 0x80) {
                llen--;
            }
        }
        memcpy(vol_label, label, llen);
    }

    memset(&io_cfg, 0, sizeof(io_cfg));
    io_cfg.partition_size_bytes = size_bytes;
    io_cfg.sector_size = 4096;
    win32_io_set_next_config(&io_cfg);

    retval = ext2fs_initialize(path_utf8, EXT2_FLAG_64BITS | EXT2_FLAG_RW, &param, win32_io_manager, &fs);
    if (retval) {
        return ext4_errcode_to_errno(retval);
    }

    memcpy(fs->super->s_uuid, fs_uuid, sizeof(fs->super->s_uuid));
    memcpy(fs->super->s_volume_name, vol_label, sizeof(fs->super->s_volume_name));
    fs->super->s_def_hash_version = EXT2_HASH_HALF_MD4;
    memcpy(fs->super->s_hash_seed, hash_seed, sizeof(fs->super->s_hash_seed));
    fs->super->s_checksum_type = EXT2_CRC32C_CHKSUM;
    ext2fs_init_csum_seed(fs);

    retval = ext2fs_allocate_tables(fs);
    if (retval) {
        ext2fs_free(fs);
        return ext4_errcode_to_errno(retval);
    }

    /* Mark reserved system inodes 1..s_first_ino-1 except ROOT_INO (handled by mkdir below) */
    for (r = 1; r < fs->super->s_first_ino; r++) {
        if (r == EXT2_ROOT_INO) continue;
        ext2fs_inode_alloc_stats2(fs, r, 1, 0);
    }

    /* Create root directory (inode 2) */
    retval = ext2fs_mkdir(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, NULL);
    if (retval) {
        ext2fs_free(fs);
        return ext4_errcode_to_errno(retval);
    }

    /* Create lost+found directory in root */
    retval = ext2fs_mkdir(fs, EXT2_ROOT_INO, 0, "lost+found");
    if (retval) {
        ext2fs_free(fs);
        return ext4_errcode_to_errno(retval);
    }

    for (g = 0; g < fs->group_desc_count; g++) {
        ext2fs_group_desc_csum_set(fs, g);
    }
    ext2fs_mark_super_dirty(fs);

    retval = ext2fs_close_free(&fs);
    return ext4_errcode_to_errno(retval);
}
