#include "../common/ext4_errno.h"
#include "../disk/disk_scanner.h"
#include "../disk/partition_table.h"
#include "../disk/superblock_probe.h"
#include "../vfs/ext4_vfs.h"
#include "../extent/extent_map.h"
#include "../driver/winfsp_driver.h"
#include "../trace/perfetto_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <dbghelp.h>
#include <psapi.h>
#include <shellapi.h>

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
    wcscat_s(path, MAX_PATH, L"zeroext_crash.dmp");
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
        "ZeroExt (zeroext) - libext2fs + WinFsp ext2/ext3/ext4 User-Space Driver & CLI\n\n"
        "Usage:\n"
        "  zeroext list [<image_or_disk>] [--json]\n"
        "  zeroext inspect <image_or_disk> [--partition <N>] [--json]\n"
        "  zeroext mkfs <image_path> --size-mb <N> [--label <label>]\n"
        "  zeroext ls <image_path> [<ext4_path>] [--partition <N>] [--json]\n"
        "  zeroext stat <image_path> <ext4_path> [--partition <N>]\n"
        "  zeroext cat <image_path> <ext4_path> [--partition <N>]\n"
        "  zeroext write <image_path> <ext4_path> <text_or_host_file> [--from-file] [--partition <N>]\n"
        "  zeroext mkdir <image_path> <ext4_path> [--partition <N>]\n"
        "  zeroext rm <image_path> <ext4_path> [--partition <N>]\n"
        "  zeroext mv <image_path> <src_path> <dst_path> [--partition <N>]\n"
        "  zeroext extents <image_or_disk> <ext4_path> [--partition <N>]\n"
        "  zeroext mount --source <image_or_disk> --mountpoint <M:|dir> [--partition <N>] [--rw | --overlay <ntfs_upper_dir>]\n"
        "                [--no-direct-io | --direct-io] [--file-info-timeout <ms|infinite>] [--ro-fallback] [--force-dirty]\n"
        "                [--cache-blocks <N>] [--debug] [--stop-event <name>] [--json-events] [--stats-interval <ms>]\n"
        "                [--trace <output_json>] [--trace-events <N>]\n"
        "  zeroext unmount <M:>     (remove a stale drive letter left by a terminated mount)\n"
        "\n"
        "  Default mount mode is Read-Only with Direct Extent I/O enabled.\n"
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

static const char *find_positional_skip_opts(int argc, char **argv, int start_idx, int target_pos) {
    int pos = 0;
    for (int i = start_idx; i < argc; i++) {
        if (strcmp(argv[i], "--partition") == 0 ||
            strcmp(argv[i], "--size-mb") == 0 ||
            strcmp(argv[i], "--label") == 0 ||
            strcmp(argv[i], "--cache-blocks") == 0 ||
            strcmp(argv[i], "--overlay") == 0 ||
            strcmp(argv[i], "--source") == 0 ||
            strcmp(argv[i], "--mountpoint") == 0 ||
            strcmp(argv[i], "--stop-event") == 0 ||
            strcmp(argv[i], "--stats-interval") == 0 ||
            strcmp(argv[i], "--trace") == 0 ||
            strcmp(argv[i], "--trace-events") == 0) {
            i++;
            continue;
        }
        if (argv[i][0] == '-') {
            continue;
        }
        if (pos == target_pos) {
            return argv[i];
        }
        pos++;
    }
    return NULL;
}

static void print_json_str(const char *s) {
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    putchar('"');
    while (*p) {
        unsigned char c = *p++;
        switch (c) {
        case '"':  fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\b': fputs("\\b", stdout); break;
        case '\f': fputs("\\f", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        default:
            if (c < 0x20) {
                printf("\\u%04x", (unsigned)c);
            } else {
                putchar((int)c);
            }
            break;
        }
    }
    putchar('"');
}

static void print_partition_json(const pt_partition_entry_t *p) {
    printf("{");
    printf("\"partition_number\":%u,", p->partition_number);
    printf("\"start_sector\":%llu,", (unsigned long long)p->start_sector);
    printf("\"sector_count\":%llu,", (unsigned long long)p->sector_count);
    printf("\"offset_bytes\":%llu,", (unsigned long long)p->offset_bytes);
    printf("\"size_bytes\":%llu,", (unsigned long long)p->size_bytes);
    printf("\"type_id\":"); print_json_str(p->type_id); putchar(',');
    printf("\"type_description\":"); print_json_str(p->type_description); putchar(',');
    printf("\"partition_name\":"); print_json_str(p->partition_name); putchar(',');
    printf("\"fs_kind\":"); print_json_str(ext4_fs_kind_name(p->probe.fs_kind)); putchar(',');
    printf("\"journal_state\":"); print_json_str(ext4_journal_state_name(p->probe.journal_state)); putchar(',');
    printf("\"volume_label\":"); print_json_str(p->probe.volume_label); putchar(',');
    printf("\"uuid\":"); print_json_str(p->probe.uuid); putchar(',');
    printf("\"block_size\":%u,", p->probe.block_size);
    printf("\"inode_size\":%u,", p->probe.inode_size);
    printf("\"blocks_count\":%llu,", (unsigned long long)p->probe.blocks_count);
    printf("\"total_bytes\":%llu,", (unsigned long long)p->probe.total_bytes);
    printf("\"free_bytes\":%llu,", (unsigned long long)p->probe.free_bytes);
    printf("\"feature_compat\":%u,", p->probe.feature_compat);
    printf("\"feature_incompat\":%u,", p->probe.feature_incompat);
    printf("\"feature_ro_compat\":%u,", p->probe.feature_ro_compat);
    printf("\"journal_inum\":%u,", p->probe.journal_inum);
    printf("\"journal_start\":%u,", p->probe.journal_start);
    printf("\"journal_errno\":%d,", p->probe.journal_errno);
    printf("\"state_flags\":%u", (unsigned)p->probe.state_flags);
    printf("}");
}

static void print_disk_json(const char *device_path, int drive_index, int accessible,
                            const pt_table_result_t *res, uint32_t filter_part) {
    printf("{");
    printf("\"device_path\":"); print_json_str(device_path); putchar(',');
    printf("\"drive_index\":%d,", drive_index);
    printf("\"accessible\":%s,", accessible ? "true" : "false");
    printf("\"style\":"); print_json_str((accessible && res) ? res->style : ""); putchar(',');
    printf("\"effective_sector_size\":%u,", (accessible && res) ? res->effective_sector_size : 0U);
    printf("\"total_size_bytes\":%llu,", (unsigned long long)((accessible && res) ? res->total_size_bytes : 0ULL));
    printf("\"partitions\":[");
    if (accessible && res) {
        int first = 1;
        for (uint32_t i = 0; i < res->count; i++) {
            const pt_partition_entry_t *p = &res->partitions[i];
            if (filter_part != 0 && p->partition_number != filter_part) continue;
            if (!first) putchar(',');
            print_partition_json(p);
            first = 0;
        }
    }
    printf("]}");
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
    int json = has_flag(argc, argv, "--json");
    const char *target = find_positional_skip_opts(argc, argv, 2, 0);

    if (target) {
        pt_table_result_t *res = (pt_table_result_t *)calloc(1, sizeof(*res));
        if (!res) return 1;
        if (pt_read_partitions(target, 0, res) != 0) {
            fprintf(stderr, "Failed to inspect '%s'\n", target);
            free(res);
            return 1;
        }
        if (json) {
            printf("{\"is_elevated\":%s,\"winfsp_available\":%s,\"disks\":[",
                   winfsp_driver_is_process_elevated() ? "true" : "false",
                   winfsp_driver_is_runtime_available() ? "true" : "false");
            print_disk_json(target, -1, 1, res, 0);
            printf("]}\n");
            fflush(stdout);
        } else {
            print_partition_table(target, res);
        }
        free(res);
        return 0;
    }

    scanned_disk_t *disks = (scanned_disk_t *)calloc(SCAN_MAX_DRIVES, sizeof(scanned_disk_t));
    uint32_t count = 0;
    if (!disks) return 1;
    disk_scanner_scan_physical_drives(disks, SCAN_MAX_DRIVES, &count);
    if (json) {
        printf("{\"is_elevated\":%s,\"winfsp_available\":%s,\"disks\":[",
               winfsp_driver_is_process_elevated() ? "true" : "false",
               winfsp_driver_is_runtime_available() ? "true" : "false");
        for (uint32_t i = 0; i < count; i++) {
            if (i > 0) putchar(',');
            print_disk_json(disks[i].device_path, (int)disks[i].drive_index,
                            disks[i].accessible, &disks[i].table, 0);
        }
        printf("]}\n");
        fflush(stdout);
    } else {
        for (uint32_t i = 0; i < count; i++) {
            if (!disks[i].accessible) {
                printf("Source: %s | (Access Denied - Run as Administrator to inspect raw physical disk)\n",
                       disks[i].device_path);
            } else {
                print_partition_table(disks[i].device_path, &disks[i].table);
            }
        }
    }
    free(disks);
    return 0;
}

static int cmd_inspect(int argc, char **argv) {
    int json = has_flag(argc, argv, "--json");
    const char *src = find_positional_skip_opts(argc, argv, 2, 0);
    if (!src) {
        print_usage();
        return 1;
    }
    uint32_t part = parse_u32_opt(argc, argv, "--partition", 0);
    pt_table_result_t *res = (pt_table_result_t *)calloc(1, sizeof(*res));
    if (!res) return 1;
    if (pt_read_partitions(src, 0, res) != 0) {
        fprintf(stderr, "Failed to read '%s'\n", src);
        free(res);
        return 1;
    }
    if (json) {
        printf("{\"is_elevated\":%s,\"winfsp_available\":%s,\"disks\":[",
               winfsp_driver_is_process_elevated() ? "true" : "false",
               winfsp_driver_is_runtime_available() ? "true" : "false");
        print_disk_json(src, -1, 1, res, part);
        printf("]}\n");
        fflush(stdout);
    } else {
        print_partition_table(src, res);
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
    const char *label = parse_str_opt(argc, argv, "--label", "ZEROEXT");
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

typedef struct ls_json_entry {
    char     name[1024];
    int      is_dir;
    int      is_symlink;
    uint64_t size;
    uint32_t mode;
    uint32_t ino;
    uint32_t hard_links;
    uint64_t last_write_filetime;
} ls_json_entry_t;

typedef struct ls_json_list {
    ls_json_entry_t *items;
    size_t           count;
    size_t           cap;
    int              oom;
} ls_json_list_t;

static int ls_json_collect_cb(const char *name, const ext4_stat_t *st, void *user_ctx) {
    ls_json_list_t *list = (ls_json_list_t *)user_ctx;
    if (list->count == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 64;
        ls_json_entry_t *nb = (ls_json_entry_t *)realloc(list->items, new_cap * sizeof(*nb));
        if (!nb) {
            list->oom = 1;
            return 1;
        }
        list->items = nb;
        list->cap = new_cap;
    }
    {
        ls_json_entry_t *e = &list->items[list->count++];
        memset(e, 0, sizeof(*e));
        strncpy_s(e->name, sizeof(e->name), name ? name : "", _TRUNCATE);
        e->is_dir = st->is_dir ? 1 : 0;
        e->is_symlink = st->is_symlink ? 1 : 0;
        e->size = st->size;
        e->mode = st->mode;
        e->ino = st->ino;
        e->hard_links = st->hard_links;
        e->last_write_filetime = st->last_write_time;
    }
    return 0;
}

static int cmd_ls(int argc, char **argv) {
    const char *src = find_positional_skip_opts(argc, argv, 2, 0);
    const char *path_arg = find_positional_skip_opts(argc, argv, 2, 1);
    const char *path = path_arg ? path_arg : "/";
    int json = has_flag(argc, argv, "--json");
    if (!src) {
        print_usage();
        return 1;
    }
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
        if (json) {
            ls_json_list_t list;
            memset(&list, 0, sizeof(list));
            if (!h->is_dir) {
                ext4_stat_t st;
                rc = ext4_vfs_stat_ino(ctx, h->ino, path, &st);
                if (rc == 0) {
                    ls_json_collect_cb(path, &st, &list);
                }
            } else {
                rc = ext4_vfs_readdir(ctx, h->ino, ls_json_collect_cb, &list);
            }
            if (list.oom) rc = -ENOMEM;
            if (rc == 0) {
                printf("{\"source\":"); print_json_str(src);
                printf(",\"partition\":%u", part);
                printf(",\"path\":"); print_json_str(path);
                printf(",\"entries\":[");
                for (size_t i = 0; i < list.count; i++) {
                    const ls_json_entry_t *e = &list.items[i];
                    if (i > 0) putchar(',');
                    printf("{\"name\":"); print_json_str(e->name);
                    printf(",\"is_dir\":%s", e->is_dir ? "true" : "false");
                    printf(",\"is_symlink\":%s", e->is_symlink ? "true" : "false");
                    printf(",\"size\":%llu", (unsigned long long)e->size);
                    printf(",\"mode\":%u", e->mode);
                    printf(",\"ino\":%u", e->ino);
                    printf(",\"hard_links\":%u", e->hard_links);
                    printf(",\"last_write_filetime\":%llu}", (unsigned long long)e->last_write_filetime);
                }
                printf("]}\n");
                fflush(stdout);
            }
            free(list.items);
        } else {
            if (!h->is_dir) {
                ext4_stat_t st;
                if (ext4_vfs_stat_ino(ctx, h->ino, path, &st) == 0) {
                    ls_print_cb(path, &st, NULL);
                }
            } else {
                rc = ext4_vfs_readdir(ctx, h->ino, ls_print_cb, NULL);
            }
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
        wchar_t wpayload[1024];
        FILE *fp = NULL;
        if (ext4_utf8_to_wide(payload, wpayload, _countof(wpayload)) == 0) {
            fp = _wfopen(wpayload, L"rb");
        }
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

static void emit_mount_error_json(int json_events, const char *stage, int code, const char *msg) {
    if (!json_events) return;
    printf("{\"event\":\"error\",\"stage\":");
    print_json_str(stage);
    printf(",\"code\":%d,\"message\":", code);
    print_json_str(msg);
    printf("}\n");
    fflush(stdout);
}

static int cmd_mount(int argc, char **argv) {
    const char *src = parse_str_opt(argc, argv, "--source", NULL);
    const char *mp = parse_str_opt(argc, argv, "--mountpoint", NULL);
    const char *overlay_dir = parse_str_opt(argc, argv, "--overlay", NULL);
    const char *stop_event_name = parse_str_opt(argc, argv, "--stop-event", NULL);
    const char *trace_path = parse_str_opt(argc, argv, "--trace", NULL);
    const char *fi_timeout_str = parse_str_opt(argc, argv, "--file-info-timeout", NULL);
    uint32_t part = parse_u32_opt(argc, argv, "--partition", 0);
    uint32_t cache_blocks = parse_u32_opt(argc, argv, "--cache-blocks", 2048);
    uint32_t stats_interval = parse_u32_opt(argc, argv, "--stats-interval", 0);
    uint32_t trace_events = parse_u32_opt(argc, argv, "--trace-events", 0);
    int has_fi_timeout = 0;
    uint32_t fi_timeout_ms = 0;
    int rw = has_flag(argc, argv, "--rw");
    int ro_fallback = has_flag(argc, argv, "--ro-fallback");
    int force_dirty = has_flag(argc, argv, "--force-dirty");
    int direct_io = has_flag(argc, argv, "--direct-io");
    int no_direct_io = has_flag(argc, argv, "--no-direct-io");
    int json_events = has_flag(argc, argv, "--json-events");
    uint32_t debug_log = has_flag(argc, argv, "--debug") ? (uint32_t)-1 : 0;
    HANDLE stop_event = NULL;
    int tracing = 0;

    if (json_events) {
        setvbuf(stdout, NULL, _IONBF, 0);
    }

    if (!src || !mp) {
        emit_mount_error_json(json_events, "args", -EINVAL, "Missing --source or --mountpoint.");
        if (!json_events) print_usage();
        return 1;
    }

    if (rw && overlay_dir && overlay_dir[0] != '\0') {
        const char *msg = "Error: --rw and --overlay cannot be used together "
                          "(--overlay writes to the NTFS upperdir while keeping lower ext4 read-only).";
        fprintf(stderr, "%s\n", msg);
        emit_mount_error_json(json_events, "args", -EINVAL, msg);
        return 1;
    }

    if (direct_io && no_direct_io) {
        const char *msg = "Error: --direct-io and --no-direct-io cannot be used together.";
        fprintf(stderr, "%s\n", msg);
        emit_mount_error_json(json_events, "args", -EINVAL, msg);
        return 1;
    }

    if (direct_io && rw) {
        const char *msg = "Error: --direct-io is only supported in Read-Only (default) or --overlay mode.";
        fprintf(stderr, "%s\n", msg);
        emit_mount_error_json(json_events, "args", -EINVAL, msg);
        return 1;
    }

    if (fi_timeout_str && fi_timeout_str[0] != '\0') {
        if (_stricmp(fi_timeout_str, "infinite") == 0 || strcmp(fi_timeout_str, "-1") == 0) {
            has_fi_timeout = 1;
            fi_timeout_ms = (uint32_t)-1;
        } else {
            char *endp = NULL;
            unsigned long val;
            errno = 0;
            val = (fi_timeout_str[0] == '-') ? 0UL : strtoul(fi_timeout_str, &endp, 10);
            if (fi_timeout_str[0] == '-' || errno == ERANGE || !endp || *endp != '\0') {
                const char *msg = "Error: invalid --file-info-timeout value (expected non-negative ms or 'infinite').";
                fprintf(stderr, "%s\n", msg);
                emit_mount_error_json(json_events, "args", -EINVAL, msg);
                return 1;
            }
            has_fi_timeout = 1;
            fi_timeout_ms = (uint32_t)val;
        }
    }

    if (stop_event_name && stop_event_name[0] != '\0') {
        wchar_t wevent[256];
        if (ext4_utf8_to_wide(stop_event_name, wevent, _countof(wevent)) != 0) {
            emit_mount_error_json(json_events, "args", -EINVAL, "Invalid --stop-event name.");
            return 1;
        }
        stop_event = CreateEventW(NULL, TRUE, FALSE, wevent);
        if (!stop_event) {
            stop_event = OpenEventW(SYNCHRONIZE, FALSE, wevent);
        }
        if (!stop_event) {
            fprintf(stderr, "Failed to open/create stop event '%s' (err=%lu).\n",
                    stop_event_name, (unsigned long)GetLastError());
            emit_mount_error_json(json_events, "args", -(int)GetLastError(), "Failed to open/create stop event.");
            return 1;
        }
    }

    if (trace_path && trace_path[0] != '\0') {
        int trc = ztrace_init(trace_path, trace_events);
        if (trc != 0) {
            char errbuf[256];
            _snprintf_s(errbuf, sizeof(errbuf), _TRUNCATE,
                        "Failed to open Perfetto trace output '%s' (err=%d).", trace_path, trc);
            fprintf(stderr, "%s\n", errbuf);
            emit_mount_error_json(json_events, "args", trc, errbuf);
            if (stop_event) CloseHandle(stop_event);
            return 1;
        }
        ztrace_set_thread_name("CLI-Main");
        tracing = 1;
    }

    ext4_mount_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = src;
    cfg.partition_number = part;
    cfg.max_cache_blocks = cache_blocks;
    if (overlay_dir && overlay_dir[0] != '\0') {
        /* Overlay keeps lower ext4 strictly RO while permitting writes to upperdir */
        cfg.read_only = 0;
        cfg.overlay_upper_dir = overlay_dir;
    } else {
        /* Read-Only by default; pass --rw to enable Read-Write mode */
        cfg.read_only = rw ? 0 : 1;
    }
    cfg.ro_fallback_on_dirty = (cfg.read_only || ro_fallback) ? 1 : 0;
    cfg.force_rw_on_dirty = force_dirty;
    cfg.case_insensitive = 1;
    cfg.enable_direct_io = direct_io ? 1 : 0;
    cfg.disable_direct_io = no_direct_io ? 1 : 0;
    cfg.has_file_info_timeout = has_fi_timeout;
    cfg.file_info_timeout = fi_timeout_ms;

    ext4_vfs_ctx_t *ctx = NULL;
    int rc = ext4_vfs_mount(&cfg, &ctx);
    if (rc != 0) {
        char errbuf[256];
        _snprintf_s(errbuf, sizeof(errbuf), _TRUNCATE, "Failed to mount '%s' (err=%d)", src, rc);
        fprintf(stderr, "%s\n", errbuf);
        emit_mount_error_json(json_events, "vfs_mount", rc, errbuf);
        if (tracing) ztrace_shutdown(NULL);
        if (stop_event) CloseHandle(stop_event);
        return 1;
    }

    if (stats_interval > 0) {
        ctx->stats_enabled = 1;
    }

    wchar_t wmp[256];
    if (ext4_utf8_to_wide(mp, wmp, _countof(wmp)) != 0) {
        emit_mount_error_json(json_events, "args", -EINVAL, "Invalid mountpoint path.");
        ext4_vfs_unmount(ctx);
        if (tracing) ztrace_shutdown(NULL);
        if (stop_event) CloseHandle(stop_event);
        return 1;
    }

    winfsp_mount_handle_t *mh = NULL;
    rc = winfsp_driver_start(ctx, wmp, debug_log, &mh);
    if (rc != 0) {
        char errbuf[256];
        _snprintf_s(errbuf, sizeof(errbuf), _TRUNCATE,
                    "WinFsp mount failed (err=%d). Make sure WinFsp runtime is installed.", rc);
        fprintf(stderr, "%s\n", errbuf);
        emit_mount_error_json(json_events, "winfsp_start", rc, errbuf);
        ext4_vfs_unmount(ctx);
        if (tracing) ztrace_shutdown(NULL);
        if (stop_event) CloseHandle(stop_event);
        return 1;
    }

    const char *mode_str = ctx->overlay
                               ? (ctx->read_only ? "OVERLAY-RO" : "OVERLAY-RW(lower=RO)")
                               : (ctx->read_only ? "RO" : "RW");
    if (json_events) {
        printf("{\"event\":\"mounted\",\"source\":"); print_json_str(src);
        printf(",\"partition\":%u", part);
        printf(",\"mountpoint\":"); print_json_str(mp);
        printf(",\"mode\":"); print_json_str(mode_str);
        printf(",\"read_only\":%s", ctx->read_only ? "true" : "false");
        printf(",\"lower_read_only\":%s", ctx->lower_read_only ? "true" : "false");
        printf(",\"direct_io\":%s", ctx->enable_direct_io ? "true" : "false");
        printf(",\"fell_back_to_ro\":%s", ctx->fell_back_to_ro ? "true" : "false");
        printf(",\"overlay_upper\":"); print_json_str(overlay_dir ? overlay_dir : "");
        printf(",\"volume_label\":"); print_json_str(ctx->volume_label);
        printf("}\n");
        fflush(stdout);
    } else {
        printf("Mounted '%s' (partition=%u, mode=%s, direct_io=%s%s%s) at %s. Press Ctrl+C to unmount...\n",
               src, part, mode_str, ctx->enable_direct_io ? "on" : "off",
               overlay_dir ? ", upper=" : "", overlay_dir ? overlay_dir : "", mp);
        if (tracing) {
            printf("Perfetto tracing active -> %s\n", trace_path);
        }
        fflush(stdout);
    }

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    {
        ULONGLONG last_stats_tick = GetTickCount64();
        while (!g_stop_requested) {
            DWORD wait_ms = (stats_interval > 0 && stats_interval < 200U) ? stats_interval : 200U;
            if (stop_event) {
                if (WaitForSingleObject(stop_event, wait_ms) == WAIT_OBJECT_0) {
                    g_stop_requested = 1;
                    break;
                }
            } else {
                Sleep(wait_ms);
            }
            if (json_events && stats_interval > 0) {
                ULONGLONG now = GetTickCount64();
                if (now - last_stats_tick >= (ULONGLONG)stats_interval) {
                    LONG64 rb = InterlockedCompareExchange64(&ctx->stat_read_bytes, 0, 0);
                    LONG64 wb = InterlockedCompareExchange64(&ctx->stat_write_bytes, 0, 0);
                    LONG64 ro = InterlockedCompareExchange64(&ctx->stat_read_ops, 0, 0);
                    LONG64 wo = InterlockedCompareExchange64(&ctx->stat_write_ops, 0, 0);
                    printf("{\"event\":\"stats\",\"read_bytes\":%lld,\"write_bytes\":%lld,\"read_ops\":%lld,\"write_ops\":%lld}\n",
                           (long long)rb, (long long)wb, (long long)ro, (long long)wo);
                    fflush(stdout);
                    last_stats_tick = now;
                }
            }
        }
    }

    if (json_events) {
        printf("{\"event\":\"unmounting\",\"mountpoint\":"); print_json_str(mp); printf("}\n");
        fflush(stdout);
    } else {
        printf("Unmounting %s...\n", mp);
    }
    winfsp_driver_stop(mh);
    ext4_vfs_unmount(ctx);
    if (stop_event) CloseHandle(stop_event);

    {
        ztrace_summary_t tsum;
        int trace_flush_rc = 0;
        memset(&tsum, 0, sizeof(tsum));
        if (tracing) {
            trace_flush_rc = ztrace_shutdown(&tsum);
        }
        if (json_events) {
            printf("{\"event\":\"unmounted\",\"mountpoint\":"); print_json_str(mp);
            if (tracing && trace_flush_rc == 0) {
                printf(",\"trace_path\":"); print_json_str(trace_path);
                printf(",\"trace_events\":%llu,\"trace_dropped\":%llu",
                       (unsigned long long)tsum.events_written,
                       (unsigned long long)tsum.dropped_events);
            }
            printf("}\n");
            fflush(stdout);
        } else {
            printf("Unmounted cleanly.\n");
            if (tracing) {
                if (trace_flush_rc == 0) {
                    printf("Trace written to '%s' (events=%llu, dropped=%llu, threads=%u). Open at https://ui.perfetto.dev/\n",
                           trace_path,
                           (unsigned long long)tsum.events_written,
                           (unsigned long long)tsum.dropped_events,
                           tsum.threads_seen);
                } else {
                    fprintf(stderr, "Failed to write trace '%s' (err=%d).\n", trace_path, trace_flush_rc);
                }
            }
        }
    }
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

static int build_utf8_argv(int *out_argc, char ***out_argv) {
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv || wargc <= 0) {
        if (wargv) LocalFree(wargv);
        return -1;
    }
    char **uargv = (char **)calloc((size_t)wargc + 1, sizeof(char *));
    if (!uargv) {
        LocalFree(wargv);
        return -1;
    }
    for (int i = 0; i < wargc; i++) {
        int needed = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        if (needed <= 0) {
            for (int j = 0; j < i; j++) free(uargv[j]);
            free(uargv);
            LocalFree(wargv);
            return -1;
        }
        uargv[i] = (char *)malloc((size_t)needed);
        if (!uargv[i]) {
            for (int j = 0; j < i; j++) free(uargv[j]);
            free(uargv);
            LocalFree(wargv);
            return -1;
        }
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, uargv[i], needed, NULL, NULL);
    }
    LocalFree(wargv);
    *out_argc = wargc;
    *out_argv = uargv;
    return 0;
}

static void free_utf8_argv(int argc, char **argv) {
    if (!argv) return;
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
}

int main(int argc, char **argv) {
    int uargc = 0;
    char **uargv = NULL;
    int using_uargv = 0;
    int rc = 1;

    install_crash_handlers();
    if (build_utf8_argv(&uargc, &uargv) == 0) {
        argc = uargc;
        argv = uargv;
        using_uargv = 1;
    }

    if (argc < 2) {
        print_usage();
        if (using_uargv) free_utf8_argv(uargc, uargv);
        return 1;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "unmount") == 0 || strcmp(cmd, "umount") == 0) rc = cmd_unmount(argc, argv);
    else if (strcmp(cmd, "list") == 0) rc = cmd_list(argc, argv);
    else if (strcmp(cmd, "inspect") == 0) rc = cmd_inspect(argc, argv);
    else if (strcmp(cmd, "mkfs") == 0) rc = cmd_mkfs(argc, argv);
    else if (strcmp(cmd, "ls") == 0) rc = cmd_ls(argc, argv);
    else if (strcmp(cmd, "stat") == 0) rc = cmd_stat(argc, argv);
    else if (strcmp(cmd, "cat") == 0) rc = cmd_cat(argc, argv);
    else if (strcmp(cmd, "write") == 0) rc = cmd_write(argc, argv);
    else if (strcmp(cmd, "mkdir") == 0) rc = cmd_mkdir(argc, argv);
    else if (strcmp(cmd, "rm") == 0) rc = cmd_rm(argc, argv);
    else if (strcmp(cmd, "mv") == 0) rc = cmd_mv(argc, argv);
    else if (strcmp(cmd, "extents") == 0) rc = cmd_extents(argc, argv);
    else if (strcmp(cmd, "mount") == 0) rc = cmd_mount(argc, argv);
    else {
        print_usage();
        rc = 1;
    }

    if (using_uargv) free_utf8_argv(uargc, uargv);
    return rc;
}
