#ifndef ZEROEXT4_OVERLAY_TOOLS_H
#define ZEROEXT4_OVERLAY_TOOLS_H

#include "overlay_vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum overlay_diff_kind {
    OV_DIFF_ADDED = 0,
    OV_DIFF_MODIFIED = 1,
    OV_DIFF_DELETED = 2,
    OV_DIFF_OPAQUE_DIR = 3,
    OV_DIFF_UPPER = 4, /* Upperdir inspected standalone without --lower */
} overlay_diff_kind_t;

typedef struct overlay_diff_entry {
    overlay_diff_kind_t kind;
    char                path[1024];
    int                 is_dir;
    uint64_t            size;
    uint32_t            mode;
    uint64_t            mtime_unix;
} overlay_diff_entry_t;

typedef struct overlay_diff_summary {
    uint64_t added_count;
    uint64_t modified_count;
    uint64_t deleted_count;
    uint64_t opaque_dir_count;
    uint64_t upper_count;
    uint64_t file_count;
    uint64_t dir_count;
    uint64_t total_upper_bytes;
} overlay_diff_summary_t;

typedef struct overlay_diff_report {
    overlay_upper_check_t   upper_check;
    overlay_diff_summary_t  summary;
    overlay_diff_entry_t   *entries;
    size_t                  entry_count;
    size_t                  entry_cap;
} overlay_diff_report_t;

typedef struct overlay_validate_issue {
    char severity[16]; /* "error" | "warn" */
    char code[48];
    char path[1024];
    char message[256];
} overlay_validate_issue_t;

typedef struct overlay_validate_report {
    overlay_upper_check_t     upper_check;
    int                       valid;
    uint64_t                  error_count;
    uint64_t                  warning_count;
    overlay_validate_issue_t *issues;
    size_t                    issue_count;
    size_t                    issue_cap;
} overlay_validate_report_t;

typedef struct overlay_export_summary {
    overlay_diff_summary_t diff_summary;
    uint64_t               exported_entries;
    uint64_t               archive_bytes;
} overlay_export_summary_t;

const char *overlay_diff_kind_name(overlay_diff_kind_t kind);

int overlay_tools_scan(const char *upper_dir_utf8,
                       ext4_vfs_ctx_t *lower_ctx_opt,
                       const overlay_lower_identity_t *expected_opt,
                       int collect_entries,
                       overlay_diff_report_t *out_report);

void overlay_tools_free_diff_report(overlay_diff_report_t *report);

int overlay_tools_validate(const char *upper_dir_utf8,
                           ext4_vfs_ctx_t *lower_ctx_opt,
                           const overlay_lower_identity_t *expected_opt,
                           overlay_validate_report_t *out_report);

void overlay_tools_free_validate_report(overlay_validate_report_t *report);

int overlay_tools_export_tar(const char *upper_dir_utf8,
                             ext4_vfs_ctx_t *lower_ctx_opt,
                             const overlay_lower_identity_t *expected_opt,
                             const char *output_tar_utf8,
                             overlay_export_summary_t *out_summary);

int overlay_tools_reset_upper(const char *upper_dir_utf8,
                              int keep_marker,
                              uint64_t *out_removed_entries);

#ifdef __cplusplus
}
#endif

#endif /* ZEROEXT4_OVERLAY_TOOLS_H */
