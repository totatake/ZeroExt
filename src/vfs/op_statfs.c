#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include "../overlay/overlay_vfs.h"
#include <errno.h>
#include <string.h>

int ext4_vfs_lower_statfs(ext4_vfs_ctx_t *ctx, ext4_volume_stat_t *out_vol) {
    uint64_t blk_sz;
    uint32_t serial = 0;

    if (!ctx || !out_vol) return -EINVAL;
    memset(out_vol, 0, sizeof(*out_vol));

    AcquireSRWLockShared(&ctx->rwlock);
    blk_sz = ctx->fs->blocksize;
    out_vol->block_size = (uint32_t)blk_sz;
    out_vol->total_bytes = ext2fs_blocks_count(ctx->fs->super) * blk_sz;
    out_vol->free_bytes = ext2fs_free_blocks_count(ctx->fs->super) * blk_sz;
    out_vol->read_only = ctx->read_only;

    memcpy(&serial, ctx->fs->super->s_uuid, sizeof(serial));
    out_vol->serial_number = serial ? serial : 0xE2F50001U;

    memcpy(out_vol->volume_label, ctx->fs->super->s_volume_name, 16);
    out_vol->volume_label[16] = '\0';
    ReleaseSRWLockShared(&ctx->rwlock);
    return 0;
}

int ext4_vfs_statfs(ext4_vfs_ctx_t *ctx, ext4_volume_stat_t *out_vol) {
    if (ctx && ctx->overlay) {
        return overlay_vfs_statfs(ctx, out_vol);
    }
    return ext4_vfs_lower_statfs(ctx, out_vol);
}

int ext4_vfs_set_label(ext4_vfs_ctx_t *ctx, const char *label) {
    size_t len;
    errcode_t retval;

    if (!ctx || !label) return -EINVAL;
    if (ctx->read_only || ctx->lower_read_only) return -EROFS;

    AcquireSRWLockExclusive(&ctx->rwlock);
    memset(ctx->fs->super->s_volume_name, 0, sizeof(ctx->fs->super->s_volume_name));
    len = strlen(label);
    if (len > sizeof(ctx->fs->super->s_volume_name)) {
        len = sizeof(ctx->fs->super->s_volume_name);
    }
    memcpy(ctx->fs->super->s_volume_name, label, len);
    memcpy(ctx->volume_label, ctx->fs->super->s_volume_name, 16);
    ctx->volume_label[16] = '\0';
    ext2fs_mark_super_dirty(ctx->fs);
    retval = ext2fs_flush(ctx->fs);
    ReleaseSRWLockExclusive(&ctx->rwlock);
    return ext4_errcode_to_errno(retval);
}
