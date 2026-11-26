#ifndef ZEROEXT4_MSVC_PUBLIC_COMPAT_H
#define ZEROEXT4_MSVC_PUBLIC_COMPAT_H

#ifdef _MSC_VER
#include <sys/types.h>

#ifndef _MODE_T_DEFINED
#define _MODE_T_DEFINED
typedef unsigned short mode_t;
#endif

#ifndef __attribute__
#define __attribute__(x)
#endif
#endif /* _MSC_VER */

#endif /* ZEROEXT4_MSVC_PUBLIC_COMPAT_H */
