#ifndef LIBEXT2FS_WIN_MSVC_COMPAT_H
#define LIBEXT2FS_WIN_MSVC_COMPAT_H

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#ifndef _CRT_NONSTDC_NO_DEPRECATE
#define _CRT_NONSTDC_NO_DEPRECATE 1
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif

#ifndef NOMINMAX
#define NOMINMAX 1
#endif

#ifdef _MSC_VER
#include <BaseTsd.h>
#include <stddef.h>
#include <stdint.h>

#ifndef __attribute__
#define __attribute__(x)
#endif

#ifndef __inline__
#define __inline__ __inline
#endif

#ifndef __restrict__
#define __restrict__ __restrict
#endif

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif

#ifndef strcasecmp
#define strcasecmp _stricmp
#endif

#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif

/* Suppress noisy MSVC conversion/padding warnings inside upstream e2fsprogs */
#pragma warning(disable : 4018) /* signed/unsigned mismatch */
#pragma warning(disable : 4100) /* unreferenced formal parameter */
#pragma warning(disable : 4127) /* conditional expression is constant */
#pragma warning(disable : 4200) /* zero-sized array in struct/union */
#pragma warning(disable : 4244) /* conversion possible loss of data */
#pragma warning(disable : 4245) /* signed/unsigned mismatch initialization */
#pragma warning(disable : 4267) /* size_t conversion */
#pragma warning(disable : 4305) /* truncation from unsigned int to __u16 */
#pragma warning(disable : 4996) /* deprecated POSIX names */
#endif /* _MSC_VER */

#endif /* LIBEXT2FS_WIN_MSVC_COMPAT_H */
