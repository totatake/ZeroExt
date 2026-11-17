#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "ext2fs/ext2fs.h"

static errcode_t namei_internal(ext2_filsys fs, ext2_ino_t root, ext2_ino_t cwd,
                                const char *name, int follow, int depth, ext2_ino_t *inode) {
    ext2_ino_t cur;
    const char *p = name;
    errcode_t retval;

    if (!fs || !name || !inode) return EXT2_ET_INVALID_ARGUMENT;
    if (depth > 40) return EXT2_ET_SYMLINK_LOOP;
    if (root == 0) root = EXT2_ROOT_INO;
    if (cwd == 0) cwd = root;

    if (*p == '/') {
        cur = root;
        while (*p == '/') p++;
    } else {
        cur = cwd;
    }

    if (*p == '\0') {
        *inode = cur;
        return 0;
    }

    while (*p) {
        const char *slash = strchr(p, '/');
        int len = slash ? (int)(slash - p) : (int)strlen(p);
        ext2_ino_t next_ino = 0;

        if (len > 0) {
            retval = ext2fs_lookup(fs, cur, p, len, NULL, &next_ino);
            if (retval) return retval;

            if (follow || slash) {
                struct ext2_inode ino_st;
                retval = ext2fs_read_inode(fs, next_ino, &ino_st);
                if (retval) return retval;
                if (LINUX_S_ISLNK(ino_st.i_mode) && (follow || slash)) {
                    char target[4096];
                    __u64 sz = EXT2_I_SIZE(&ino_st);
                    if (sz >= sizeof(target)) return EXT2_ET_INVALID_ARGUMENT;
                    if (ext2fs_is_fast_symlink(&ino_st)) {
                        memcpy(target, ino_st.i_block, (size_t)sz);
                    } else {
                        ext2_file_t f = NULL;
                        unsigned int got = 0;
                        retval = ext2fs_file_open2(fs, next_ino, &ino_st, 0, &f);
                        if (retval) return retval;
                        retval = ext2fs_file_read(f, target, (unsigned int)sz, &got);
                        ext2fs_file_close(f);
                        if (retval) return retval;
                    }
                    target[sz] = '\0';
                    retval = namei_internal(fs, root, cur, target, 1, depth + 1, &next_ino);
                    if (retval) return retval;
                }
            }
            cur = next_ino;
        }

        if (!slash) break;
        p = slash;
        while (*p == '/') p++;
    }

    *inode = cur;
    return 0;
}

errcode_t ext2fs_namei(ext2_filsys fs, ext2_ino_t root, ext2_ino_t cwd, const char *name, ext2_ino_t *inode) {
    return namei_internal(fs, root, cwd, name, 0, 0, inode);
}

errcode_t ext2fs_namei_follow(ext2_filsys fs, ext2_ino_t root, ext2_ino_t cwd, const char *name, ext2_ino_t *inode) {
    return namei_internal(fs, root, cwd, name, 1, 0, inode);
}
