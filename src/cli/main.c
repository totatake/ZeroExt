#include "../common/ext4_errno.h"
#include "../disk/disk_scanner.h"
#include "../disk/partition_table.h"
#include "../disk/superblock_probe.h"
#include "../vfs/ext4_vfs.h"
#include "../extent/extent_map.h"
#include "../driver/winfsp_driver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <dbghelp.h>
#include <psapi.h>

static volatile int g_stop_requested = 0;

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    (void)ctrl_type;
    g_stop_requested = 1;
    return TRUE;
}

/* ---- Crash diagnostics: a crash on a WinFsp dispatcher thread would otherwise make the
 * mount process vanish silently, leaving an inaccessible drive letter behind. ---- */
static void write_minidump(EXCEPTION_POINTERS *ep) {
    WCHAR path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    HANDLE f;
    while (n > 0 && path[n - 1] != L'\\') n--;
    path[n] = 0;
    wcscat_s(path, MAX_PATH, L"ext4fsp_crash.dmp");
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                              MiniDumpWithIndirectlyReferencedMemory, ep ? &mei : NULL, NULL, NULL)) {
            fprintf(stderr, "Crash dump written: %ls\n", path);
        }
        CloseHandle(f);
    }
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    void *addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = NULL;
    char modname[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod)) {
        GetModuleFileNameA(mod, modname, sizeof(modname));
    }
    fprintf(stderr, "\n*** FATAL: exception 0x%08lX at %p (%s+0x%llX), thread %lu ***\n",
            ep->ExceptionRecord->ExceptionCode, addr, modname,
            (unsigned long long)((ULONG_PTR)addr - (ULONG_PTR)mod), GetCurrentThreadId());
    fflush(stderr);
    write_minidump(ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void crt_invalid_parameter(const wchar_t *expr, const wchar_t *func, const wchar_t *file,
                                  unsigned int line, uintptr_t reserved) {
    (void)reserved;
    fprintf(stderr, "\n*** FATAL: CRT invalid parameter (%ls) in %ls %ls:%u ***\n",
            expr ? expr : L"?", func ? func : L"?", file ? file : L"?", line);
    fflush(stderr);
    write_minidump(NULL);
    TerminateProcess(GetCurrentProcess(), 3);
}

static void abort_handler(int sig) {
    (void)sig;
    fprintf(stderr, "\n*** FATAL: abort() called ***\n");
    fflush(stderr);
    write_minidump(NULL);
}

static void install_crash_handlers(void) {
    SetUnhandledExceptionFilter(crash_filter);
    _set_invalid_parameter_handler(crt_invalid_parameter);
    signal(SIGABRT, abort_handler);
}

static void print_usage(void) {
    printf(
        "ext4fsp - libext2fs + WinFsp ext2/ext3/ext4 User-Space Driver & CLI\n\n"
        "Usage:\n"
        "  ext4fsp list [<image_or_disk>]\n"
        "  ext4fsp inspect <image_or_disk> [--partition <N>]\n"
        "  ext4fsp mkfs <image_path> --size-mb <N> [--label <label>]\n"
        "  ext4fsp ls <image_path> [<ext4_path>] [--partition <N>]\n"
        "  ext4fsp stat <image_path> <ext4_path> [--partition <N>]\n"
        "  ext4fsp cat <image_path> <ext4_path> [--partition <N>]\n"
        "  ext4fsp write <image_path> <ext4_path> <text_or_host_file> [--from-file] [--partition <N>]\n"
        "  ext4fsp mkdir <image_path> <ext4_path> [--partition <N>]\n"
        "  ext4fsp rm <image_path> <ext4_path> [--partition <N>]\n"
        "  ext4fsp mv <image_path> <src_path> <dst_path> [--partition <N>]\n"
        "  ext4fsp extents <image_or_disk> <ext4_path> [--partition <N>]\n"
        "  ext4fsp mount --source <image_or_disk> --mountpoint <M:|dir> [--partition <N>] [--rw|--ro]\n"
        "                [--overlay <ntfs_upper_dir>] [--no-direct-io] [--force-dirty] [--cache-blocks <N>] [--debug]\n"
        "  ext4fsp unmount <M:>     (remove a stale drive letter left by a terminated mount)\n"
        "\n"
        "  A mount stays active while the 'mount' process runs; press Ctrl+C in that console to unmount.\n"
    );
}

static uint32_t parse_u32_opt(int argc, char **argv, const char *flag, uint32_t def_val) {
    for (int i = 0; i + 1 < argc; i++) {
        if (strcmp(argv[i], flag) == 0) {
            return (uint32_t)strtoul(argv[i + 1], NULL, 10);
        }
    }
    return def_val;
}

static const char *parse_str_opt(int argc, char **argv, const char *flag, const char *def_val) {
    for (int i = 0; i + 1 < argc; i++) {
        if (strcmp(argv[i], flag) == 0) {
            return argv[i + 1];
        }
    }
    return def_val;
}

static int has_flag(int argc, char **argv, const char *flag) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0) return 1;
    }
    return 0;
}

static void print_partition_table(const char *source, const pt_table_result_t *res) {
    printf("Source: %s | Style: %s | SectorSize: %u | TotalBytes: %llu\n",
           source, res->style, res->effective_sector_size, (unsigned long long)res->total_size_bytes);
    for (uint32_t i = 0; i < res->count; i++) {
        const pt_partition_entry_t *p = &res->partitions[i];
        printf("  Part %-2u | Off: %-12llu | Size: %-12llu | Type: %-22s | FS: %-7s | Journal: %-14s | Label: \"%s\"\n",
               p->partition_number,
               (unsigned long long)p->offset_bytes,
               (unsigned long long)p->size_bytes,
               p->type_description,
               ext4_fs_kind_name(p->probe.fs_kind),
               ext4_journal_state_name(p->probe.journal_state),
               p->probe.volume_label);
    }
}

static int cmd_list(int argc, char **argv) {
    if (argc >= 3 && argv[2][0] != '-') {
        pt_table_result_t *res = (pt_table_result_t *)calloc(1, sizeof(*res));
        if (!res) return 1;
        if (pt_read_partitions(argv[2], 0, res) != 0) {
            fprintf(stderr, "Failed to inspect '%s'\n", argv[2]);
            free(res);
            return 1;
        }
        print_partition_table(argv[2], res);
        free(res);
        return 0;
    }

    scanned_disk_t *disks = (scanned_disk_t *)calloc(SCAN_MAX_DRIVES, sizeof(scanned_disk_t));
    uint32_t count = 0;
    if (!disks) return 1;
    disk_scanner_scan_physical_drives(disks, SCAN_MAX_DRIVES, &count);
    for (uint32_t i = 0; i < count; i++) {
        if (!disks[i].accessible) {
            printf("Source: %s | (Access Denied - Run as Administrator to inspect raw physical disk)\n",
                   disks[i].device_path);
        } else {
            print_partition_table(disks[i].device_path, &disks[i].table);
        }
    }
    free(disks);
    return 0;
}

static int cmd_inspect(int argc, char **argv) {
    if (argc < 3) {
        print_usage();
        return 1;
    }
    uint32_t part = parse_u32_opt(argc, argv, "--partition", 0);
    pt_table_result_t *res = (pt_table_result_t *)calloc(1, sizeof(*res));
    if (!res) return 1;
    if (pt_read_partitions(argv[2], 0, res) != 0) {
        fprintf(stderr, "Failed to read '%s'\n", argv[2]);
        free(res);
        return 1;
    }
    print_partition_table(argv[2], res);
    for (uint32_t i = 0; i < res->count; i++) {
        const pt_partition_entry_t *p = &res->partitions[i];
        if (part != 0 && p->partition_number != part) continue;
        if (p->probe.fs_kind == EXT4_FS_UNKNOWN) continue;
        printf("\n[Partition %u Detail]\n", p->partition_number);
        printf("  Filesystem   : %s\n", ext4_fs_kind_name(p->probe.fs_kind));
        printf("  Label        : %s\n", p->probe.volume_label);
        printf("  UUID         : %s\n", p->probe.uuid);
        printf("  BlockSize    : %u\n", p->probe.block_size);
        printf("  InodeSize    : %u\n", p->probe.inode_size);
        printf("  Blocks       : %llu\n", (unsigned long long)p->probe.blocks_count);
        printf("  TotalBytes   : %llu\n", (unsigned long long)p->probe.total_bytes);
        printf("  FreeBytes    : %llu\n", (unsigned long long)p->probe.free_bytes);
        printf("  Compat       : 0x%08X\n", p->probe.feature_compat);
        printf("  Incompat     : 0x%08X\n", p->probe.feature_incompat);
        printf("  RoCompat     : 0x%08X\n", p->probe.feature_ro_compat);
        printf("  JournalState : %s (inum=%u, s_start=%u, s_errno=%d)\n",
               ext4_journal_state_name(p->probe.journal_state),
               p->probe.journal_inum, p->probe.journal_start, p->probe.journal_errno);
    }
    free(res);
    return 0;
}

static int cmd_mkfs(int argc, char **argv) {
    if (argc < 3) {
        print_usage();
        return 1;
    }
    const char *img = argv[2];
    uint32_t size_mb = parse_u32_opt(argc, argv, "--size-mb", 16);
    const char *label = parse_str_opt(argc, argv, "--label", "EXT4FSP");
    int rc = ext4_vfs_mkfs(img, (uint64_t)size_mb * 1024ULL * 1024ULL, label);
    if (rc != 0) {
        fprintf(stderr, "mkfs failed: %d\n", rc);
        return 1;
    }
    printf("Created ext4 image '%s' (%u MB, label='%s')\n", img, size_mb, label);
    return 0;
}

static int mount_helper(const char *src, uint32_t part, int read_only, ext4_vfs_ctx_t **out_ctx) {
    ext4_mount_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = src;
    cfg.partition_number = part;
    cfg.read_only = read_only;
    cfg.ro_fallback_on_dirty = read_only;
    cfg.case_insensitive = 0;
    return ext4_vfs_mount(&cfg, out_ctx);
}

static int ls_print_cb(const char *name, const ext4_stat_t *st, void *user_ctx) {
    (void)user_ctx;
    char type_ch = st->is_dir ? 'd' : (st->is_symlink ? 'l' : '-');
    printf("%c%04o  links=%-3u  size=%-10llu  ino=%-6u  %s\n",
           type_ch,
           st->mode & 07777,
           st->hard_links,
           (unsigned long long)st->size,
           st->ino,
           name);
    return 0;
}

static int cmd_ls(int argc, char **argv) {
    if (argc < 3) {
        print_usage();
        return 1;
    }
    const char *src = argv[2];
    const char *path = (argc >= 4 && argv[3][0] != '-') ? argv[3] : "/";
    uint32_t part = parse_u32_opt(argc, argv, "--partition", 0);
    ext4_vfs_ctx_t *ctx = NULL;
    ext4_file_handle_t *h = NULL;
    int rc = mount_helper(src, part, 1, &ctx);
    if (rc != 0) {
        fprintf(stderr, "Mount failed: %d\n", rc);
        return 1;
    }
    rc = ext4_vfs_open(ctx, path, 0, &h);
    if (rc == 0) {
        if (!h->is_dir) {
            ext4_stat_t st;
            if (ext4_vfs_stat_ino(ctx, h->ino, path, &st) == 0) {
                ls_print_cb(path, &st, NULL);
            }
        } else {
            rc = ext4_vfs_readdir(ctx, h->ino, ls_print_cb, NULL);
        }
        ext4_vfs_close_handle(h);
    }
    ext4_vfs_unmount(ctx);
    if (rc != 0) {
        fprintf(stderr, "ls failed: %d\n", rc);
        return 1;
    }
    return 0;
}

static int cmd_stat(int argc, char **argv) {
    if (argc < 4) {
        print_usage();
        return 1;
    }
    const char *src = argv[2];
    const char *path = argv[3];
    uint32_t part = parse_u32_opt(argc, argv, "--partition", 0);
    ext4_vfs_ctx_t *ctx = NULL;
    ext4_stat_t st;
    int rc = mount_helper(src, part, 1, &ctx);
    if (rc != 0) {
        fprintf(stderr, "Mount failed: %d\n", rc);
        return 1;
    }
    rc = ext4_vfs_stat(ctx, path, &st);
    if (rc == 0) {
        printf("Path: %s\n", path);
        printf("  Inode      : %u\n", st.ino);
        printf("  Mode       : 0%o\n", st.mode);
        printf("  Size       : %llu\n", (unsigned long long)st.size);
        printf("  Links      : %u\n", st.hard_links);
        printf("  WinAttrs   : 0x%08X\n", st.file_attributes);
        if (st.is_symlink) {
            char target[1024];
            if (ext4_vfs_readlink(ctx, path, target, sizeof(target)) == 0) {
                printf("  SymlinkTo  : %s\n", target);
            }
        }
    }
    ext4_vfs_unmount(ctx);
    if (rc != 0) {
        fprintf(stderr, "stat failed: %d\n", rc);
        return 1;
    }
    return 0;
}

static int cmd_cat(int argc, char **argv) {
    if (argc < 4) {
        print_usage();
        return 1;
    }
    const char *src = argv[2];
    const char *path = argv[3];
    uint32_t part = parse_u32_opt(argc, argv, "--partition", 0);
    ext4_vfs_ctx_t *ctx = NULL;
    ext4_file_handle_t *h = NULL;
    int rc = mount_helper(src, part, 1, &ctx);
    if (rc != 0) {
        fprintf(stderr, "Mount failed: %d\n", rc);
        return 1;
    }
    rc = ext4_vfs_open(ctx, path, 0, &h);
    if (rc == 0) {
        uint8_t buf[65536];
        uint64_t off = 0;
        while (1) {
            uint32_t got = 0;
            rc = ext4_vfs_read(ctx, h->ino, buf, off, sizeof(buf), &got);
            if (rc != 0 || got == 0) break;
            fwrite(buf, 1, got, stdout);
            off += got;
        }
        ext4_vfs_close_handle(h);
    }
    ext4_vfs_unmount(ctx);
    if (rc != 0) {
        fprintf(stderr, "cat failed: %d\n", rc);
        return 1;
    }
    return 0;
}

static int cmd_write(int argc, char **argv) {
    if (argc < 5) {
        print_usage();
        return 1;
    }
    const char *src = argv[2];
    const char *path = argv[3];
    const char *payload = argv[4];
    int from_file = has_flag(argc, argv, "--from-file");
    uint32_t part = parse_u32_opt(argc, argv, "--partition", 0);

    uint8_t *data_buf = NULL;
    size_t data_len = 0;
    if (from_file) {
        FILE *fp = fopen(payload, "rb");
        if (!fp) {
            fprintf(stderr, "Cannot open host file '%s'\n", payload);
            return 1;
        }
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data_len = sz > 0 ? (size_t)sz : 0;
        data_buf = (uint8_t *)malloc(data_len ? data_len : 1);
        if (data_len > 0) fread(data_buf, 1, data_len, fp);
        fclose(fp);
    } else {
        data_len = strlen(payload);
        data_buf = (uint8_t *)_strdup(payload);
    }

    ext4_vfs_ctx_t *ctx = NULL;
    int rc = mount_helper(src, part, 0, &ctx);
    if (rc != 0) {
        free(data_buf);
        fprintf(stderr, "Mount RW failed: %d\n", rc);
        return 1;
    }

    ext4_file_handle_t *h = NULL;
    rc = ext4_vfs_open(ctx, path, 1, &h);
    if (rc == -ENOENT) {
        rc = ext4_vfs_create(ctx, path, 0, 0644, FILE_ATTRIBUTE_ARCHIVE, NULL);
        if (rc == 0) {
            rc = ext4_vfs_open(ctx, path, 1, &h);
        }
    } else if (rc == 0) {
        rc = ext4_vfs_truncate(ctx, h->ino, 0);
    }

    if (rc == 0 && h) {
        uint64_t off = 0;
        while (off < data_len) {
            uint32_t chunk = (data_len - off > 1048576ULL) ? 1048576U : (uint32_t)(data_len - off);
            uint32_t written = 0;
            rc = ext4_vfs_write(ctx, h->ino, data_buf + off, off, chunk, 0, &written, NULL);
            if (rc != 0) break;
            off += written;
        }
        ext4_vfs_close_handle(h);
    }
    ext4_vfs_unmount(ctx);
    free(data_buf);

    if (rc != 0) {
        fprintf(stderr, "write failed: %d\n", rc);
        return 1;
    }
    printf("Wrote %zu bytes to %s\n", data_len, path);
    return 0;
}

static int cmd_mkdir(int argc, char **argv) {
    if (argc < 4) {
        print_usage();
        return 1;
    }
    uint32_t part = parse_u32_opt(argc, argv, "--partition", 0);
    ext4_vfs_ctx_t *ctx = NULL;
    int rc = mount_helper(argv[2], part, 0, &ctx);
    if (rc != 0) return 1;
    rc = ext4_vfs_mkdir(ctx, argv[3], 0755, NULL);
    ext4_vfs_unmount(ctx);
    if (rc != 0) {
        fprintf(stderr, "mkdir failed: %d\n", rc);
        return 1;
    }
    printf("Created directory %s\n", argv[3]);
    return 0;
}

static int cmd_rm(int argc, char **argv) {
    if (argc < 4) {
        print_usage();
        return 1;
    }
    uint32_t part = parse_u32_opt(argc, argv, "--partition", 0);
    ext4_vfs_ctx_t *ctx = NULL;
    int rc = mount_helper(argv[2], part, 0, &ctx);
    if (rc != 0) return 1;
    rc = ext4_vfs_unlink(ctx, argv[3]);
    ext4_vfs_unmount(ctx);
    if (rc != 0) {
        fprintf(stderr, "rm failed: %d\n", rc);
        return 1;
    }
    printf("Removed %s\n", argv[3]);
    return 0;
}

static int cmd_mv(int argc, char **argv) {
    if (argc < 5) {
        print_usage();
        return 1;
    }
    uint32_t part = parse_u32_opt(argc, argv, "--partition", 0);
    ext4_vfs_ctx_t *ctx = NULL;
    int rc = mount_helper(argv[2], part, 0, &ctx);
    if (rc != 0) return 1;
    rc = ext4_vfs_rename(ctx, argv[3], argv[4], 1);
    ext4_vfs_unmount(ctx);
    if (rc != 0) {
        fprintf(stderr, "mv failed: %d\n", rc);
        return 1;
    }
    printf("Moved %s -> %s\n", argv[3], argv[4]);
    return 0;
}

static int cmd_extents(int argc, char **argv) {
    if (argc < 4) {
        print_usage();
        return 1;
    }
    uint32_t part = parse_u32_opt(argc, argv, "--partition", 0);
    ext4_vfs_ctx_t *ctx = NULL;
    int rc = mount_helper(argv[2], part, 1, &ctx);
    if (rc != 0) return 1;

    ext4_extent_map_t *map = NULL;
    rc = ext4_vfs_get_extents(ctx, argv[3], &map);
    if (rc != 0) {
        fprintf(stderr, "extents failed for '%s': %d\n", argv[3], rc);
        ext4_vfs_unmount(ctx);
        return 1;
    }

    const char *kind_str = (map->kind == EXT4_STORAGE_EXTENTS) ? "extents" :
                           (map->kind == EXT4_STORAGE_INDIRECT) ? "indirect" : "inline";
    printf("Path: %s | Inode: %u | Size: %llu bytes | BlockSize: %u | Kind: %s | Runs: %zu | PartOff: %llu\n",
           argv[3], map->ino, (unsigned long long)map->file_size, map->block_size,
           kind_str, map->run_count, (unsigned long long)ctx->partition_offset);
    if (map->kind == EXT4_STORAGE_INLINE) {
        printf("  [INLINE] %zu bytes stored directly inside inode\n", map->inline_size);
    } else {
        for (size_t i = 0; i < map->run_count; i++) {
            const ext4_extent_run_t *r = &map->runs[i];
            printf("  [%3zu] Logical [%12llu .. %12llu) -> PartRel %12llu (DiskAbs %12llu) Len=%llu%s\n",
                   i,
                   (unsigned long long)r->logical_offset,
                   (unsigned long long)(r->logical_offset + r->length),
                   (unsigned long long)r->physical_offset,
                   (unsigned long long)(ctx->partition_offset + r->physical_offset),
                   (unsigned long long)r->length,
                   r->is_uninit ? " [UNINIT]" : "");
        }
    }
    ext4_extent_map_release(map);
    ext4_vfs_unmount(ctx);
    return 0;
}

static int cmd_mount(int argc, char **argv) {
    const char *src = parse_str_opt(argc, argv, "--source", NULL);
    const char *mp = parse_str_opt(argc, argv, "--mountpoint", NULL);
    const char *overlay_dir = parse_str_opt(argc, argv, "--overlay", NULL);
    uint32_t part = parse_u32_opt(argc, argv, "--partition", 0);
    uint32_t cache_blocks = parse_u32_opt(argc, argv, "--cache-blocks", 2048);
    int rw = has_flag(argc, argv, "--rw");
    int ro = has_flag(argc, argv, "--ro");
    int force_dirty = has_flag(argc, argv, "--force-dirty");
    int no_direct_io = has_flag(argc, argv, "--no-direct-io");
    uint32_t debug_log = has_flag(argc, argv, "--debug") ? (uint32_t)-1 : 0;

    if (!src || !mp) {
        print_usage();
        return 1;
    }

    ext4_mount_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = src;
    cfg.partition_number = part;
    cfg.max_cache_blocks = cache_blocks;
    if (overlay_dir && overlay_dir[0] != '\0') {
        /* Overlay keeps lower ext4 strictly RO while permitting writes to upperdir unless --ro is forced */
        cfg.read_only = ro ? 1 : 0;
        cfg.overlay_upper_dir = overlay_dir;
    } else {
        cfg.read_only = (ro || !rw) ? 1 : 0;
    }
    cfg.ro_fallback_on_dirty = cfg.read_only;
    cfg.force_rw_on_dirty = force_dirty;
    cfg.case_insensitive = 1;
    cfg.disable_direct_io = no_direct_io;

    ext4_vfs_ctx_t *ctx = NULL;
    int rc = ext4_vfs_mount(&cfg, &ctx);
    if (rc != 0) {
        fprintf(stderr, "Failed to mount '%s' (err=%d)\n", src, rc);
        return 1;
    }

    wchar_t wmp[256];
    if (ext4_utf8_to_wide(mp, wmp, _countof(wmp)) != 0) {
        ext4_vfs_unmount(ctx);
        return 1;
    }

    winfsp_mount_handle_t *mh = NULL;
    rc = winfsp_driver_start(ctx, wmp, debug_log, &mh);
    if (rc != 0) {
        fprintf(stderr, "WinFsp mount failed (err=%d). Make sure WinFsp runtime is installed.\n", rc);
        ext4_vfs_unmount(ctx);
        return 1;
    }

    const char *mode_str = ctx->overlay
                               ? (ctx->read_only ? "OVERLAY-RO" : "OVERLAY-RW(lower=RO)")
                               : (ctx->read_only ? "RO" : "RW");
    printf("Mounted '%s' (partition=%u, mode=%s, direct_io=%s%s%s) at %s. Press Ctrl+C to unmount...\n",
           src, part, mode_str, ctx->disable_direct_io ? "off" : "on",
           overlay_dir ? ", upper=" : "", overlay_dir ? overlay_dir : "", mp);
    fflush(stdout);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    while (!g_stop_requested) {
        Sleep(200);
    }
    printf("Unmounting %s...\n", mp);
    winfsp_driver_stop(mh);
    ext4_vfs_unmount(ctx);
    printf("Unmounted cleanly.\n");
    return 0;
}

static int cmd_unmount(int argc, char **argv) {
    wchar_t wdrive[8];
    if (argc < 3) {
        print_usage();
        return 1;
    }
    if (ext4_utf8_to_wide(argv[2], wdrive, _countof(wdrive)) != 0) return 1;
    if (wdrive[0] && wdrive[1] == 0) { wdrive[1] = L':'; wdrive[2] = 0; }
    if (wdrive[2] == L'\\') wdrive[2] = 0;
    if (winfsp_driver_force_unmount(wdrive) != 0) {
        fprintf(stderr, "No mount found for %s (run from the same elevation level that mounted it).\n", argv[2]);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    install_crash_handlers();
    if (argc < 2) {
        print_usage();
        return 1;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "unmount") == 0 || strcmp(cmd, "umount") == 0) return cmd_unmount(argc, argv);
    if (strcmp(cmd, "list") == 0) return cmd_list(argc, argv);
    if (strcmp(cmd, "inspect") == 0) return cmd_inspect(argc, argv);
    if (strcmp(cmd, "mkfs") == 0) return cmd_mkfs(argc, argv);
    if (strcmp(cmd, "ls") == 0) return cmd_ls(argc, argv);
    if (strcmp(cmd, "stat") == 0) return cmd_stat(argc, argv);
    if (strcmp(cmd, "cat") == 0) return cmd_cat(argc, argv);
    if (strcmp(cmd, "write") == 0) return cmd_write(argc, argv);
    if (strcmp(cmd, "mkdir") == 0) return cmd_mkdir(argc, argv);
    if (strcmp(cmd, "rm") == 0) return cmd_rm(argc, argv);
    if (strcmp(cmd, "mv") == 0) return cmd_mv(argc, argv);
    if (strcmp(cmd, "extents") == 0) return cmd_extents(argc, argv);
    if (strcmp(cmd, "mount") == 0) return cmd_mount(argc, argv);

    print_usage();
    return 1;
}
