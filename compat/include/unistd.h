#ifndef LIBEXT2FS_WIN_COMPAT_UNISTD_H
#define LIBEXT2FS_WIN_COMPAT_UNISTD_H

#ifdef _MSC_VER
#include <io.h>
#include <process.h>
#include <stdlib.h>
#include <BaseTsd.h>

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif

#ifndef R_OK
#define R_OK 4
#define W_OK 2
#define X_OK 0
#define F_OK 0
#endif

static __inline int getpagesize(void) {
    return 4096;
}

static __inline int getuid(void) { return 0; }
static __inline int geteuid(void) { return 0; }
static __inline int getgid(void) { return 0; }
static __inline int getegid(void) { return 0; }
#endif

#endif /* LIBEXT2FS_WIN_COMPAT_UNISTD_H */
