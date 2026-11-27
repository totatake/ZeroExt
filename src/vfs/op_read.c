#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include "../extent/extent_map.h"
#include "../overlay/overlay_vfs.h"
#include "../trace/perfetto_trace.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define STD_READ_BATCH_RUNS   64U
#define STD_READ_MAX_RUN_SIZE (4U * 1024U * 1024U)
#define STD_READ_MAX_IO_BLKS  1024U

typedef struct std_read_run {
    uint64_t pblk;
    uint32_t dst_off;
    uint32_t blk_off;
    uint32_t byte_len;
    int      is_hole;
} std_read_run_t;

static int is_extent_end_err(errcode_t retval) {
    if (retval == EXT2_ET_EXTENT_NOT_FOUND) return 1;
#ifdef EXT2_ET_EXTENT_NO_NEXT
    if (retval == EXT2_ET_EXTENT_NO_NEXT) return 1;
#endif
    return 0;
}

static int ext4_vfs_standard_read_fallback_locked(ext4_vfs_ctx_t *ctx, ext2_ino_t ino,
                                                  void *buf, uint64_t offset,
                                                  uint32_t length, uint32_t *out_read) {
    ext2_file_t file = NULL;
    errcode_t retval;
    unsigned int got = 0;

    retval = ext2fs_file_open(ctx->fs, ino, 0, &file);
    if (retval == 0) {
        struct ext2_inode *inode = ext2fs_file_get_inode(file);
        if (LINUX_S_ISDIR(inode->i_mode)) {
            retval = EISDIR;
        } else {
            retval = ext2fs_file_llseek(file, offset, EXT2_SEEK_SET, NULL);
            if (retval == 0) {
                retval = ext2fs_file_read(file, buf, length, &got);
            }
        }
        ext2fs_file_close(file);
    }
    if (retval != 0) return ext4_errcode_to_errno(retval);
    *out_read = (uint32_t)got;
    return 0;
}

static int ext4_vfs_e2fsprogs_read(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, void *buf,
                                   uint64_t offset, uint32_t length, uint32_t *out_read) {
    int rc;
    uint64_t t_vfs = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    ztrace_lock_guard_t lg = ztrace_srw_acquire_shared(
        &ctx->rwlock, "ctx->rwlock wait (S)", "ctx->rwlock hold (S)");

    EnterCriticalSection(&ctx->fs_lock);
    rc = ext4_vfs_standard_read_fallback_locked(ctx, ino, buf, offset, length, out_read);
    LeaveCriticalSection(&ctx->fs_lock);
    ztrace_srw_release_shared(&ctx->rwlock, &lg);

    if (t_vfs) {
        uint64_t end = ztrace_now_qpc();
        ztrace_record_complete("vfs", "ext2fs_file_read", t_vfs, end - t_vfs,
                               (uint64_t)ino, (uint64_t)(*out_read), (int64_t)rc, ZTRACE_ARGS_RW_OP);
    }
    return rc;
}

static int ext4_vfs_standard_read_extents(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, void *buf,
                                          uint64_t offset, uint32_t length, uint32_t *out_read) {
    struct ext2_inode inode;
    errcode_t retval;
    uint64_t file_size;
    uint64_t cur_pos;
    uint64_t end_pos;
    uint32_t want;
    uint32_t bsz;
    io_channel io;
    uint8_t stack_blk[4096];
    uint8_t *heap_blk = NULL;
    uint8_t *blk_buf;
    int rc = 0;
    int fs_locked = 0;
    uint64_t t_vfs = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    ztrace_lock_guard_t lg = ztrace_srw_acquire_shared(
        &ctx->rwlock, "ctx->rwlock wait (S)", "ctx->rwlock hold (S)");

    EnterCriticalSection(&ctx->fs_lock);
    fs_locked = 1;

    memset(&inode, 0, sizeof(inode));
    retval = ext2fs_read_inode(ctx->fs, ino, &inode);
    if (retval != 0) {
        rc = ext4_errcode_to_errno(retval);
        goto out;
    }
    if (LINUX_S_ISDIR(inode.i_mode)) {
        rc = -EISDIR;
        goto out;
    }

    if (!(inode.i_flags & EXT4_EXTENTS_FL) ||
        (inode.i_flags & EXT4_INLINE_DATA_FL) ||
        LINUX_S_ISLNK(inode.i_mode)) {
        rc = ext4_vfs_standard_read_fallback_locked(ctx, ino, buf, offset, length, out_read);
        goto out;
    }

    file_size = EXT2_I_SIZE(&inode);
    if (offset >= file_size) {
        *out_read = 0;
        rc = 0;
        goto out;
    }

    {
        uint64_t rem_file = file_size - offset;
        want = ((uint64_t)length < rem_file) ? length : (uint32_t)rem_file;
    }
    if (want == 0) {
        *out_read = 0;
        rc = 0;
        goto out;
    }

    bsz = (uint32_t)ctx->fs->blocksize;
    io = ctx->fs->io;
    if (bsz <= sizeof(stack_blk)) {
        blk_buf = stack_blk;
    } else {
        heap_blk = (uint8_t *)malloc(bsz);
        if (!heap_blk) {
            rc = -ENOMEM;
            goto out;
        }
        blk_buf = heap_blk;
    }

    cur_pos = offset;
    end_pos = offset + (uint64_t)want;

    while (cur_pos < end_pos) {
        std_read_run_t runs[STD_READ_BATCH_RUNS];
        uint32_t run_count = 0;
        ext2_extent_handle_t ehandle = NULL;

        if (!fs_locked) {
            EnterCriticalSection(&ctx->fs_lock);
            fs_locked = 1;
        }

        retval = ext2fs_extent_open2(ctx->fs, ino, &inode, &ehandle);
        if (retval != 0) {
            rc = ext4_errcode_to_errno(retval);
            goto out;
        }

        while (cur_pos < end_pos && run_count < STD_READ_BATCH_RUNS) {
            blk64_t lblk = cur_pos / bsz;
            uint32_t in_blk = (uint32_t)(cur_pos % bsz);
            uint64_t rem = end_pos - cur_pos;
            uint64_t pblk = 0;
            uint32_t take = 0;
            int is_hole = 0;
            struct ext2fs_extent ext;

            retval = ext2fs_extent_goto(ehandle, lblk);
            if (retval == 0) {
                retval = ext2fs_extent_get(ehandle, EXT2_EXTENT_CURRENT, &ext);
                if (retval != 0) {
                    ext2fs_extent_free(ehandle);
                    rc = ext4_errcode_to_errno(retval);
                    goto out;
                }
                {
                    blk64_t off_in_ext = lblk - ext.e_lblk;
                    blk64_t avail_blks = (blk64_t)ext.e_len - off_in_ext;
                    uint64_t max_bytes = avail_blks * (uint64_t)bsz - (uint64_t)in_blk;
                    uint64_t cap_bytes = (uint64_t)STD_READ_MAX_RUN_SIZE - (uint64_t)in_blk;
                    uint64_t take64 = (rem < max_bytes) ? rem : max_bytes;
                    if (take64 > cap_bytes) take64 = cap_bytes;
                    take = (uint32_t)take64;
                    if (ext.e_flags & EXT2_EXTENT_FLAGS_UNINIT) {
                        is_hole = 1;
                        pblk = 0;
                    } else {
                        is_hole = 0;
                        pblk = ext.e_pblk + off_in_ext;
                    }
                }
            } else if (retval == EXT2_ET_EXTENT_NOT_FOUND) {
                uint64_t hole_bytes = rem;
                retval = ext2fs_extent_get(ehandle, EXT2_EXTENT_CURRENT, &ext);
                if (retval == EXT2_ET_NO_CURRENT_NODE) {
                    /* Empty extent tree: rest of range is hole */
                } else if (retval != 0) {
                    ext2fs_extent_free(ehandle);
                    rc = ext4_errcode_to_errno(retval);
                    goto out;
                } else if (ext.e_lblk > lblk) {
                    blk64_t hole_blks = ext.e_lblk - lblk;
                    hole_bytes = hole_blks * (uint64_t)bsz - (uint64_t)in_blk;
                } else {
                    struct ext2fs_extent next_ext;
                    retval = ext2fs_extent_get(ehandle, EXT2_EXTENT_NEXT_LEAF, &next_ext);
                    if (retval == 0 && next_ext.e_lblk > lblk) {
                        blk64_t hole_blks = next_ext.e_lblk - lblk;
                        hole_bytes = hole_blks * (uint64_t)bsz - (uint64_t)in_blk;
                    } else if (retval != 0 && !is_extent_end_err(retval)) {
                        ext2fs_extent_free(ehandle);
                        rc = ext4_errcode_to_errno(retval);
                        goto out;
                    }
                }
                {
                    uint64_t take64 = (rem < hole_bytes) ? rem : hole_bytes;
                    if (take64 > STD_READ_MAX_RUN_SIZE) take64 = STD_READ_MAX_RUN_SIZE;
                    take = (uint32_t)take64;
                    is_hole = 1;
                    pblk = 0;
                }
            } else {
                ext2fs_extent_free(ehandle);
                rc = ext4_errcode_to_errno(retval);
                goto out;
            }

            if (take == 0) break;

            /* Merge adjacent contiguous runs when aligned to block boundary */
            if (run_count > 0) {
                std_read_run_t *prev = &runs[run_count - 1];
                uint32_t prev_end_in_blk = prev->blk_off + prev->byte_len;
                if ((prev_end_in_blk % bsz) == 0 && in_blk == 0 &&
                    prev->is_hole == is_hole &&
                    prev->byte_len + take <= STD_READ_MAX_RUN_SIZE &&
                    (is_hole || prev->pblk + (blk64_t)(prev_end_in_blk / bsz) == pblk)) {
                    prev->byte_len += take;
                    cur_pos += take;
                    continue;
                }
            }

            runs[run_count].pblk = pblk;
            runs[run_count].dst_off = (uint32_t)(cur_pos - offset);
            runs[run_count].blk_off = in_blk;
            runs[run_count].byte_len = take;
            runs[run_count].is_hole = is_hole;
            run_count++;
            cur_pos += take;
        }

        ext2fs_extent_free(ehandle);
        LeaveCriticalSection(&ctx->fs_lock);
        fs_locked = 0;

        /* Execute I/O concurrently across threads outside fs_lock */
        for (uint32_t r = 0; r < run_count; r++) {
            const std_read_run_t *run = &runs[r];
            uint8_t *dst = (uint8_t *)buf + run->dst_off;
            uint32_t rem_r = run->byte_len;
            uint32_t off_in_blk = run->blk_off;
            blk64_t cur_pblk = run->pblk;

            if (run->is_hole) {
                memset(dst, 0, rem_r);
                continue;
            }

            if (off_in_blk != 0 || rem_r < bsz) {
                uint32_t c = bsz - off_in_blk;
                if (c > rem_r) c = rem_r;
                retval = io_channel_read_blk64(io, cur_pblk, 1, blk_buf);
                if (retval != 0) {
                    rc = ext4_errcode_to_errno(retval);
                    goto out;
                }
                memcpy(dst, blk_buf + off_in_blk, c);
                dst += c;
                rem_r -= c;
                cur_pblk++;
            }

            while (rem_r >= bsz) {
                uint32_t full_blks = rem_r / bsz;
                uint32_t batch = (full_blks > STD_READ_MAX_IO_BLKS) ? STD_READ_MAX_IO_BLKS : full_blks;
                retval = io_channel_read_blk64(io, cur_pblk, (int)batch, dst);
                if (retval != 0) {
                    rc = ext4_errcode_to_errno(retval);
                    goto out;
                }
                dst += (size_t)batch * bsz;
                rem_r -= batch * bsz;
                cur_pblk += batch;
            }

            if (rem_r > 0) {
                retval = io_channel_read_blk64(io, cur_pblk, 1, blk_buf);
                if (retval != 0) {
                    rc = ext4_errcode_to_errno(retval);
                    goto out;
                }
                memcpy(dst, blk_buf, rem_r);
            }
        }

        if (run_count == 0) break;
    }

    *out_read = (uint32_t)(cur_pos - offset);
    rc = 0;

out:
    if (fs_locked) {
        LeaveCriticalSection(&ctx->fs_lock);
    }
    ztrace_srw_release_shared(&ctx->rwlock, &lg);
    free(heap_blk);
    if (t_vfs) {
        uint64_t end = ztrace_now_qpc();
        ztrace_record_complete("vfs", "ext2fs_file_read", t_vfs, end - t_vfs,
                               (uint64_t)ino, (uint64_t)(*out_read), (int64_t)rc, ZTRACE_ARGS_RW_OP);
    }
    return rc;
}

int ext4_vfs_lower_read(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, void *buf, uint64_t offset,
                        uint32_t length, uint32_t *out_read) {
    if (!ctx || !out_read || ino == 0) return -EINVAL;
    *out_read = 0;
    if (length == 0) return 0;
    if (!buf) return -EINVAL;

    if (ctx->enable_direct_io && ctx->lower_read_only && ctx->extent_cache) {
        ext4_extent_map_t *map = ext4_extent_cache_get(ctx->extent_cache, ino);
        int rc = 0;
        if (!map) {
            ztrace_lock_guard_t lg = ztrace_srw_acquire_shared(
                &ctx->rwlock, "ctx->rwlock wait (S)", "ctx->rwlock hold (S)");
            EnterCriticalSection(&ctx->fs_lock);
            rc = ext4_extent_extract(ctx->fs, ino, &map);
            LeaveCriticalSection(&ctx->fs_lock);
            ztrace_srw_release_shared(&ctx->rwlock, &lg);
            if (rc == 0 && map) {
                ext4_extent_cache_put(ctx->extent_cache, map);
            }
        }
        if (rc == 0 && map) {
            rc = ext4_extent_pread(ctx->fs->io, map, buf, offset, length, out_read);
            ext4_extent_map_release(map);
            return rc;
        }
        if (map) {
            ext4_extent_map_release(map);
        }
        if (rc == -EISDIR) {
            return rc;
        }
    }

    if (ctx->enable_rw_fast_io && !ctx->lower_read_only) {
        return ext4_vfs_standard_read_extents(ctx, ino, buf, offset, length, out_read);
    }

    return ext4_vfs_e2fsprogs_read(ctx, ino, buf, offset, length, out_read);
}

int ext4_vfs_read(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, void *buf, uint64_t offset,
                  uint32_t length, uint32_t *out_read) {
    if (ctx && ctx->overlay) {
        return overlay_vfs_read(ctx, ino, buf, offset, length, out_read);
    }
    return ext4_vfs_lower_read(ctx, ino, buf, offset, length, out_read);
}

int ext4_vfs_read_handle(ext4_vfs_ctx_t *ctx, ext4_file_handle_t *handle, void *buf,
                         uint64_t offset, uint32_t length, uint32_t *out_read) {
    if (!ctx || !handle || !out_read) return -EINVAL;
    *out_read = 0;
    if (length == 0) return 0;
    if (!buf) return -EINVAL;

    if (!ctx->overlay && ctx->enable_direct_io && ctx->lower_read_only &&
        ctx->extent_cache && !handle->is_dir) {
        ext4_extent_map_t *map = (ext4_extent_map_t *)ReadPointerAcquire(
            (PVOID volatile *)&handle->cached_extents);
        int rc = 0;
        if (!map) {
            map = ext4_extent_cache_get(ctx->extent_cache, handle->ino);
            if (!map) {
                ztrace_lock_guard_t lg = ztrace_srw_acquire_shared(
                    &ctx->rwlock, "ctx->rwlock wait (S)", "ctx->rwlock hold (S)");
                EnterCriticalSection(&ctx->fs_lock);
                rc = ext4_extent_extract(ctx->fs, handle->ino, &map);
                LeaveCriticalSection(&ctx->fs_lock);
                ztrace_srw_release_shared(&ctx->rwlock, &lg);
                if (rc == 0 && map) {
                    ext4_extent_cache_put(ctx->extent_cache, map);
                }
            }
            if (rc == 0 && map) {
                ext4_extent_map_t *prev = (ext4_extent_map_t *)InterlockedCompareExchangePointer(
                    (PVOID volatile *)&handle->cached_extents, map, NULL);
                if (prev != NULL) {
                    ext4_extent_map_release(map);
                    map = prev;
                }
            } else if (map) {
                ext4_extent_map_release(map);
                map = NULL;
            }
        }
        if (rc == 0 && map) {
            return ext4_extent_pread(ctx->fs->io, map, buf, offset, length, out_read);
        }
        if (rc == -EISDIR) {
            return rc;
        }
    }

    return ext4_vfs_read(ctx, handle->ino, buf, offset, length, out_read);
}

int ext4_vfs_get_extents(ext4_vfs_ctx_t *ctx, const char *path, ext4_extent_map_t **out_map) {
    char norm[1024];
    ext2_ino_t ino = 0;
    int rc;

    if (!ctx || !path || !out_map) return -EINVAL;
    *out_map = NULL;

    rc = ext4_vfs_normalize_path(path, norm, sizeof(norm));
    if (rc != 0) return rc;

    {
        ztrace_lock_guard_t lg = ztrace_srw_acquire_shared(
            &ctx->rwlock, "ctx->rwlock wait (S)", "ctx->rwlock hold (S)");
        EnterCriticalSection(&ctx->fs_lock);
        rc = ext4_vfs_lookup_locked(ctx, norm, &ino);
        if (rc == 0) {
            rc = ext4_extent_extract(ctx->fs, ino, out_map);
        }
        LeaveCriticalSection(&ctx->fs_lock);
        ztrace_srw_release_shared(&ctx->rwlock, &lg);
    }
    return rc;
}
