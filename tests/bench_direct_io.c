/*
 * Performance benchmark comparing standard ext2fs_file_read vs --direct-io
 * (lockless extent-based pread) across VFS and live WinFsp mount layers.
 */

#include "vfs/ext4_vfs.h"
#include "extent/extent_map.h"

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BENCH_MAX_THREADS 16

typedef struct worker_arg {
    ext4_vfs_ctx_t *ctx;
    ext2_ino_t      ino;
    uint64_t        file_size;
    uint32_t        chunk_sz;
    uint64_t        ops;
    int             random_mode;
    uint32_t        seed;
    uint64_t        bytes_done;
    int             err;
} worker_arg_t;

static double qpc_sec(LARGE_INTEGER t0, LARGE_INTEGER t1, LARGE_INTEGER freq) {
    double secs = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    return secs > 0.0 ? secs : 1e-9;
}

static uint32_t lcg_next(uint32_t *state) {
    *state = (*state) * 1664525U + 1013904223U;
    return *state;
}

static DWORD WINAPI bench_worker(LPVOID param) {
    worker_arg_t *w = (worker_arg_t *)param;
    uint64_t total_pages = w->chunk_sz ? (w->file_size / w->chunk_sz) : 0;
    uint8_t *buf;
    uint64_t done = 0;

    if (total_pages == 0) {
        w->err = -1;
        return 1;
    }

    buf = (uint8_t *)_aligned_malloc(w->chunk_sz, 4096);
    if (!buf) {
        w->err = -1;
        return 1;
    }

    for (uint64_t i = 0; i < w->ops; i++) {
        uint64_t off;
        uint32_t got = 0;
        int rc;

        if (w->random_mode) {
            uint64_t r = ((uint64_t)lcg_next(&w->seed) << 16) ^ lcg_next(&w->seed);
            off = (r % total_pages) * (uint64_t)w->chunk_sz;
        } else {
            off = (i % total_pages) * (uint64_t)w->chunk_sz;
        }

        rc = ext4_vfs_read(w->ctx, w->ino, buf, off, w->chunk_sz, &got);
        if (rc != 0 || got == 0) {
            w->err = rc ? rc : -5;
            break;
        }
        done += got;
    }

    w->bytes_done = done;
    _aligned_free(buf);
    return 0;
}

static int run_vfs_bench(const char *label, ext4_vfs_ctx_t *ctx, ext2_ino_t ino,
                         uint64_t file_size, uint32_t chunk_sz, uint64_t ops_per_thread,
                         int threads, int random_mode) {
    LARGE_INTEGER freq, t0, t1;
    worker_arg_t args[BENCH_MAX_THREADS];
    HANDLE th[BENCH_MAX_THREADS];
    int created = 0;
    int any_err = 0;
    uint64_t total_bytes = 0;
    uint64_t total_ops;
    double secs, mbps, iops, us_per_op;

    if (threads < 1 || threads > BENCH_MAX_THREADS) {
        fprintf(stderr, "Invalid thread count %d in %s\n", threads, label);
        return 1;
    }

    memset(args, 0, sizeof(args));
    memset(th, 0, sizeof(th));
    QueryPerformanceFrequency(&freq);

    /* Brief warmup so initial extent map lookup or OS page state is comparable */
    {
        uint8_t *wb = (uint8_t *)malloc(chunk_sz);
        uint32_t wg = 0;
        if (wb) {
            (void)ext4_vfs_read(ctx, ino, wb, 0, chunk_sz, &wg);
            free(wb);
        }
    }

    QueryPerformanceCounter(&t0);
    for (int t = 0; t < threads; t++) {
        args[t].ctx = ctx;
        args[t].ino = ino;
        args[t].file_size = file_size;
        args[t].chunk_sz = chunk_sz;
        args[t].ops = ops_per_thread;
        args[t].random_mode = random_mode;
        args[t].seed = 0x12345U + (uint32_t)t * 7919U;

        if (threads == 1) {
            (void)bench_worker(&args[t]);
        } else {
            HANDLE h = CreateThread(NULL, 0, bench_worker, &args[t], 0, NULL);
            if (!h) {
                args[t].err = -1;
                any_err = 1;
                break;
            }
            th[created++] = h;
        }
    }
    if (created > 0) {
        WaitForMultipleObjects((DWORD)created, th, TRUE, INFINITE);
        for (int t = 0; t < created; t++) {
            CloseHandle(th[t]);
        }
    }
    QueryPerformanceCounter(&t1);

    for (int t = 0; t < threads; t++) {
        if (args[t].err != 0) {
            fprintf(stderr, "Worker error in %s (thread %d): %d\n", label, t, args[t].err);
            any_err = 1;
        }
        total_bytes += args[t].bytes_done;
    }

    total_ops = ops_per_thread * (uint64_t)threads;
    secs = qpc_sec(t0, t1, freq);
    mbps = ((double)total_bytes / (1024.0 * 1024.0)) / secs;
    iops = (double)total_ops / secs;
    us_per_op = (secs * 1e6) / (double)total_ops;
    printf("%-42s | %8.3f s | %9.1f MB/s | %10.0f IOPS | %8.2f us/op\n",
           label, secs, mbps, iops, us_per_op);
    fflush(stdout);
    return any_err;
}

static int prepare_image(const char *img_path) {
    ext4_mount_config_t cfg;
    ext4_vfs_ctx_t *ctx = NULL;
    ext2_ino_t model_ino = 0, frag_ino = 0, sep_ino = 0;
    uint32_t w = 0;
    const uint32_t chunk = 1024U * 1024U;
    uint8_t *buf = (uint8_t *)malloc(chunk);
    int rc = 1;

    if (!buf) {
        return 1;
    }

    DeleteFileA(img_path);
    if (ext4_vfs_mkfs(img_path, 160ULL * 1024ULL * 1024ULL, "BENCH") != 0) {
        goto cleanup;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.source_path = img_path;
    cfg.read_only = 0;
    if (ext4_vfs_mount(&cfg, &ctx) != 0) {
        goto cleanup;
    }

    /* 1. Contiguous 64 MB /model.gguf */
    if (ext4_vfs_create(ctx, "/model.gguf", 0, 0644, 0, &model_ino) != 0) {
        goto cleanup;
    }
    for (uint32_t i = 0; i < 64; i++) {
        memset(buf, (int)(i & 0xFF), chunk);
        if (ext4_vfs_write(ctx, model_ino, buf, (uint64_t)i * chunk, chunk, 0, &w, NULL) != 0) {
            goto cleanup;
        }
    }

    /* 2. Multi-extent 32 MB /frag.bin (interleaved in 512 KB chunks with /sep.bin) */
    if (ext4_vfs_create(ctx, "/frag.bin", 0, 0644, 0, &frag_ino) != 0) {
        goto cleanup;
    }
    if (ext4_vfs_create(ctx, "/sep.bin", 0, 0644, 0, &sep_ino) != 0) {
        goto cleanup;
    }
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t half = chunk / 2;
        memset(buf, (int)((i + 1) & 0xFF), half);
        if (ext4_vfs_write(ctx, frag_ino, buf, (uint64_t)i * half, half, 0, &w, NULL) != 0) {
            goto cleanup;
        }
        if (ext4_vfs_write(ctx, sep_ino, buf, (uint64_t)i * 4096ULL, 4096U, 0, &w, NULL) != 0) {
            goto cleanup;
        }
    }

    rc = 0;

cleanup:
    if (ctx) {
        ext4_vfs_unmount(ctx);
    }
    free(buf);
    if (rc != 0) {
        DeleteFileA(img_path);
    }
    return rc;
}

static int run_winfsp_live_read(const char *file_path, uint32_t chunk_kb,
                                int unbuffered, int random_mode) {
    wchar_t file_w[MAX_PATH];
    DWORD flags = unbuffered ? FILE_FLAG_NO_BUFFERING : FILE_FLAG_SEQUENTIAL_SCAN;
    HANDLE h;
    LARGE_INTEGER fsz, freq, t0, t1;
    uint32_t chunk_sz;
    uint8_t *buf;
    uint64_t total_done = 0;
    uint64_t ops = 0;
    uint32_t seed = 0xCAFEBABEU;
    int rc = 1;

    if (chunk_kb == 0 || chunk_kb > 65536U) {
        fprintf(stderr, "Invalid chunk_kb: %u\n", chunk_kb);
        return 1;
    }
    chunk_sz = chunk_kb * 1024U;

    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, file_path, -1, file_w, MAX_PATH) == 0 &&
        MultiByteToWideChar(CP_ACP, 0, file_path, -1, file_w, MAX_PATH) == 0) {
        fprintf(stderr, "Path conversion failed: %s\n", file_path);
        return 1;
    }

    h = CreateFileW(file_w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, flags, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "CreateFileW failed (%s): %lu\n", file_path, GetLastError());
        return 1;
    }

    if (!GetFileSizeEx(h, &fsz) || (uint64_t)fsz.QuadPart < chunk_sz) {
        fprintf(stderr, "GetFileSizeEx failed or file smaller than %u KB\n", chunk_kb);
        CloseHandle(h);
        return 1;
    }

    buf = (uint8_t *)_aligned_malloc(chunk_sz, 4096);
    if (!buf) {
        CloseHandle(h);
        return 1;
    }

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    if (!random_mode) {
        DWORD got = 0;
        while (ReadFile(h, buf, chunk_sz, &got, NULL) && got > 0) {
            total_done += got;
            ops++;
        }
    } else {
        uint64_t pages = (uint64_t)fsz.QuadPart / chunk_sz;
        ops = 16384;
        for (uint64_t i = 0; i < ops; i++) {
            uint64_t off = ((uint64_t)lcg_next(&seed) % pages) * (uint64_t)chunk_sz;
            LARGE_INTEGER li;
            DWORD got = 0;
            li.QuadPart = (LONGLONG)off;
            if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) {
                break;
            }
            if (!ReadFile(h, buf, chunk_sz, &got, NULL) || got == 0) {
                break;
            }
            total_done += got;
        }
    }

    QueryPerformanceCounter(&t1);
    CloseHandle(h);
    _aligned_free(buf);

    if (total_done > 0) {
        double secs = qpc_sec(t0, t1, freq);
        double mbps = ((double)total_done / (1024.0 * 1024.0)) / secs;
        double iops = (double)ops / secs;
        printf("WinFsp %s %s (%uKB) | bytes=%llu MB | %8.3f s | %9.1f MB/s | %9.0f IOPS\n",
               unbuffered ? "UNBUF" : "CACHED",
               random_mode ? "RND" : "SEQ",
               chunk_kb,
               (unsigned long long)(total_done / (1024ULL * 1024ULL)),
               secs, mbps, iops);
        rc = 0;
    } else {
        fprintf(stderr, "No bytes read from %s (error=%lu)\n", file_path, GetLastError());
    }
    return rc;
}

static void print_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s\n", prog);
    printf("      Run in-process VFS read benchmark (enable_direct_io=0 vs 1)\n");
    printf("  %s --winfsp-read <file_path> <chunk_kb> [unbuf|cached] [seq|rnd]\n", prog);
    printf("      Benchmark Win32 ReadFile throughput against a mounted WinFsp drive\n");
    printf("  %s --help\n", prog);
}

int main(int argc, char **argv) {
    char tmp_dir[MAX_PATH];
    char img_path[MAX_PATH];
    int failed = 0;

    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_usage(argv[0]);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "--winfsp-read") == 0) {
        if (argc < 4) {
            print_usage(argv[0]);
            return 1;
        }
        {
            uint32_t kb = (uint32_t)strtoul(argv[3], NULL, 10);
            int unbuf = (argc >= 5 && strcmp(argv[4], "unbuf") == 0);
            int rnd = (argc >= 6 && strcmp(argv[5], "rnd") == 0);
            return run_winfsp_live_read(argv[2], kb, unbuf, rnd);
        }
    }

    GetTempPathA((DWORD)sizeof(tmp_dir), tmp_dir);
    snprintf(img_path, sizeof(img_path), "%sext4fsp_bench_%lu.img", tmp_dir, GetCurrentProcessId());

    printf("Preparing 160MB ext4 benchmark image (/model.gguf 64MB, /frag.bin 32MB)...\n");
    if (prepare_image(img_path) != 0) {
        fprintf(stderr, "Failed to prepare benchmark image\n");
        return 1;
    }

    for (int mode = 0; mode < 2; mode++) {
        ext4_vfs_ctx_t *ctx = NULL;
        ext4_mount_config_t cfg;
        ext4_stat_t st_model, st_frag;
        ext4_extent_map_t *m_model = NULL, *m_frag = NULL;

        memset(&cfg, 0, sizeof(cfg));
        cfg.source_path = img_path;
        cfg.read_only = 1;
        cfg.max_cache_blocks = 2048;
        cfg.case_insensitive = 1;
        cfg.enable_direct_io = mode;

        if (ext4_vfs_mount(&cfg, &ctx) != 0) {
            fprintf(stderr, "Mount failed (mode=%d)\n", mode);
            DeleteFileA(img_path);
            return 1;
        }
        if (ext4_vfs_stat(ctx, "/model.gguf", &st_model) != 0 ||
            ext4_vfs_stat(ctx, "/frag.bin", &st_frag) != 0) {
            fprintf(stderr, "Stat failed (mode=%d)\n", mode);
            ext4_vfs_unmount(ctx);
            DeleteFileA(img_path);
            return 1;
        }

        if (mode == 1) {
            if (ext4_vfs_get_extents(ctx, "/model.gguf", &m_model) == 0 &&
                ext4_vfs_get_extents(ctx, "/frag.bin", &m_frag) == 0) {
                printf("\nExtent info: /model.gguf runs=%zu, /frag.bin runs=%zu\n",
                       m_model ? m_model->run_count : 0, m_frag ? m_frag->run_count : 0);
            }
            ext4_extent_map_release(m_model);
            ext4_extent_map_release(m_frag);
        }

        printf("\n=== Mode: enable_direct_io=%d (%s) ===\n",
               mode, mode ? "--direct-io ON (lockless extent pread)" : "Standard ext2fs_file_read");
        failed |= run_vfs_bench("1T Seq  64KB /model.gguf (512MB)", ctx, st_model.ino, st_model.size, 64U * 1024U, 8192, 1, 0);
        failed |= run_vfs_bench("1T Seq   1MB /model.gguf (1024MB)", ctx, st_model.ino, st_model.size, 1024U * 1024U, 1024, 1, 0);
        failed |= run_vfs_bench("1T Seq   4KB /model.gguf (128MB)", ctx, st_model.ino, st_model.size, 4096U, 32768, 1, 0);
        failed |= run_vfs_bench("1T Rnd   4KB /model.gguf (32K ops)", ctx, st_model.ino, st_model.size, 4096U, 32768, 1, 1);
        failed |= run_vfs_bench("8T Seq  64KB /model.gguf (2048MB)", ctx, st_model.ino, st_model.size, 64U * 1024U, 4096, 8, 0);
        failed |= run_vfs_bench("8T Rnd   4KB /model.gguf (131K ops)", ctx, st_model.ino, st_model.size, 4096U, 16384, 8, 1);
        failed |= run_vfs_bench("8T Rnd   4KB /frag.bin   (131K ops)", ctx, st_frag.ino, st_frag.size, 4096U, 16384, 8, 1);

        ext4_vfs_unmount(ctx);
    }

    DeleteFileA(img_path);
    return failed ? 1 : 0;
}
