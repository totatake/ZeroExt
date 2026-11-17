/*
 * ext2_io.h --- I/O Manager interface for ext2fs
 */
#ifndef _EXT2FS_EXT2_IO_H
#define _EXT2FS_EXT2_IO_H

#include <et/com_err.h>
#include <ext2fs/ext2_types.h>

typedef __u32 ext2_ino_t;
typedef __u32 blk_t;
typedef __u64 blk64_t;
typedef __u32 dgrp_t;
typedef __u64 ext2_off64_t;
typedef __u32 ext2_off_t;

typedef struct struct_io_manager *io_manager;
typedef struct struct_io_channel *io_channel;
typedef struct struct_io_stats *io_stats;

#define EXT2_ET_MAGIC_IO_CHANNEL_CONST 1
#define EXT2_ET_MAGIC_IO_MANAGER_CONST 2

#define IO_FLAG_RW        0x0001
#define IO_FLAG_EXCLUSIVE 0x0002
#define IO_FLAG_DIRECT_IO 0x0004
#define IO_FLAG_NOCACHE   0x0008

struct struct_io_stats {
    int num_fields;
    int reserved;
    __u64 bytes_read;
    __u64 bytes_written;
};

struct struct_io_channel {
    errcode_t magic;
    io_manager manager;
    char *name;
    int block_size;
    errcode_t (*read_error)(io_channel channel, unsigned long block, int count,
                            void *data, int size, int actual_bytes_read, errcode_t error);
    errcode_t (*write_error)(io_channel channel, unsigned long block, int count,
                             const void *data, int size, int actual_bytes_written, errcode_t error);
    int refcount;
    int flags;
    long reserved[14];
    void *private_data;
    void *app_data;
    int align;
};

struct struct_io_manager {
    errcode_t magic;
    const char *name;
    errcode_t (*open)(const char *name, int flags, io_channel *channel);
    errcode_t (*close)(io_channel channel);
    errcode_t (*set_blksize)(io_channel channel, int blksize);
    errcode_t (*read_blk)(io_channel channel, unsigned long block, int count, void *data);
    errcode_t (*write_blk)(io_channel channel, unsigned long block, int count, const void *data);
    errcode_t (*flush)(io_channel channel);
    errcode_t (*write_byte)(io_channel channel, unsigned long offset, int count, const void *data);
    errcode_t (*set_option)(io_channel channel, const char *option, const char *arg);
    errcode_t (*get_stats)(io_channel channel, io_stats *io_stats);
    errcode_t (*read_blk64)(io_channel channel, unsigned long long block, int count, void *data);
    errcode_t (*write_blk64)(io_channel channel, unsigned long long block, int count, const void *data);
    errcode_t (*discard)(io_channel channel, unsigned long long block, unsigned long long count);
    errcode_t (*cache_readahead)(io_channel channel, unsigned long long block, unsigned long long count);
    errcode_t (*zeroout)(io_channel channel, unsigned long long block, unsigned long long count);
    long reserved[14];
};

#define io_channel_open(m, n, f, c) ((m)->open((n), (f), (c)))
#define io_channel_close(c)         ((c)->manager->close((c)))
#define io_channel_set_blksize(c, s) ((c)->manager->set_blksize((c), (s)))
#define io_channel_flush(c)         ((c)->manager->flush((c)))

extern errcode_t io_channel_read_blk64(io_channel channel, unsigned long long block, int count, void *data);
extern errcode_t io_channel_write_blk64(io_channel channel, unsigned long long block, int count, const void *data);
extern errcode_t io_channel_read_blk(io_channel channel, unsigned long block, int count, void *data);
extern errcode_t io_channel_write_blk(io_channel channel, unsigned long block, int count, const void *data);
extern errcode_t io_channel_write_byte(io_channel channel, unsigned long offset, int count, const void *data);
extern errcode_t io_channel_set_options(io_channel channel, const char *options);
extern errcode_t io_channel_discard(io_channel channel, unsigned long long block, unsigned long long count);
extern errcode_t io_channel_zeroout(io_channel channel, unsigned long long block, unsigned long long count);

#endif /* _EXT2FS_EXT2_IO_H */
