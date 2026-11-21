#ifndef ZEROEXT_CONFIG_H
#define ZEROEXT_CONFIG_H

#define PACKAGE "e2fsprogs"
#define PACKAGE_NAME "e2fsprogs"
#define PACKAGE_VERSION "1.47.4"
#define VERSION "1.47.4"

#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_ERRNO_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_FCNTL_H 1
#define HAVE_UNISTD_H 1
#define HAVE_LIMITS_H 1

#define HAVE_STRCASECMP 1
#define HAVE_STRDUP 1
#define HAVE_STRNLEN 1
#define HAVE_MEMSET 1
#define HAVE_MEMCPY 1

/* #undef HAVE_EXT2_IOCTLS */
/* #undef ENABLE_BMAP_STATS */
/* #undef ENABLE_NLS */

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

#endif /* ZEROEXT_CONFIG_H */
