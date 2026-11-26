#include "perfetto_trace.h"
#include "../common/ext4_errno.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZTRACE_DEFAULT_EVENTS_PER_THREAD 65536U
#define ZTRACE_MIN_EVENTS_PER_THREAD     1024U
#define ZTRACE_MAX_EVENTS_PER_THREAD     1048576U

typedef struct ztrace_event {
    const char *cat;
    const char *name;
    uint64_t    ts_qpc;
    uint64_t    dur_qpc;
    uint64_t    arg0;
    uint64_t    arg1;
    int64_t     arg2;
    uint16_t    type;
    uint16_t    arg_schema;
    uint32_t    reserved;
} ztrace_event_t;

typedef char ztrace_event_size_assert_t[(sizeof(ztrace_event_t) == 64) ? 1 : -1];

typedef struct ztrace_thread_buf {
    DWORD                     tid;
    char                      thread_name[64];
    uint32_t                  capacity;
    uint32_t                  mask;
    uint64_t                  write_seq;
    uint64_t                  dropped_events;
    volatile LONG             writing;
    ztrace_event_t           *events;
    struct ztrace_thread_buf *next;
} ztrace_thread_buf_t;

typedef struct ztrace_global_state {
    SRWLOCK              registry_lock;
    ztrace_thread_buf_t *head;
    uint32_t             threads_seen;
    uint32_t             events_per_thread;
    uint64_t             qpc_freq;
    uint64_t             start_qpc;
    LONG                 epoch;
    int                  initialized;
    wchar_t              output_path_w[1024];
} ztrace_global_state_t;

volatile LONG g_zeroext4_trace_enabled = 0;

static ztrace_global_state_t g_state = {
    SRWLOCK_INIT,
    NULL,
    0,
    ZTRACE_DEFAULT_EVENTS_PER_THREAD,
    1,
    0,
    0,
    0,
    {0}
};

static __declspec(thread) ztrace_thread_buf_t *tls_buf = NULL;
static __declspec(thread) LONG tls_epoch = 0;

static uint32_t normalize_ring_capacity(uint32_t requested) {
    uint32_t v = requested ? requested : ZTRACE_DEFAULT_EVENTS_PER_THREAD;
    uint32_t cap = ZTRACE_MIN_EVENTS_PER_THREAD;
    if (v <= ZTRACE_MIN_EVENTS_PER_THREAD) return ZTRACE_MIN_EVENTS_PER_THREAD;
    if (v >= ZTRACE_MAX_EVENTS_PER_THREAD) return ZTRACE_MAX_EVENTS_PER_THREAD;
    while (cap < v && cap < ZTRACE_MAX_EVENTS_PER_THREAD) {
        cap <<= 1;
    }
    return cap;
}

static ztrace_thread_buf_t *get_or_alloc_thread_buf(void) {
    LONG cur_epoch = g_state.epoch;
    ztrace_thread_buf_t *b;
    uint32_t cap;

    if (!ztrace_is_enabled()) return NULL;

    if (tls_buf && tls_epoch == cur_epoch) {
        return tls_buf;
    }

    cap = g_state.events_per_thread;
    b = (ztrace_thread_buf_t *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->events = (ztrace_event_t *)calloc(cap, sizeof(ztrace_event_t));
    if (!b->events) {
        free(b);
        return NULL;
    }
    b->tid = GetCurrentThreadId();
    b->capacity = cap;
    b->mask = cap - 1U;

    AcquireSRWLockExclusive(&g_state.registry_lock);
    if (!ztrace_is_enabled() || g_state.epoch != cur_epoch) {
        ReleaseSRWLockExclusive(&g_state.registry_lock);
        free(b->events);
        free(b);
        return NULL;
    }
    b->next = g_state.head;
    g_state.head = b;
    g_state.threads_seen++;
    ReleaseSRWLockExclusive(&g_state.registry_lock);

    tls_buf = b;
    tls_epoch = cur_epoch;
    return b;
}

static void record_event_raw(uint16_t type, const char *cat, const char *name,
                             uint64_t ts_qpc, uint64_t dur_qpc,
                             uint64_t arg0, uint64_t arg1, int64_t arg2,
                             uint16_t arg_schema) {
    ztrace_thread_buf_t *b;
    ztrace_event_t *ev;
    uint64_t seq;

    if (!ztrace_is_enabled()) return;
    b = get_or_alloc_thread_buf();
    if (!b) return;

    InterlockedExchange(&b->writing, 1);
    if (!ztrace_is_enabled()) {
        InterlockedExchange(&b->writing, 0);
        return;
    }

    seq = b->write_seq;
    ev = &b->events[seq & b->mask];
    ev->cat = cat ? cat : "";
    ev->name = name ? name : "";
    ev->ts_qpc = ts_qpc;
    ev->dur_qpc = dur_qpc;
    ev->arg0 = arg0;
    ev->arg1 = arg1;
    ev->arg2 = arg2;
    ev->type = type;
    ev->arg_schema = arg_schema;
    ev->reserved = 0;

    if (seq >= b->capacity) {
        b->dropped_events++;
    }
    b->write_seq = seq + 1;
    InterlockedExchange(&b->writing, 0);
}

int ztrace_init(const char *output_path_utf8, uint32_t events_per_thread) {
    wchar_t wpath[1024];
    FILE *probe_fp;
    LARGE_INTEGER freq, start;

    if (!output_path_utf8 || output_path_utf8[0] == '\0') return -EINVAL;
    if (ext4_utf8_to_wide(output_path_utf8, wpath, _countof(wpath)) != 0) {
        return -EINVAL;
    }

    AcquireSRWLockExclusive(&g_state.registry_lock);
    if (g_state.initialized) {
        ReleaseSRWLockExclusive(&g_state.registry_lock);
        return -EBUSY;
    }

    /* Validate output path is writable right away so CLI/GUI fails fast on bad paths */
    probe_fp = _wfopen(wpath, L"wb");
    if (!probe_fp) {
        ReleaseSRWLockExclusive(&g_state.registry_lock);
        return -EIO;
    }
    fclose(probe_fp);

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    g_state.head = NULL;
    g_state.threads_seen = 0;
    g_state.events_per_thread = normalize_ring_capacity(events_per_thread);
    g_state.qpc_freq = (freq.QuadPart > 0) ? (uint64_t)freq.QuadPart : 10000000ULL;
    g_state.start_qpc = (uint64_t)start.QuadPart;
    g_state.epoch++;
    g_state.initialized = 1;
    wcscpy_s(g_state.output_path_w, _countof(g_state.output_path_w), wpath);

    InterlockedExchange(&g_zeroext4_trace_enabled, 1);
    ReleaseSRWLockExclusive(&g_state.registry_lock);
    return 0;
}

void ztrace_set_thread_name(const char *name) {
    ztrace_thread_buf_t *b;
    if (!ztrace_is_enabled() || !name || name[0] == '\0') return;
    b = get_or_alloc_thread_buf();
    if (!b) return;
    strncpy_s(b->thread_name, sizeof(b->thread_name), name, _TRUNCATE);
}

void ztrace_record_complete(const char *cat, const char *name,
                            uint64_t ts_qpc, uint64_t dur_qpc,
                            uint64_t arg0, uint64_t arg1, int64_t arg2,
                            uint16_t arg_schema) {
    record_event_raw(ZTRACE_EV_COMPLETE, cat, name, ts_qpc, dur_qpc, arg0, arg1, arg2, arg_schema);
}

void ztrace_record_counter(const char *cat, const char *name,
                           uint64_t arg0, uint64_t arg1, int64_t arg2,
                           uint16_t arg_schema) {
    record_event_raw(ZTRACE_EV_COUNTER, cat, name, ztrace_now_qpc(), 0, arg0, arg1, arg2, arg_schema);
}

void ztrace_record_instant(const char *cat, const char *name,
                           uint64_t arg0, uint64_t arg1, int64_t arg2,
                           uint16_t arg_schema) {
    record_event_raw(ZTRACE_EV_INSTANT, cat, name, ztrace_now_qpc(), 0, arg0, arg1, arg2, arg_schema);
}

static void write_json_escaped(FILE *fp, const char *s) {
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    fputc('"', fp);
    while (*p) {
        unsigned char c = *p++;
        switch (c) {
        case '"':  fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\b': fputs("\\b", fp); break;
        case '\f': fputs("\\f", fp); break;
        case '\n': fputs("\\n", fp); break;
        case '\r': fputs("\\r", fp); break;
        case '\t': fputs("\\t", fp); break;
        default:
            if (c < 0x20) {
                fprintf(fp, "\\u%04x", (unsigned)c);
            } else {
                fputc((int)c, fp);
            }
            break;
        }
    }
    fputc('"', fp);
}

static double qpc_delta_to_us(uint64_t delta_qpc, uint64_t freq) {
    return ((double)delta_qpc * 1000000.0) / (double)(freq ? freq : 1ULL);
}

static void write_event_args_json(FILE *fp, const ztrace_event_t *ev, uint64_t freq) {
    switch ((ztrace_arg_schema_t)ev->arg_schema) {
    case ZTRACE_ARGS_IO:
        fprintf(fp, ",\"args\":{\"offset\":%llu,\"bytes\":%llu,\"status\":%lld}",
                (unsigned long long)ev->arg0,
                (unsigned long long)ev->arg1,
                (long long)ev->arg2);
        break;
    case ZTRACE_ARGS_RW_OP:
        fprintf(fp, ",\"args\":{\"ino\":%llu,\"bytes\":%llu,\"rc\":%lld}",
                (unsigned long long)ev->arg0,
                (unsigned long long)ev->arg1,
                (long long)ev->arg2);
        break;
    case ZTRACE_ARGS_LOCK:
        fprintf(fp, ",\"args\":{\"wait_us\":%.3f,\"exclusive\":%s}",
                qpc_delta_to_us(ev->arg0, freq),
                ev->arg1 ? "true" : "false");
        break;
    case ZTRACE_ARGS_EXTENT:
        fprintf(fp, ",\"args\":{\"ino\":%llu,\"runs\":%llu,\"rc\":%lld}",
                (unsigned long long)ev->arg0,
                (unsigned long long)ev->arg1,
                (long long)ev->arg2);
        break;
    case ZTRACE_ARGS_WIN32_IO:
        fprintf(fp, ",\"args\":{\"offset\":%llu,\"bytes\":%llu,\"err\":%lld}",
                (unsigned long long)ev->arg0,
                (unsigned long long)ev->arg1,
                (long long)ev->arg2);
        break;
    case ZTRACE_ARGS_OVERLAY:
        fprintf(fp, ",\"args\":{\"bytes\":%llu,\"in_upper\":%llu,\"rc\":%lld}",
                (unsigned long long)ev->arg0,
                (unsigned long long)ev->arg1,
                (long long)ev->arg2);
        break;
    case ZTRACE_ARGS_STATUS:
        fprintf(fp, ",\"args\":{\"ino\":%llu,\"status\":%lld}",
                (unsigned long long)ev->arg0,
                (long long)ev->arg2);
        break;
    case ZTRACE_ARGS_COUNTER:
        fprintf(fp, ",\"args\":{\"value\":%llu}",
                (unsigned long long)ev->arg0);
        break;
    case ZTRACE_ARGS_CACHE_COUNTER:
        fprintf(fp, ",\"args\":{\"hits\":%llu,\"misses\":%llu}",
                (unsigned long long)ev->arg0,
                (unsigned long long)ev->arg1);
        break;
    case ZTRACE_ARGS_LRU_COUNTER:
        fprintf(fp, ",\"args\":{\"hits\":%llu,\"misses\":%llu,\"rmw_ops\":%llu}",
                (unsigned long long)ev->arg0,
                (unsigned long long)ev->arg1,
                (unsigned long long)ev->arg2);
        break;
    case ZTRACE_ARGS_NONE:
    default:
        break;
    }
}

int ztrace_shutdown(ztrace_summary_t *out_summary) {
    ztrace_thread_buf_t *head = NULL;
    wchar_t out_path[1024];
    uint64_t freq;
    uint64_t start_qpc;
    uint32_t threads_seen = 0;
    uint64_t total_written = 0;
    uint64_t total_dropped = 0;
    FILE *fp = NULL;
    char *io_buf = NULL;
    DWORD pid = GetCurrentProcessId();
    int first_item = 1;
    int rc = 0;

    if (out_summary) {
        memset(out_summary, 0, sizeof(*out_summary));
    }

    InterlockedExchange(&g_zeroext4_trace_enabled, 0);

    AcquireSRWLockExclusive(&g_state.registry_lock);
    if (!g_state.initialized) {
        ReleaseSRWLockExclusive(&g_state.registry_lock);
        return 0;
    }
    head = g_state.head;
    threads_seen = g_state.threads_seen;
    freq = g_state.qpc_freq;
    start_qpc = g_state.start_qpc;
    wcscpy_s(out_path, _countof(out_path), g_state.output_path_w);

    g_state.head = NULL;
    g_state.threads_seen = 0;
    g_state.initialized = 0;
    g_state.epoch++;
    ReleaseSRWLockExclusive(&g_state.registry_lock);

    /* Ensure any concurrent writer finishing its ring slot completes before freeing buffers */
    for (ztrace_thread_buf_t *b = head; b != NULL; b = b->next) {
        while (InterlockedCompareExchange(&b->writing, 0, 0) != 0) {
            YieldProcessor();
        }
    }

    fp = _wfopen(out_path, L"wb");
    if (!fp) {
        rc = -EIO;
    } else {
        io_buf = (char *)malloc(65536);
        if (io_buf) {
            setvbuf(fp, io_buf, _IOFBF, 65536);
        }

        fputs("{\"displayTimeUnit\":\"ns\",\"traceEvents\":[\n", fp);

        /* Process metadata */
        fprintf(fp, "{\"ph\":\"M\",\"pid\":%lu,\"tid\":0,\"name\":\"process_name\",\"args\":{\"name\":\"ZeroExt4\"}}",
                (unsigned long)pid);
        first_item = 0;

        /* Thread metadata + ring events */
        for (ztrace_thread_buf_t *b = head; b != NULL; b = b->next) {
            const char *tname = b->thread_name[0] ? b->thread_name : "Thread";
            uint64_t count = (b->write_seq <= b->capacity) ? b->write_seq : (uint64_t)b->capacity;
            uint64_t start_idx = (b->write_seq <= b->capacity) ? 0ULL : (b->write_seq & b->mask);

            total_dropped += b->dropped_events;

            if (!first_item) fputs(",\n", fp);
            first_item = 0;
            fprintf(fp, "{\"ph\":\"M\",\"pid\":%lu,\"tid\":%lu,\"name\":\"thread_name\",\"args\":{\"name\":",
                    (unsigned long)pid, (unsigned long)b->tid);
            write_json_escaped(fp, tname);
            fputs("}}", fp);

            for (uint64_t k = 0; k < count; k++) {
                const ztrace_event_t *ev = &b->events[(start_idx + k) & b->mask];
                uint64_t rel_qpc = (ev->ts_qpc >= start_qpc) ? (ev->ts_qpc - start_qpc) : 0ULL;
                double ts_us = qpc_delta_to_us(rel_qpc, freq);

                if (!first_item) fputs(",\n", fp);
                first_item = 0;

                if (ev->type == ZTRACE_EV_COMPLETE) {
                    double dur_us = qpc_delta_to_us(ev->dur_qpc, freq);
                    fprintf(fp, "{\"ph\":\"X\",\"cat\":");
                    write_json_escaped(fp, ev->cat);
                    fputs(",\"name\":", fp);
                    write_json_escaped(fp, ev->name);
                    fprintf(fp, ",\"pid\":%lu,\"tid\":%lu,\"ts\":%.3f,\"dur\":%.3f",
                            (unsigned long)pid, (unsigned long)b->tid, ts_us, dur_us);
                    write_event_args_json(fp, ev, freq);
                    fputc('}', fp);
                } else if (ev->type == ZTRACE_EV_COUNTER) {
                    fprintf(fp, "{\"ph\":\"C\",\"cat\":");
                    write_json_escaped(fp, ev->cat);
                    fputs(",\"name\":", fp);
                    write_json_escaped(fp, ev->name);
                    fprintf(fp, ",\"pid\":%lu,\"tid\":0,\"ts\":%.3f",
                            (unsigned long)pid, ts_us);
                    write_event_args_json(fp, ev, freq);
                    fputc('}', fp);
                } else {
                    fprintf(fp, "{\"ph\":\"i\",\"s\":\"t\",\"cat\":");
                    write_json_escaped(fp, ev->cat);
                    fputs(",\"name\":", fp);
                    write_json_escaped(fp, ev->name);
                    fprintf(fp, ",\"pid\":%lu,\"tid\":%lu,\"ts\":%.3f",
                            (unsigned long)pid, (unsigned long)b->tid, ts_us);
                    write_event_args_json(fp, ev, freq);
                    fputc('}', fp);
                }
                total_written++;
            }
        }

        fputs("\n]}\n", fp);
        if (ferror(fp)) {
            rc = -EIO;
        }
        fclose(fp);
        free(io_buf);
    }

    while (head) {
        ztrace_thread_buf_t *next = head->next;
        free(head->events);
        free(head);
        head = next;
    }

    if (out_summary) {
        out_summary->events_written = total_written;
        out_summary->dropped_events = total_dropped;
        out_summary->threads_seen = threads_seen;
        out_summary->qpc_freq = freq;
    }
    return rc;
}
