#include "config.h"
#include <string.h>
#include "ext2fs/ext2fs.h"

struct unlink_ctx {
    const char *name;
    int namelen;
    ext2_ino_t inode;
    struct ext2_dir_entry *prev;
    int prev_offset;
    int done;
};

static int unlink_cb(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent,
                     int offset, int blocksize, char *buf, void *priv_data) {
    struct unlink_ctx *ctx = (struct unlink_ctx *)priv_data;
    struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)dirent;
    (void)dir;
    (void)entry;
    (void)blocksize;
    (void)buf;

    if (offset == 0) {
        ctx->prev = NULL;
    }

    if (de->inode != 0 &&
        (!ctx->name || ((int)de->name_len == ctx->namelen && memcmp(de->name, ctx->name, (size_t)ctx->namelen) == 0)) &&
        (!ctx->inode || de->inode == ctx->inode)) {
        if (ctx->prev && ctx->prev->inode != 0) {
            ctx->prev->rec_len += de->rec_len;
        } else {
            de->inode = 0;
        }
        ctx->done = 1;
        return DIRENT_CHANGED | DIRENT_ABORT;
    }

    ctx->prev = dirent;
    ctx->prev_offset = offset;
    return 0;
}

errcode_t ext2fs_unlink(ext2_filsys fs, ext2_ino_t dir, const char *name, ext2_ino_t ino, int flags) {
    struct unlink_ctx ctx;
    struct ext2_inode dir_inode;
    errcode_t retval;
    (void)flags;

    if (!fs) return EXT2_ET_INVALID_ARGUMENT;
    if (!(fs->flags & EXT2_FLAG_RW)) return EXT2_ET_RO_FILSYS;

    retval = ext2fs_read_inode(fs, dir, &dir_inode);
    if (retval) return retval;
    if (!LINUX_S_ISDIR(dir_inode.i_mode)) return EXT2_ET_NO_DIRECTORY;

    if (dir_inode.i_flags & EXT2_INDEX_FL) {
        dir_inode.i_flags &= ~EXT2_INDEX_FL;
        ext2fs_write_inode(fs, dir, &dir_inode);
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.name = name;
    ctx.namelen = name ? (int)strlen(name) : 0;
    ctx.inode = ino;

    retval = (errcode_t)ext2fs_dir_iterate2(fs, dir, DIRENT_FLAG_INCLUDE_EMPTY, NULL, unlink_cb, &ctx);
    if (retval) return retval;
    return ctx.done ? 0 : EXT2_ET_FILE_NOT_FOUND;
}
