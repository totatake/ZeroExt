#include "config.h"
#include <string.h>
#include "ext2fs/ext2fs.h"

struct lookup_ctx {
    const char *name;
    int namelen;
    ext2_ino_t *inode;
    int found;
};

static int lookup_cb(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent,
                     int offset, int blocksize, char *buf, void *priv_data) {
    struct lookup_ctx *ctx = (struct lookup_ctx *)priv_data;
    struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)dirent;
    (void)dir;
    (void)entry;
    (void)offset;
    (void)blocksize;
    (void)buf;

    if (de->inode == 0) {
        return 0;
    }
    if ((int)de->name_len == ctx->namelen && memcmp(de->name, ctx->name, (size_t)ctx->namelen) == 0) {
        *ctx->inode = de->inode;
        ctx->found = 1;
        return DIRENT_ABORT;
    }
    return 0;
}

errcode_t ext2fs_lookup(ext2_filsys fs, ext2_ino_t dir, const char *name, int namelen, char *buf, ext2_ino_t *inode) {
    struct lookup_ctx ctx;
    int ret;

    if (!fs || !name || !inode) return EXT2_ET_INVALID_ARGUMENT;
    if (namelen < 0) namelen = (int)strlen(name);

    ctx.name = name;
    ctx.namelen = namelen;
    ctx.inode = inode;
    ctx.found = 0;

    ret = ext2fs_dir_iterate2(fs, dir, 0, buf, lookup_cb, &ctx);
    if (ret) return (errcode_t)ret;
    return ctx.found ? 0 : EXT2_ET_FILE_NOT_FOUND;
}
