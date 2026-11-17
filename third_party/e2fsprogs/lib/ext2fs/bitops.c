#include "config.h"
#include "ext2fs/ext2fs.h"

int ext2fs_set_bit(unsigned int nr, void *addr) {
    unsigned char *p = (unsigned char *)addr + (nr >> 3);
    unsigned char mask = (unsigned char)(1U << (nr & 7U));
    int old = (*p & mask) != 0;
    *p |= mask;
    return old;
}

int ext2fs_clear_bit(unsigned int nr, void *addr) {
    unsigned char *p = (unsigned char *)addr + (nr >> 3);
    unsigned char mask = (unsigned char)(1U << (nr & 7U));
    int old = (*p & mask) != 0;
    *p &= (unsigned char)(~mask);
    return old;
}

int ext2fs_test_bit(unsigned int nr, const void *addr) {
    const unsigned char *p = (const unsigned char *)addr + (nr >> 3);
    return (*p & (1U << (nr & 7U))) != 0;
}
