#ifndef LIBEXT2FS_WIN_COMPAT_SYS_IOCTL_H
#define LIBEXT2FS_WIN_COMPAT_SYS_IOCTL_H

#ifdef _MSC_VER
static __inline int ioctl(int fd, unsigned long request, ...) {
    (void)fd;
    (void)request;
    return -1;
}
#endif

#endif /* LIBEXT2FS_WIN_COMPAT_SYS_IOCTL_H */
