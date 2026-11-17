#ifndef LIBEXT2FS_WIN_COMPAT_SYS_TIME_H
#define LIBEXT2FS_WIN_COMPAT_SYS_TIME_H

#include <time.h>

#ifdef _MSC_VER
#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
struct timeval {
    long tv_sec;
    long tv_usec;
};
#endif
#endif

#endif /* LIBEXT2FS_WIN_COMPAT_SYS_TIME_H */
