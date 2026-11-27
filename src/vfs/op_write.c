#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include "../overlay/overlay_vfs.h"
#include "../trace/perfetto_trace.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define WRITE_MAX_IO_BLKS 1024U

static int is_extent_end_err_w(errcode_t retval) {
    if (retval == EXT2_ET_EXTENT_NOT_FOUND) return 1;
#ifdef EXT2_ET_EXTENT_NO_NEXT
    if (retval == EXT2_ET_EXTENT_NO_NEXT) return 1;
#endif
    return 0;
}

static errcode_t find_next_leaf_after(ext2_extent_handle_t ehandle,
                                      struct ext2fs_extent *out_next) {
    struct ext2fs_extent ext;
    errcode_t rc = ext2fs_extent_get(ehandle, EXT2_EXTENT_NEXT_SIB, &ext);
    if (rc == 0) {
        *out_next = ext;
        return 0;
    }
    if (rc != EXT2_ET_EXTENT_NO_NEXT) {
        return rc;
    }
    for (;;) {
        rc = ext2fs_extent_get(ehandle, EXT2_EXTENT_UP, &ext);
        if (rc == EXT2_ET_EXTENT_NO_UP) {
            return EXT2_ET_EXTENT_NO_NEXT;
        }
        if (rc != 0) {
            return rc;
        }
        rc = ext2fs_extent_get(ehandle, EXT2_EXTENT_NEXT_SIB, &ext);
        if (rc == 0) {
            while (!(ext.e_flags & EXT2_EXTENT_FLAGS_LEAF)) {
                rc = ext2fs_extent_get(ehandle, EXT2_EXTENT_DOWN, &ext);
                if (rc != 0) return rc;
            }
            *out_next = ext;
            return 0;
        }
        if (rc != EXT2_ET_EXTENT_NO_NEXT) {
            return rc;
        }
    }
}

static errcode_t write_data_blocks(ext2_filsys fs, blk64_t pblk, const uint8_t *src,
                                   uint32_t in_blk, uint32_t byte_len,
                                   int already_initialized, uint8_t *blk_buf) {
    uint32_t bsz = (uint32_t)fs->blocksize;
    uint32_t rem = byte_len;
    blk64_t cur_pblk = pblk;
    errcode_t retval = 0;

    if (rem == 0) return 0;

    if (in_blk != 0 || rem < bsz) {
        uint32_t c = bsz - in_blk;
        if (c > rem) c = rem;
        if (already_initialized) {
            retval = io_channel_read_blk64(fs->io, cur_pblk, 1, blk_buf);
            if (retval != 0) return retval;
        } else {
            memset(blk_buf, 0, bsz);
        }
        memcpy(blk_buf + in_blk, src, c);
        retval = io_channel_write_blk64(fs->io, cur_pblk, 1, blk_buf);
        if (retval != 0) return retval;
        src += c;
        rem -= c;
        cur_pblk++;
    }

    while (rem >= bsz) {
        uint32_t full_blks = rem / bsz;
        uint32_t batch = (full_blks > WRITE_MAX_IO_BLKS) ? WRITE_MAX_IO_BLKS : full_blks;
        retval = io_channel_write_blk64(fs->io, cur_pblk, (int)batch, src);
        if (retval != 0) return retval;
        src += (size_t)batch * bsz;
        rem -= batch * bsz;
        cur_pblk += batch;
    }

    if (rem > 0) {
        if (already_initialized) {
            retval = io_channel_read_blk64(fs->io, cur_pblk, 1, blk_buf);
            if (retval != 0) return retval;
        } else {
            memset(blk_buf, 0, bsz);
        }
        memcpy(blk_buf, src, rem);
        retval = io_channel_write_blk64(fs->io, cur_pblk, 1, blk_buf);
        if (retval != 0) return retval;
    }

    return 0;
}

static errcode_t ext4_vfs_write_extents(ext4_vfs_ctx_t *ctx, ext2_ino_t ino,
                                        struct ext2_inode_large *full_ino,
                                        const void *buf, uint64_t start_pos,
                                        uint32_t length, uint32_t *out_written,
                                        int *out_need_fallback) {
    ext2_filsys fs = ctx->fs;
    uint32_t bsz = (uint32_t)fs->blocksize;
    uint64_t end_pos = start_pos + (uint64_t)length;
    uint64_t old_size = EXT2_I_SIZE(full_ino);
    ext2_extent_handle_t ehandle = NULL;
    uint8_t stack_blk[4096];
    uint8_t *heap_blk = NULL;
    uint8_t *blk_buf;
    uint32_t done = 0;
    int inode_dirty = 0;
    errcode_t retval = 0;

    *out_written = 0;
    *out_need_fallback = 0;
    if (length == 0) return 0;
    if (end_pos < start_pos) return EXT2_ET_FILE_TOO_BIG;
    /* Extent-mapped files cap logical block indices below 2^32 - 1 */
    if (((end_pos - 1ULL) / (uint64_t)bsz) >= ((1ULL << 32) - 1ULL)) {
        return EXT2_ET_FILE_TOO_BIG;
    }

    if (bsz <= sizeof(stack_blk)) {
        blk_buf = stack_blk;
    } else {
        heap_blk = (uint8_t *)malloc(bsz);
        if (!heap_blk) return EXT2_ET_NO_MEMORY;
        blk_buf = heap_blk;
    }

    retval = ext2fs_extent_open2(fs, ino, (struct ext2_inode *)full_ino, &ehandle);
    if (retval != 0) {
        free(heap_blk);
        return retval;
    }

    while (done < length) {
        uint64_t cur_pos = start_pos + (uint64_t)done;
        blk64_t cur_lblk = cur_pos / (uint64_t)bsz;
        uint32_t in_blk = (uint32_t)(cur_pos % (uint64_t)bsz);
        uint64_t rem_bytes = (uint64_t)(length - done);
        blk64_t rem_blks = ((uint64_t)in_blk + rem_bytes + (uint64_t)bsz - 1ULL) / (uint64_t)bsz;

        retval = ext2fs_extent_goto(ehandle, cur_lblk);
        if (retval == 0) {
            struct ext2fs_extent ext;
            blk64_t off_in_ext;
            blk64_t avail_blks;
            uint64_t max_bytes;
            uint32_t take;

            retval = ext2fs_extent_get(ehandle, EXT2_EXTENT_CURRENT, &ext);
            if (retval != 0) break;

            if (ext.e_flags & EXT2_EXTENT_FLAGS_UNINIT) {
                *out_need_fallback = 1;
                retval = 0;
                break;
            }

            off_in_ext = cur_lblk - ext.e_lblk;
            avail_blks = (blk64_t)ext.e_len - off_in_ext;
            max_bytes = avail_blks * (uint64_t)bsz - (uint64_t)in_blk;
            take = (rem_bytes < max_bytes) ? (uint32_t)rem_bytes : (uint32_t)max_bytes;

            retval = write_data_blocks(fs, ext.e_pblk + off_in_ext,
                                       (const uint8_t *)buf + done,
                                       in_blk, take, 1, blk_buf);
            if (retval != 0) break;
            done += take;
        } else if (retval == EXT2_ET_EXTENT_NOT_FOUND) {
            struct ext2fs_extent cur_ext;
            struct ext2fs_extent left_ext;
            struct ext2fs_extent next_ext;
            int tree_empty = 0;
            int insert_before = 0;
            int has_left = 0;
            blk64_t max_hole_blks = rem_blks;
            blk64_t pblk = 0;
            blk64_t plen = 0;
            int extended_left = 0;

            memset(&cur_ext, 0, sizeof(cur_ext));
            memset(&left_ext, 0, sizeof(left_ext));
            retval = ext2fs_extent_get(ehandle, EXT2_EXTENT_CURRENT, &cur_ext);
            if (retval == EXT2_ET_NO_CURRENT_NODE) {
                tree_empty = 1;
                retval = 0;
            } else if (retval != 0) {
                break;
            } else if (cur_ext.e_lblk > cur_lblk) {
                insert_before = 1;
                if (cur_ext.e_lblk - cur_lblk < max_hole_blks) {
                    max_hole_blks = cur_ext.e_lblk - cur_lblk;
                }
            } else {
                errcode_t ret_next;
                has_left = 1;
                left_ext = cur_ext;
                ret_next = find_next_leaf_after(ehandle, &next_ext);
                if (ret_next == 0) {
                    if (next_ext.e_lblk > cur_lblk &&
                        next_ext.e_lblk - cur_lblk < max_hole_blks) {
                        max_hole_blks = next_ext.e_lblk - cur_lblk;
                    }
                } else if (!is_extent_end_err_w(ret_next)) {
                    retval = ret_next;
                    break;
                }
                retval = ext2fs_extent_goto(ehandle, left_ext.e_lblk);
                if (retval != 0) break;
            }

            if (max_hole_blks > EXT_INIT_MAX_LEN) {
                max_hole_blks = EXT_INIT_MAX_LEN;
            }

            if (has_left &&
                (left_ext.e_lblk + (blk64_t)left_ext.e_len == cur_lblk) &&
                !(left_ext.e_flags & EXT2_EXTENT_FLAGS_UNINIT) &&
                left_ext.e_len < EXT_INIT_MAX_LEN) {
                blk64_t want_pblk = left_ext.e_pblk + (blk64_t)left_ext.e_len;
                blk64_t grow_ask = (blk64_t)EXT_INIT_MAX_LEN - (blk64_t)left_ext.e_len;
                if (grow_ask > max_hole_blks) grow_ask = max_hole_blks;

                if (want_pblk >= fs->super->s_first_data_block &&
                    want_pblk < ext2fs_blocks_count(fs->super)) {
                    errcode_t ret_alloc = ext2fs_new_range(fs, EXT2_NEWRANGE_FIXED_GOAL,
                                                           want_pblk, grow_ask, NULL,
                                                           &pblk, &plen);
                    if (ret_alloc == 0 && pblk == want_pblk && plen > 0) {
                        if (plen > grow_ask) plen = grow_ask;
                        ext2fs_block_alloc_stats_range(fs, pblk, (blk_t)plen, +1);
                        left_ext.e_len += (__u32)plen;
                        retval = ext2fs_extent_replace(ehandle, 0, &left_ext);
                        if (retval != 0) {
                            ext2fs_block_alloc_stats_range(fs, pblk, (blk_t)plen, -1);
                            break;
                        }
                        ext2fs_iblk_add_blocks(fs, (struct ext2_inode *)full_ino, plen);
                        inode_dirty = 1;
                        extended_left = 1;
                    }
                }
            }

            if (!extended_left) {
                blk64_t goal = has_left
                    ? (left_ext.e_pblk + (blk64_t)left_ext.e_len)
                    : ext2fs_find_inode_goal(fs, ino, (struct ext2_inode *)full_ino, cur_lblk);
                struct ext2fs_extent newex;

                retval = ext2fs_new_range(fs, 0, goal, max_hole_blks, NULL, &pblk, &plen);
                if (retval != 0) break;
                if (plen > max_hole_blks) plen = max_hole_blks;

                ext2fs_block_alloc_stats_range(fs, pblk, (blk_t)plen, +1);

                memset(&newex, 0, sizeof(newex));
                newex.e_lblk = cur_lblk;
                newex.e_pblk = pblk;
                newex.e_len = (__u32)plen;
                newex.e_flags = EXT2_EXTENT_FLAGS_LEAF;

                retval = ext2fs_extent_insert(
                    ehandle,
                    (tree_empty || insert_before) ? 0 : EXT2_EXTENT_INSERT_AFTER,
                    &newex);
                if (retval != 0) {
                    ext2fs_block_alloc_stats_range(fs, pblk, (blk_t)plen, -1);
                    inode_dirty = 1;
                    break;
                }
                ext2fs_iblk_add_blocks(fs, (struct ext2_inode *)full_ino, plen);
                inode_dirty = 1;
                retval = ext2fs_extent_fix_parents(ehandle);
                if (retval != 0) break;
            }

            {
                uint64_t max_bytes = plen * (uint64_t)bsz - (uint64_t)in_blk;
                uint32_t take = (rem_bytes < max_bytes) ? (uint32_t)rem_bytes : (uint32_t)max_bytes;
                retval = write_data_blocks(fs, pblk, (const uint8_t *)buf + done,
                                           in_blk, take, 0, blk_buf);
                if (retval != 0) break;
                done += take;
            }
        } else {
            break;
        }
    }

    ext2fs_extent_free(ehandle);
    free(heap_blk);

    if (done > 0 || inode_dirty) {
        if (done > 0) {
            uint64_t new_end = start_pos + (uint64_t)done;
            if (new_end > old_size) {
                errcode_t sz_rc = ext2fs_inode_size_set(fs, (struct ext2_inode *)full_ino, new_end);
                if (retval == 0) retval = sz_rc;
            }
            ext4_inode_touch_times(full_ino, 0, 1, 1);
        }
        {
            errcode_t w_rc = ext2fs_write_inode_full(fs, ino, (struct ext2_inode *)full_ino,
                                                     sizeof(*full_ino));
            if (retval == 0) retval = w_rc;
        }
    }

    *out_written = done;
    return retval;
}

int ext4_vfs_write_stat(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, const void *buf, uint64_t offset,
                        uint32_t length, int write_to_eof, const char *leaf_name,
                        uint32_t *out_written, uint64_t *out_new_size, ext4_stat_t *out_st) {
    ext2_file_t file = NULL;
    struct ext2_inode_large full_ino;
    errcode_t retval;
    unsigned int written = 0;
    ext2_off64_t fsize = 0;
    uint64_t t_vfs;
    ztrace_lock_guard_t lg;

    if (!ctx || !out_written || ino == 0) return -EINVAL;
    if (ctx->overlay) {
        int rc = overlay_vfs_write(ctx, ino, buf, offset, length, write_to_eof, out_written, out_new_size);
        if (rc == 0 && out_st) {
            rc = overlay_vfs_stat_ino(ctx, ino, leaf_name, out_st);
        }
        return rc;
    }
    *out_written = 0;
    if (out_new_size) *out_new_size = 0;
    if (ctx->read_only) return -EROFS;
    if (length > 0 && !buf) return -EINVAL;

    t_vfs = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    lg = ztrace_srw_acquire_exclusive(&ctx->rwlock, "ctx->rwlock wait (X)", "ctx->rwlock hold (X)");
    memset(&full_ino, 0, sizeof(full_ino));
    retval = ext2fs_read_inode_full(ctx->fs, ino, (struct ext2_inode *)&full_ino, sizeof(full_ino));
    if (retval == 0) {
        if (LINUX_S_ISDIR(full_ino.i_mode)) {
            retval = EISDIR;
        } else if (length > 0 || write_to_eof) {
            uint64_t cur_offset = write_to_eof ? EXT2_I_SIZE(&full_ino) : offset;
            const uint8_t *cur_buf = (const uint8_t *)buf;
            uint32_t cur_len = length;
            int need_fallback = 1;

            if (ctx->enable_rw_fast_io &&
                cur_len > 0 &&
                (full_ino.i_flags & EXT4_EXTENTS_FL) &&
                !(full_ino.i_flags & EXT4_INLINE_DATA_FL) &&
                !ext2fs_has_feature_bigalloc(ctx->fs->super) &&
                !(ctx->fs->flags & EXT2_FLAG_SHARE_DUP)) {
                uint32_t fast_written = 0;
                need_fallback = 0;
                retval = ext4_vfs_write_extents(ctx, ino, &full_ino, cur_buf, cur_offset,
                                                cur_len, &fast_written, &need_fallback);
                written += fast_written;
                if (retval == 0 && need_fallback && fast_written < cur_len) {
                    cur_buf += fast_written;
                    cur_offset += fast_written;
                    cur_len -= fast_written;
                } else {
                    need_fallback = 0;
                }
            }

            if (retval == 0 && need_fallback) {
                retval = ext2fs_file_open2(ctx->fs, ino, (struct ext2_inode *)&full_ino, EXT2_FILE_WRITE, &file);
                if (retval == 0) {
                    unsigned int fb_written = 0;
                    if (write_to_eof && written == 0) {
                        retval = ext2fs_file_llseek(file, 0, EXT2_SEEK_END, NULL);
                    } else {
                        retval = ext2fs_file_llseek(file, cur_offset, EXT2_SEEK_SET, NULL);
                    }
                    if (retval == 0 && cur_len > 0) {
                        retval = ext2fs_file_write(file, cur_buf, cur_len, &fb_written);
                        written += fb_written;
                    }
                    if (retval == 0) {
                        retval = ext2fs_file_close(file);
                        file = NULL;
                        if (retval == 0) {
                            memset(&full_ino, 0, sizeof(full_ino));
                            retval = ext2fs_read_inode_full(ctx->fs, ino, (struct ext2_inode *)&full_ino, sizeof(full_ino));
                        }
                        if (retval == 0) {
                            fsize = EXT2_I_SIZE(&full_ino);
                            ext4_inode_touch_times(&full_ino, 0, 1, 1);
                            retval = ext2fs_write_inode_full(ctx->fs, ino, (struct ext2_inode *)&full_ino, sizeof(full_ino));
                            if (retval == 0 && out_st) {
                                ext4_fill_stat_from_inode(ctx, ino, &full_ino, leaf_name, out_st);
                            }
                        }
                    }
                    if (file) {
                        ext2fs_file_close(file);
                    }
                }
            } else if (retval == 0) {
                if (written == 0 && write_to_eof) {
                    fsize = EXT2_I_SIZE(&full_ino);
                    ext4_inode_touch_times(&full_ino, 0, 1, 1);
                    retval = ext2fs_write_inode_full(ctx->fs, ino, (struct ext2_inode *)&full_ino, sizeof(full_ino));
                } else {
                    fsize = EXT2_I_SIZE(&full_ino);
                }
                if (retval == 0 && out_st) {
                    ext4_fill_stat_from_inode(ctx, ino, &full_ino, leaf_name, out_st);
                }
            }
        } else {
            fsize = EXT2_I_SIZE(&full_ino);
            ext4_inode_touch_times(&full_ino, 0, 1, 1);
            retval = ext2fs_write_inode_full(ctx->fs, ino, (struct ext2_inode *)&full_ino, sizeof(full_ino));
            if (retval == 0 && out_st) {
                ext4_fill_stat_from_inode(ctx, ino, &full_ino, leaf_name, out_st);
            }
        }
    }
    ztrace_srw_release_exclusive(&ctx->rwlock, &lg);
    if (t_vfs) {
        uint64_t end = ztrace_now_qpc();
        int rc_vfs = (retval != 0) ? ext4_errcode_to_errno(retval) : 0;
        ztrace_record_complete("vfs", "ext2fs_file_write", t_vfs, end - t_vfs,
                               (uint64_t)ino, (uint64_t)written, (int64_t)rc_vfs, ZTRACE_ARGS_RW_OP);
    }

    if (retval != 0) return ext4_errcode_to_errno(retval);
    *out_written = (uint32_t)written;
    if (out_new_size) *out_new_size = fsize;
    return 0;
}

int ext4_vfs_write(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, const void *buf, uint64_t offset,
                   uint32_t length, int write_to_eof, uint32_t *out_written, uint64_t *out_new_size) {
    return ext4_vfs_write_stat(ctx, ino, buf, offset, length, write_to_eof, NULL,
                               out_written, out_new_size, NULL);
}
