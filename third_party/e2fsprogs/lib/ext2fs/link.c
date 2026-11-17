#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "ext2fs/ext2fs.h"

struct link_ctx {
    const char *name;
    int namelen;
    ext2_ino_t inode;
    int flags;
    int done;
};

static int link_cb(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent,
                   int offset, int blocksize, char *buf, void *priv_data) {
    struct link_ctx *ctx = (struct link_ctx *)priv_data;
    struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)dirent;
    int need_len = EXT2_DIR_REC_LEN(ctx->namelen);
    int cur_actual;
    int free_space;
    (void)dir;
    (void)entry;
    (void)offset;
    (void)blocksize;
    (void)buf;

    if (ctx->done) {
        return DIRENT_ABORT;
    }
    cur_actual = (de->inode == 0) ? 0 : EXT2_DIR_REC_LEN(de->name_len);
    free_space = de->rec_len - cur_actual;

    if (free_space >= need_len) {
        if (de->inode == 0) {
            de->inode = ctx->inode;
            de->name_len = (__u8)ctx->namelen;
            de->file_type = (__u8)(ctx->flags & 0xFF);
            memcpy(de->name, ctx->name, (size_t)ctx->namelen);
        } else {
            struct ext2_dir_entry_2 *next_de = (struct ext2_dir_entry_2 *)(((char *)de) + cur_actual);
            int orig_rec_len = de->rec_len;
            de->rec_len = (__u16)cur_actual;
            next_de->inode = ctx->inode;
            next_de->rec_len = (__u16)(orig_rec_len - cur_actual);
            next_de->name_len = (__u8)ctx->namelen;
            next_de->file_type = (__u8)(ctx->flags & 0xFF);
            memcpy(next_de->name, ctx->name, (size_t)ctx->namelen);
        }
        ctx->done = 1;
        return DIRENT_CHANGED | DIRENT_ABORT;
    }
    return 0;
}

errcode_t ext2fs_expand_dir(ext2_filsys fs, ext2_ino_t dir) {
    struct ext2_inode inode;
    blk64_t new_blk = 0;
    blk64_t next_lblk;
    char *dir_blk = NULL;
    errcode_t retval;

    if (!fs) return EXT2_ET_INVALID_ARGUMENT;
    if (!(fs->flags & EXT2_FLAG_RW)) return EXT2_ET_RO_FILSYS;

    retval = ext2fs_read_inode(fs, dir, &inode);
    if (retval) return retval;
    if (!LINUX_S_ISDIR(inode.i_mode)) return EXT2_ET_NO_DIRECTORY;

    next_lblk = inode.i_size / fs->blocksize;
    retval = ext2fs_bmap2(fs, dir, &inode, NULL, BMAP_ALLOC, next_lblk, NULL, &new_blk);
    if (retval) return retval;

    retval = ext2fs_new_dir_block(fs, 0, 0, &dir_blk);
    if (retval) return retval;

    retval = ext2fs_write_dir_block4(fs, new_blk, dir_blk, 0, dir);
    ext2fs_free_mem(&dir_blk);
    if (retval) return retval;

    inode.i_size += fs->blocksize;
    return ext2fs_write_inode(fs, dir, &inode);
}

errcode_t ext2fs_link(ext2_filsys fs, ext2_ino_t dir, const char *name, ext2_ino_t ino, int flags) {
    struct link_ctx ctx;
    struct ext2_inode dir_inode;
    int namelen;
    errcode_t retval;

    if (!fs || !name) return EXT2_ET_INVALID_ARGUMENT;
    if (!(fs->flags & EXT2_FLAG_RW)) return EXT2_ET_RO_FILSYS;
    namelen = (int)strlen(name);
    if (namelen == 0 || namelen > EXT2_NAME_LEN) return EXT2_ET_INVALID_ARGUMENT;

    retval = ext2fs_read_inode(fs, dir, &dir_inode);
    if (retval) return retval;
    if (!LINUX_S_ISDIR(dir_inode.i_mode)) return EXT2_ET_NO_DIRECTORY;

    /* Clear HTree flag on modification so kernel re-scans linearly safely */
    if (dir_inode.i_flags & EXT2_INDEX_FL) {
        dir_inode.i_flags &= ~EXT2_INDEX_FL;
        retval = ext2fs_write_inode(fs, dir, &dir_inode);
        if (retval) return retval;
    }

    ctx.name = name;
    ctx.namelen = namelen;
    ctx.inode = ino;
    ctx.flags = EXT2_HAS_INCOMPAT_FEATURE(fs->super, EXT2_FEATURE_INCOMPAT_FILETYPE) ? flags : 0;
    ctx.done = 0;

    retval = (errcode_t)ext2fs_dir_iterate2(fs, dir, DIRENT_FLAG_INCLUDE_EMPTY, NULL, link_cb, &ctx);
    if (retval) return retval;
    if (ctx.done) return 0;

    retval = ext2fs_expand_dir(fs, dir);
    if (retval) return retval;

    retval = (errcode_t)ext2fs_dir_iterate2(fs, dir, DIRENT_FLAG_INCLUDE_EMPTY, NULL, link_cb, &ctx);
    if (retval) return retval;
    return ctx.done ? 0 : EXT2_ET_DIR_NO_SPACE;
}
