#ifndef ZEROEXT_MSVC_COMPAT_H
#define ZEROEXT_MSVC_COMPAT_H

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
#include <sys/time.h>

#ifndef __attribute__
#define __attribute__(x)
#endif

#ifndef __inline__
#define __inline__ __inline
#endif

#ifndef __restrict__
#define __restrict__ __restrict
#endif

#ifndef __GNUC_PREREQ
#define __GNUC_PREREQ(maj, min) 0
#endif

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif

#ifndef _MODE_T_DEFINED
#define _MODE_T_DEFINED
typedef unsigned short mode_t;
#endif

#ifndef S_ISBLK
#define S_ISBLK(m) (0)
#endif

#ifndef strcasecmp
#define strcasecmp _stricmp
#endif

#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif

/* Suppress noisy MSVC conversion/padding warnings inside upstream e2fsprogs */
#pragma warning(disable : 4005) /* macro redefinition (min in alloc.c / crc32c.c) */
#pragma warning(disable : 4018) /* signed/unsigned mismatch */
#pragma warning(disable : 4100) /* unreferenced formal parameter */
#pragma warning(disable : 4117) /* reserved macro '__FUNCTION__' in kernel-jbd.h */
#pragma warning(disable : 4127) /* conditional expression is constant */
#pragma warning(disable : 4146) /* unary minus applied to unsigned type */
#pragma warning(disable : 4200) /* zero-sized array in struct/union */
#pragma warning(disable : 4244) /* conversion possible loss of data */
#pragma warning(disable : 4245) /* signed/unsigned mismatch initialization */
#pragma warning(disable : 4267) /* size_t conversion */
#pragma warning(disable : 4305) /* truncation from unsigned int to __u16 */
#pragma warning(disable : 4334) /* 32-bit shift implicitly converted to 64 bits */
#pragma warning(disable : 4789) /* unreachable branch overrun warning in ext2fs_inode_xtime_set */
#pragma warning(disable : 4996) /* deprecated POSIX names */
#endif /* _MSC_VER */

#endif /* ZEROEXT_MSVC_COMPAT_H */
