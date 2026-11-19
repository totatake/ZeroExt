#ifndef ZEROEXT_COMPAT_SYS_TIME_H
#define ZEROEXT_COMPAT_SYS_TIME_H

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

#endif /* ZEROEXT_COMPAT_SYS_TIME_H */
