#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int ext4_vfs_open(ext4_vfs_ctx_t *ctx, const char *path, int writable, ext4_file_handle_t **out_handle) {
    char norm[1024];
    ext2_ino_t ino = 0;
    struct ext2_inode ino_st;
    ext4_file_handle_t *h;
    int rc;
    errcode_t retval;

    if (!ctx || !path || !out_handle) return -EINVAL;
    *out_handle = NULL;
    memset(&ino_st, 0, sizeof(ino_st));

    if (writable && ctx->read_only) {
        return -EROFS;
    }

    rc = ext4_vfs_normalize_path(path, norm, sizeof(norm));
    if (rc != 0) return rc;

    AcquireSRWLockShared(&ctx->rwlock);
    rc = ext4_vfs_lookup_locked(ctx, norm, &ino);
    if (rc == 0) {
        retval = ext2fs_read_inode(ctx->fs, ino, &ino_st);
        if (retval) rc = ext4_errcode_to_errno(retval);
    }
    ReleaseSRWLockShared(&ctx->rwlock);

    if (rc != 0) return rc;

    h = (ext4_file_handle_t *)calloc(1, sizeof(*h));
    if (!h) return -ENOMEM;
    h->ino = ino;
    h->is_dir = LINUX_S_ISDIR(ino_st.i_mode);
    h->open_writable = writable;
    strcpy_s(h->path, sizeof(h->path), norm);
    *out_handle = h;
    return 0;
}

void ext4_vfs_close_handle(ext4_file_handle_t *handle) {
    free(handle);
}
