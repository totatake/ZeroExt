#include "disk_scanner.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>

int disk_scanner_scan_physical_drives(scanned_disk_t *out_disks, uint32_t max_disks, uint32_t *out_count) {
    uint32_t found = 0;
    uint32_t idx;

    if (!out_disks || !out_count) return -1;
    *out_count = 0;

    for (idx = 0; idx < SCAN_MAX_DRIVES && found < max_disks; idx++) {
        char path[64];
        wchar_t wpath[64];
        HANDLE h;

        snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%u", idx);
        swprintf(wpath, 64, L"\\\\.\\PhysicalDrive%u", idx);

        h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            if (err == ERROR_ACCESS_DENIED) {
                scanned_disk_t *d = &out_disks[found++];
                memset(d, 0, sizeof(*d));
                d->drive_index = idx;
                strcpy_s(d->device_path, sizeof(d->device_path), path);
                d->accessible = 0;
            }
            continue;
        }
        CloseHandle(h);

        {
            scanned_disk_t *d = &out_disks[found++];
            memset(d, 0, sizeof(*d));
            d->drive_index = idx;
            strcpy_s(d->device_path, sizeof(d->device_path), path);
            d->accessible = 1;
            pt_read_partitions(path, 0, &d->table);
        }
    }

    *out_count = found;
    return 0;
}
