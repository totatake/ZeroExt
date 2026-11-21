#ifndef ZEROEXT_COMPAT_ARPA_INET_H
#define ZEROEXT_COMPAT_ARPA_INET_H

#include <stdint.h>
#include <stdlib.h>

static __inline uint32_t htonl(uint32_t x) {
    return (uint32_t)_byteswap_ulong((unsigned long)x);
}

static __inline uint32_t ntohl(uint32_t x) {
    return (uint32_t)_byteswap_ulong((unsigned long)x);
}

static __inline uint16_t htons(uint16_t x) {
    return (uint16_t)_byteswap_ushort((unsigned short)x);
}

static __inline uint16_t ntohs(uint16_t x) {
    return (uint16_t)_byteswap_ushort((unsigned short)x);
}

#endif /* ZEROEXT_COMPAT_ARPA_INET_H */
