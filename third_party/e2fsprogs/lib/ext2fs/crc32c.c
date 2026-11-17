#include "config.h"
#include "ext2fs/ext2fs.h"

#define CRC32C_POLY_LE 0x82F63B78U

static __u32 crc32c_table[256];
static int crc32c_initialized = 0;

static void init_crc32c_table(void) {
    __u32 i, j;
    if (crc32c_initialized) {
        return;
    }
    for (i = 0; i < 256; i++) {
        __u32 c = i;
        for (j = 0; j < 8; j++) {
            if (c & 1) {
                c = CRC32C_POLY_LE ^ (c >> 1);
            } else {
                c >>= 1;
            }
        }
        crc32c_table[i] = c;
    }
    crc32c_initialized = 1;
}

__u32 ext2fs_crc32c_le(__u32 crc, unsigned char const *p, size_t len) {
    init_crc32c_table();
    while (len--) {
        crc = crc32c_table[(crc ^ *p++) & 0xFFU] ^ (crc >> 8);
    }
    return crc;
}
