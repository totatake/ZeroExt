#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include "../overlay/overlay_vfs.h"
#include "../trace/perfetto_trace.h"
#include <errno.h>

int ext4_vfs_flush(ext4_vfs_ctx_t *ctx) {
    errcode_t retval;
    ztrace_lock_guard_t lg;
    if (!ctx) return -EINVAL;
    if (ctx->overlay) return overlay_vfs_flush(ctx);
    if (ctx->read_only || ctx->lower_read_only) return 0;

    lg = ztrace_srw_acquire_exclusive(&ctx->rwlock, "ctx->rwlock wait (X)", "ctx->rwlock hold (X)");
    retval = ext2fs_flush(ctx->fs);
    ztrace_srw_release_exclusive(&ctx->rwlock, &lg);
    return ext4_errcode_to_errno(retval);
}
