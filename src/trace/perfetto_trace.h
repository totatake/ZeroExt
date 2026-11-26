#ifndef ZEROEXT4_PERFETTO_TRACE_H
#define ZEROEXT4_PERFETTO_TRACE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ztrace_ev_type {
    ZTRACE_EV_COMPLETE = 0, /* 'X' duration span */
    ZTRACE_EV_COUNTER  = 1, /* 'C' process-level counter */
    ZTRACE_EV_INSTANT  = 2, /* 'i' instant marker */
} ztrace_ev_type_t;

typedef enum ztrace_arg_schema {
    ZTRACE_ARGS_NONE = 0,
    ZTRACE_ARGS_IO,            /* offset, bytes, status */
    ZTRACE_ARGS_RW_OP,         /* ino, bytes, rc */
    ZTRACE_ARGS_LOCK,          /* wait_us (from qpc), exclusive */
    ZTRACE_ARGS_EXTENT,        /* ino, runs, hit */
    ZTRACE_ARGS_WIN32_IO,      /* offset, bytes, err */
    ZTRACE_ARGS_OVERLAY,       /* bytes, in_upper, rc */
    ZTRACE_ARGS_STATUS,        /* ino, status */
    ZTRACE_ARGS_COUNTER,       /* value */
    ZTRACE_ARGS_CACHE_COUNTER, /* hits, misses */
    ZTRACE_ARGS_LRU_COUNTER,   /* hits, misses, rmw_ops */
} ztrace_arg_schema_t;

typedef struct ztrace_summary {
    uint64_t events_written;
    uint64_t dropped_events;
    uint32_t threads_seen;
    uint64_t qpc_freq;
} ztrace_summary_t;

extern volatile LONG g_zeroext4_trace_enabled;

static __inline int ztrace_is_enabled(void) {
    return g_zeroext4_trace_enabled != 0;
}

static __inline uint64_t ztrace_now_qpc(void) {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (uint64_t)li.QuadPart;
}

/* Initialize tracing to output_path_utf8 (Chrome Trace Event JSON for ui.perfetto.dev).
 * events_per_thread = 0 uses default 65536 (rounded to power of 2 in [1024, 1048576]). */
int ztrace_init(const char *output_path_utf8, uint32_t events_per_thread);

/* Disable tracing, flush all thread-local ring buffers to disk, and free buffers. */
int ztrace_shutdown(ztrace_summary_t *out_summary);

/* Assign a human-readable Perfetto thread track name for the calling thread. */
void ztrace_set_thread_name(const char *name);

/* cat and name must point to static / string-literal storage that outlives tracing. */
void ztrace_record_complete(const char *cat, const char *name,
                            uint64_t ts_qpc, uint64_t dur_qpc,
                            uint64_t arg0, uint64_t arg1, int64_t arg2,
                            uint16_t arg_schema);

void ztrace_record_counter(const char *cat, const char *name,
                           uint64_t arg0, uint64_t arg1, int64_t arg2,
                           uint16_t arg_schema);

void ztrace_record_instant(const char *cat, const char *name,
                           uint64_t arg0, uint64_t arg1, int64_t arg2,
                           uint16_t arg_schema);

typedef struct ztrace_lock_guard {
    const char *wait_name;
    const char *hold_name;
    uint64_t    acq_start_qpc;
    uint64_t    hold_start_qpc;
    uint64_t    wait_qpc;
    int         active;
    int         exclusive;
} ztrace_lock_guard_t;

static __inline ztrace_lock_guard_t ztrace_srw_acquire_shared(
    SRWLOCK *lock, const char *wait_name, const char *hold_name) {
    ztrace_lock_guard_t lg;
    if (!ztrace_is_enabled()) {
        AcquireSRWLockShared(lock);
        lg.wait_name = NULL;
        lg.hold_name = NULL;
        lg.acq_start_qpc = 0;
        lg.hold_start_qpc = 0;
        lg.wait_qpc = 0;
        lg.active = 0;
        lg.exclusive = 0;
        return lg;
    }
    lg.wait_name = wait_name;
    lg.hold_name = hold_name;
    lg.exclusive = 0;
    lg.active = 1;
    lg.acq_start_qpc = ztrace_now_qpc();
    AcquireSRWLockShared(lock);
    lg.hold_start_qpc = ztrace_now_qpc();
    lg.wait_qpc = (lg.hold_start_qpc >= lg.acq_start_qpc) ? (lg.hold_start_qpc - lg.acq_start_qpc) : 0;
    return lg;
}

static __inline void ztrace_srw_release_shared(SRWLOCK *lock, const ztrace_lock_guard_t *lg) {
    uint64_t end_qpc = 0;
    int active = (lg && lg->active && ztrace_is_enabled());
    if (active) {
        end_qpc = ztrace_now_qpc();
    }
    ReleaseSRWLockShared(lock);
    if (active) {
        uint64_t hold_qpc = (end_qpc >= lg->hold_start_qpc) ? (end_qpc - lg->hold_start_qpc) : 0;
        if (lg->wait_name) {
            ztrace_record_complete("lock", lg->wait_name, lg->acq_start_qpc, lg->wait_qpc,
                                   lg->wait_qpc, (uint64_t)lg->exclusive, 0, ZTRACE_ARGS_LOCK);
        }
        if (lg->hold_name) {
            ztrace_record_complete("lock", lg->hold_name, lg->hold_start_qpc, hold_qpc,
                                   lg->wait_qpc, (uint64_t)lg->exclusive, 0, ZTRACE_ARGS_LOCK);
        }
    }
}

static __inline ztrace_lock_guard_t ztrace_srw_acquire_exclusive(
    SRWLOCK *lock, const char *wait_name, const char *hold_name) {
    ztrace_lock_guard_t lg;
    if (!ztrace_is_enabled()) {
        AcquireSRWLockExclusive(lock);
        lg.wait_name = NULL;
        lg.hold_name = NULL;
        lg.acq_start_qpc = 0;
        lg.hold_start_qpc = 0;
        lg.wait_qpc = 0;
        lg.active = 0;
        lg.exclusive = 1;
        return lg;
    }
    lg.wait_name = wait_name;
    lg.hold_name = hold_name;
    lg.exclusive = 1;
    lg.active = 1;
    lg.acq_start_qpc = ztrace_now_qpc();
    AcquireSRWLockExclusive(lock);
    lg.hold_start_qpc = ztrace_now_qpc();
    lg.wait_qpc = (lg.hold_start_qpc >= lg.acq_start_qpc) ? (lg.hold_start_qpc - lg.acq_start_qpc) : 0;
    return lg;
}

static __inline void ztrace_srw_release_exclusive(SRWLOCK *lock, const ztrace_lock_guard_t *lg) {
    uint64_t end_qpc = 0;
    int active = (lg && lg->active && ztrace_is_enabled());
    if (active) {
        end_qpc = ztrace_now_qpc();
    }
    ReleaseSRWLockExclusive(lock);
    if (active) {
        uint64_t hold_qpc = (end_qpc >= lg->hold_start_qpc) ? (end_qpc - lg->hold_start_qpc) : 0;
        if (lg->wait_name) {
            ztrace_record_complete("lock", lg->wait_name, lg->acq_start_qpc, lg->wait_qpc,
                                   lg->wait_qpc, (uint64_t)lg->exclusive, 0, ZTRACE_ARGS_LOCK);
        }
        if (lg->hold_name) {
            ztrace_record_complete("lock", lg->hold_name, lg->hold_start_qpc, hold_qpc,
                                   lg->wait_qpc, (uint64_t)lg->exclusive, 0, ZTRACE_ARGS_LOCK);
        }
    }
}

#ifdef __cplusplus
}
#endif

#endif /* ZEROEXT4_PERFETTO_TRACE_H */
