#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include <errno.h>

int ext4_vfs_flush(ext4_vfs_ctx_t *ctx) {
    errcode_t retval;
    if (!ctx) return -EINVAL;
    if (ctx->read_only) return 0;

    AcquireSRWLockExclusive(&ctx->rwlock);
    retval = ext2fs_flush(ctx->fs);
    ReleaseSRWLockExclusive(&ctx->rwlock);
    return ext4_errcode_to_errno(retval);
}
