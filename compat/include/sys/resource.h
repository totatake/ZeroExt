#ifndef LIBEXT2FS_WIN_COMPAT_SYS_RESOURCE_H
#define LIBEXT2FS_WIN_COMPAT_SYS_RESOURCE_H

#ifdef _MSC_VER
struct rlimit {
    unsigned long long rlim_cur;
    unsigned long long rlim_max;
};
#define RLIMIT_FSIZE 1
static __inline int getrlimit(int resource, struct rlimit *rlim) {
    (void)resource;
    if (rlim) {
        rlim->rlim_cur = (unsigned long long)-1;
        rlim->rlim_max = (unsigned long long)-1;
    }
    return 0;
}
#endif

#endif /* LIBEXT2FS_WIN_COMPAT_SYS_RESOURCE_H */
