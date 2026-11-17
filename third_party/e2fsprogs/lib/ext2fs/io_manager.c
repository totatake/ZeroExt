#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "ext2fs/ext2_io.h"
#include "ext2fs/ext2_err.h"

errcode_t io_channel_read_blk64(io_channel channel, unsigned long long block, int count, void *data) {
    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL) {
        return EXT2_ET_MAGIC_IO_CHANNEL;
    }
    if (channel->manager->read_blk64) {
        return channel->manager->read_blk64(channel, block, count, data);
    }
    if (block > 0xFFFFFFFFULL) {
        return EXT2_ET_IO_CHANNEL_NO_SUPPORT_64;
    }
    return channel->manager->read_blk(channel, (unsigned long)block, count, data);
}

errcode_t io_channel_write_blk64(io_channel channel, unsigned long long block, int count, const void *data) {
    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL) {
        return EXT2_ET_MAGIC_IO_CHANNEL;
    }
    if (channel->manager->write_blk64) {
        return channel->manager->write_blk64(channel, block, count, data);
    }
    if (block > 0xFFFFFFFFULL) {
        return EXT2_ET_IO_CHANNEL_NO_SUPPORT_64;
    }
    return channel->manager->write_blk(channel, (unsigned long)block, count, data);
}

errcode_t io_channel_read_blk(io_channel channel, unsigned long block, int count, void *data) {
    return io_channel_read_blk64(channel, (unsigned long long)block, count, data);
}

errcode_t io_channel_write_blk(io_channel channel, unsigned long block, int count, const void *data) {
    return io_channel_write_blk64(channel, (unsigned long long)block, count, data);
}

errcode_t io_channel_write_byte(io_channel channel, unsigned long offset, int count, const void *data) {
    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL) {
        return EXT2_ET_MAGIC_IO_CHANNEL;
    }
    if (!channel->manager->write_byte) {
        return EXT2_ET_UNIMPLEMENTED;
    }
    return channel->manager->write_byte(channel, offset, count, data);
}

errcode_t io_channel_set_options(io_channel channel, const char *opts) {
    errcode_t retval = 0;
    char *copy, *p, *next, *eq;

    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL) {
        return EXT2_ET_MAGIC_IO_CHANNEL;
    }
    if (!opts || !*opts) {
        return 0;
    }
    if (!channel->manager->set_option) {
        return EXT2_ET_UNIMPLEMENTED;
    }
    copy = (char *)malloc(strlen(opts) + 1);
    if (!copy) {
        return EXT2_ET_NO_MEMORY;
    }
    strcpy(copy, opts);
    p = copy;
    while (p && *p) {
        next = strchr(p, '&');
        if (next) {
            *next++ = '\0';
        }
        eq = strchr(p, '=');
        if (eq) {
            *eq++ = '\0';
        }
        retval = channel->manager->set_option(channel, p, eq);
        if (retval) {
            break;
        }
        p = next;
    }
    free(copy);
    return retval;
}

errcode_t io_channel_discard(io_channel channel, unsigned long long block, unsigned long long count) {
    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL) {
        return EXT2_ET_MAGIC_IO_CHANNEL;
    }
    if (channel->manager->discard) {
        return channel->manager->discard(channel, block, count);
    }
    return EXT2_ET_UNIMPLEMENTED;
}

errcode_t io_channel_zeroout(io_channel channel, unsigned long long block, unsigned long long count) {
    if (!channel || channel->magic != EXT2_ET_MAGIC_IO_CHANNEL) {
        return EXT2_ET_MAGIC_IO_CHANNEL;
    }
    if (channel->manager->zeroout) {
        return channel->manager->zeroout(channel, block, count);
    }
    return EXT2_ET_UNIMPLEMENTED;
}
