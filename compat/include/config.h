#ifndef LIBEXT2FS_WIN_CONFIG_H
#define LIBEXT2FS_WIN_CONFIG_H

#define PACKAGE "e2fsprogs"
#define PACKAGE_NAME "e2fsprogs"
#define PACKAGE_VERSION "1.47.2"
#define VERSION "1.47.2"

#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_ERRNO_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_FCNTL_H 1
#define HAVE_UNISTD_H 1
#define HAVE_LIMITS_H 1

#define HAVE_STRCASECMP 1
#define HAVE_STRDUP 1
#define HAVE_MEMSET 1
#define HAVE_MEMCPY 1

#define HAVE_EXT2_IOCTLS 0
#define ENABLE_BMAP_STATS 0
#define ENABLE_NLS 0

#define SIZEOF_SHORT 2
#define SIZEOF_INT 4
#define SIZEOF_LONG 4
#define SIZEOF_LONG_LONG 8
#define SIZEOF_OFF_T 8
#define SIZEOF_TIME_T 8

#ifdef _WIN64
#define SIZEOF_SIZE_T 8
#define SIZEOF_VOID_P 8
#else
#define SIZEOF_SIZE_T 4
#define SIZEOF_VOID_P 4
#endif

#endif /* LIBEXT2FS_WIN_CONFIG_H */
