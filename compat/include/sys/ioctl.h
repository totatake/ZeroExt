#ifndef ZEROEXT4_COMPAT_SYS_IOCTL_H
#define ZEROEXT4_COMPAT_SYS_IOCTL_H

#ifdef _MSC_VER
static __inline int ioctl(int fd, unsigned long request, ...) {
    (void)fd;
    (void)request;
    return -1;
}
#endif

#endif /* ZEROEXT4_COMPAT_SYS_IOCTL_H */
