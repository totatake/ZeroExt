#ifndef LIBEXT2FS_WIN_COMPAT_STRINGS_H
#define LIBEXT2FS_WIN_COMPAT_STRINGS_H

#include <string.h>

#ifdef _MSC_VER
#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif

static __inline int ffs(int i) {
    unsigned int u = (unsigned int)i;
    int bit = 1;
    if (u == 0) return 0;
    while ((u & 1u) == 0) {
        u >>= 1;
        bit++;
    }
    return bit;
}
#endif

#endif /* LIBEXT2FS_WIN_COMPAT_STRINGS_H */
