#ifndef LIBEXT2FS_WIN_DISK_SCANNER_H
#define LIBEXT2FS_WIN_DISK_SCANNER_H

#include "partition_table.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCAN_MAX_DRIVES 16

typedef struct scanned_disk {
    uint32_t          drive_index;
    char              device_path[64];
    int               accessible;
    pt_table_result_t table;
} scanned_disk_t;

int disk_scanner_scan_physical_drives(scanned_disk_t *out_disks, uint32_t max_disks, uint32_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* LIBEXT2FS_WIN_DISK_SCANNER_H */
