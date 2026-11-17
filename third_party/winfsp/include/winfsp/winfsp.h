/*
 * winfsp.h --- WinFsp (Windows File System Proxy) ABI header & dynamic loader
 *
 * Compatible with WinFsp 2.x (winfsp-x64.dll) while allowing compilation
 * without pre-installing WinFsp SDK headers and safe in-process testing.
 */
#ifndef WINFSP_WINFSP_H_INCLUDED
#define WINFSP_WINFSP_H_INCLUDED

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _NTDEF_
typedef LONG NTSTATUS;
#endif

#ifndef _WINTERNL_
typedef struct _FSP_IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
} FSP_IO_STATUS_BLOCK, *PFSP_IO_STATUS_BLOCK;
#define PIO_STATUS_BLOCK PFSP_IO_STATUS_BLOCK
#endif

typedef PVOID PFILE_FULL_EA_INFORMATION;

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS                   ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_UNSUCCESSFUL
#define STATUS_UNSUCCESSFUL              ((NTSTATUS)0xC0000001L)
#endif
#ifndef STATUS_NOT_IMPLEMENTED
#define STATUS_NOT_IMPLEMENTED           ((NTSTATUS)0xC0000002L)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER         ((NTSTATUS)0xC000000DL)
#endif
#ifndef STATUS_END_OF_FILE
#define STATUS_END_OF_FILE               ((NTSTATUS)0xC0000011L)
#endif
#ifndef STATUS_ACCESS_DENIED
#define STATUS_ACCESS_DENIED             ((NTSTATUS)0xC0000022L)
#endif
#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL          ((NTSTATUS)0xC0000023L)
#endif
#ifndef STATUS_OBJECT_NAME_INVALID
#define STATUS_OBJECT_NAME_INVALID       ((NTSTATUS)0xC0000033L)
#endif
#ifndef STATUS_OBJECT_NAME_NOT_FOUND
#define STATUS_OBJECT_NAME_NOT_FOUND     ((NTSTATUS)0xC0000034L)
#endif
#ifndef STATUS_OBJECT_NAME_COLLISION
#define STATUS_OBJECT_NAME_COLLISION     ((NTSTATUS)0xC0000035L)
#endif
#ifndef STATUS_OBJECT_PATH_NOT_FOUND
#define STATUS_OBJECT_PATH_NOT_FOUND     ((NTSTATUS)0xC000003AL)
#endif
#ifndef STATUS_DISK_FULL
#define STATUS_DISK_FULL                 ((NTSTATUS)0xC000007FL)
#endif
#ifndef STATUS_INSUFFICIENT_RESOURCES
#define STATUS_INSUFFICIENT_RESOURCES    ((NTSTATUS)0xC000009AL)
#endif
#ifndef STATUS_DEVICE_DATA_ERROR
#define STATUS_DEVICE_DATA_ERROR         ((NTSTATUS)0xC000009CL)
#endif
#ifndef STATUS_MEDIA_WRITE_PROTECTED
#define STATUS_MEDIA_WRITE_PROTECTED     ((NTSTATUS)0xC00000A2L)
#endif
#ifndef STATUS_FILE_IS_A_DIRECTORY
#define STATUS_FILE_IS_A_DIRECTORY       ((NTSTATUS)0xC00000BAL)
#endif
#ifndef STATUS_NOT_SUPPORTED
#define STATUS_NOT_SUPPORTED             ((NTSTATUS)0xC00000BBL)
#endif
#ifndef STATUS_DIRECTORY_NOT_EMPTY
#define STATUS_DIRECTORY_NOT_EMPTY       ((NTSTATUS)0xC0000101L)
#endif
#ifndef STATUS_NOT_A_DIRECTORY
#define STATUS_NOT_A_DIRECTORY           ((NTSTATUS)0xC0000103L)
#endif
#ifndef STATUS_CANNOT_DELETE
#define STATUS_CANNOT_DELETE             ((NTSTATUS)0xC0000121L)
#endif

#define FSP_FSCTL_DEFAULT_ALIGN_UP(x, s) (((x) + ((s) - 1)) & ~((s) - 1))
#define FSP_FSCTL_TRANSACT_PATH_SIZEMAX  (1024 * sizeof(WCHAR))
#define FSP_FSCTL_VOLUME_PREFIX_SIZE     (192 * sizeof(WCHAR))
#define FSP_FSCTL_VOLUME_FSNAME_SIZE     (16 * sizeof(WCHAR))

enum {
    FspCleanupDelete = 0x01,
    FspCleanupSetAllocationSize = 0x02,
    FspCleanupSetArchiveBit = 0x10,
    FspCleanupSetLastAccessTime = 0x20,
    FspCleanupSetLastWriteTime = 0x40,
    FspCleanupSetChangeTime = 0x80,
};

typedef struct {
    UINT16 Version;
    UINT16 SectorSize;
    UINT16 SectorsPerAllocationUnit;
    UINT16 MaxComponentLength;
    UINT64 VolumeCreationTime;
    UINT32 VolumeSerialNumber;
    UINT32 TransactTimeout;
    UINT32 IrpTimeout;
    UINT32 IrpCapacity;
    UINT32 FileInfoTimeout;
    UINT32 CaseSensitiveSearch : 1;
    UINT32 CasePreservedNames : 1;
    UINT32 UnicodeOnDisk : 1;
    UINT32 PersistentAcls : 1;
    UINT32 ReparsePoints : 1;
    UINT32 ReparsePointsAccessCheck : 1;
    UINT32 NamedStreams : 1;
    UINT32 HardLinks : 1;
    UINT32 ExtendedAttributes : 1;
    UINT32 ReadOnlyVolume : 1;
    UINT32 PostCleanupWhenModifiedOnly : 1;
    UINT32 PassQueryDirectoryPattern : 1;
    UINT32 AlwaysUseDoubleBuffering : 1;
    UINT32 PassQueryDirectoryFileName : 1;
    UINT32 FlushAndPurgeOnCleanup : 1;
    UINT32 DeviceControl : 1;
    UINT32 UmFileContextIsUserContext2 : 1;     /* bit 16 */
    UINT32 UmFileContextIsFullContext : 1;      /* bit 17 */
    UINT32 UmNoReparsePointsDirCheck : 1;       /* bit 18 */
    UINT32 UmReservedFlags : 5;                 /* bits 19-23 */
    UINT32 AllowOpenInKernelMode : 1;           /* bit 24 */
    UINT32 CasePreservedExtendedAttributes : 1; /* bit 25 */
    UINT32 WslFeatures : 1;                     /* bit 26 */
    UINT32 DirectoryMarkerAsNextOffset : 1;     /* bit 27 */
    UINT32 RejectIrpPriorToTransact0 : 1;       /* bit 28 */
    UINT32 SupportsPosixUnlinkRename : 1;       /* bit 29 */
    UINT32 PostDispositionWhenNecessaryOnly : 1;/* bit 30 */
    UINT32 KmReservedFlags : 1;                 /* bit 31 */
    WCHAR  Prefix[FSP_FSCTL_VOLUME_PREFIX_SIZE / sizeof(WCHAR)];       /* +0x028 */
    WCHAR  FileSystemName[FSP_FSCTL_VOLUME_FSNAME_SIZE / sizeof(WCHAR)];/* +0x1A8 */
    UINT32 VolumeInfoTimeoutValid : 1;          /* +0x1C8 AdditionalFlags */
    UINT32 DirInfoTimeoutValid : 1;
    UINT32 SecurityTimeoutValid : 1;
    UINT32 StreamInfoTimeoutValid : 1;
    UINT32 EaTimeoutValid : 1;
    UINT32 KmAdditionalReservedFlags : 27;
    UINT32 VolumeInfoTimeout;
    UINT32 DirInfoTimeout;
    UINT32 SecurityTimeout;
    UINT32 StreamInfoTimeout;
    UINT32 EaTimeout;
    UINT32 FsextControlCode;
    UINT32 Reserved32[1];
    UINT64 Reserved64[2];
} FSP_FSCTL_VOLUME_PARAMS;

/* ABI layout verified against the official WinFsp 2.x runtime (winfsp-msil.dll
 * Fsp.Interop.VolumeParams: SizeOf=504, Prefix@0x28, FileSystemName@0x1A8,
 * AdditionalFlags@0x1C8; winfsp-x64.dll FspVolumeCreate uses 0x1C8 for Version==0). */
#define FSP_FSCTL_VOLUME_PARAMS_V0_SIZE 0x1C8
#define FSP_ABI_ASSERT(name, cond) typedef char fsp_abi_assert_##name[(cond) ? 1 : -1]
FSP_ABI_ASSERT(volparams_size, sizeof(FSP_FSCTL_VOLUME_PARAMS) == 504);
FSP_ABI_ASSERT(volparams_prefix, offsetof(FSP_FSCTL_VOLUME_PARAMS, Prefix) == 0x28);
FSP_ABI_ASSERT(volparams_fsname, offsetof(FSP_FSCTL_VOLUME_PARAMS, FileSystemName) == 0x1A8);
FSP_ABI_ASSERT(volparams_vitimeout, offsetof(FSP_FSCTL_VOLUME_PARAMS, VolumeInfoTimeout) == 0x1CC);

typedef struct {
    UINT64 TotalSize;
    UINT64 FreeSize;
    UINT16 VolumeLabelLength;
    WCHAR  VolumeLabel[32];
} FSP_FSCTL_VOLUME_INFO;

typedef struct {
    UINT32 FileAttributes;
    UINT32 ReparseTag;
    UINT64 AllocationSize;
    UINT64 FileSize;
    UINT64 CreationTime;
    UINT64 LastAccessTime;
    UINT64 LastWriteTime;
    UINT64 ChangeTime;
    UINT64 IndexNumber;
    UINT32 HardLinks;
    UINT32 EaSize;
} FSP_FSCTL_FILE_INFO;

typedef struct {
    FSP_FSCTL_FILE_INFO FileInfo;
    PWSTR NormalizedName;
    UINT16 NormalizedNameSize;
} FSP_FSCTL_OPEN_FILE_INFO;

typedef struct {
    UINT16 Size;
    FSP_FSCTL_FILE_INFO FileInfo;
    union {
        UINT64 NextOffset;
        UINT8 Padding[24];
    };
    WCHAR FileNameBuf[];
} FSP_FSCTL_DIR_INFO;

FSP_ABI_ASSERT(fileinfo_size, sizeof(FSP_FSCTL_FILE_INFO) == 72);
FSP_ABI_ASSERT(dirinfo_fileinfo, offsetof(FSP_FSCTL_DIR_INFO, FileInfo) == 0x08);
FSP_ABI_ASSERT(dirinfo_namebuf, offsetof(FSP_FSCTL_DIR_INFO, FileNameBuf) == 0x68);

typedef struct {
    UINT16 Size;
    UINT64 StreamSize;
    UINT64 StreamAllocationSize;
    WCHAR StreamNameBuf[];
} FSP_FSCTL_STREAM_INFO;

typedef struct _FSP_FILE_SYSTEM FSP_FILE_SYSTEM;
typedef struct _FSP_FILE_SYSTEM_INTERFACE FSP_FILE_SYSTEM_INTERFACE;

struct _FSP_FILE_SYSTEM_INTERFACE {
    NTSTATUS (*GetVolumeInfo)(FSP_FILE_SYSTEM *FileSystem, FSP_FSCTL_VOLUME_INFO *VolumeInfo);
    NTSTATUS (*SetVolumeLabel)(FSP_FILE_SYSTEM *FileSystem, PWSTR VolumeLabel,
                               FSP_FSCTL_VOLUME_INFO *VolumeInfo);
    NTSTATUS (*GetSecurityByName)(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName,
                                  PUINT32 PFileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor,
                                  SIZE_T *PSecurityDescriptorSize);
    NTSTATUS (*Create)(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName, UINT32 CreateOptions,
                       UINT32 GrantedAccess, UINT32 FileAttributes,
                       PSECURITY_DESCRIPTOR SecurityDescriptor, UINT64 AllocationSize,
                       PVOID *PFileContext, FSP_FSCTL_FILE_INFO *FileInfo);
    NTSTATUS (*Open)(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName, UINT32 CreateOptions,
                     UINT32 GrantedAccess, PVOID *PFileContext, FSP_FSCTL_FILE_INFO *FileInfo);
    NTSTATUS (*Overwrite)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT32 FileAttributes,
                          BOOLEAN ReplaceFileAttributes, UINT64 AllocationSize,
                          FSP_FSCTL_FILE_INFO *FileInfo);
    VOID (*Cleanup)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName, ULONG Flags);
    VOID (*Close)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext);
    NTSTATUS (*Read)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PVOID Buffer,
                     UINT64 Offset, ULONG Length, PULONG PBytesTransferred);
    NTSTATUS (*Write)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PVOID Buffer,
                      UINT64 Offset, ULONG Length, BOOLEAN WriteToEndOfFile,
                      BOOLEAN ConstrainedIo, PULONG PBytesTransferred,
                      FSP_FSCTL_FILE_INFO *FileInfo);
    NTSTATUS (*Flush)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
                      FSP_FSCTL_FILE_INFO *FileInfo);
    NTSTATUS (*GetFileInfo)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
                            FSP_FSCTL_FILE_INFO *FileInfo);
    NTSTATUS (*SetBasicInfo)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT32 FileAttributes,
                             UINT64 CreationTime, UINT64 LastAccessTime, UINT64 LastWriteTime,
                             UINT64 ChangeTime, FSP_FSCTL_FILE_INFO *FileInfo);
    NTSTATUS (*SetFileSize)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT64 NewSize,
                            BOOLEAN SetAllocationSize, FSP_FSCTL_FILE_INFO *FileInfo);
    NTSTATUS (*CanDelete)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName);
    NTSTATUS (*Rename)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName,
                       PWSTR NewFileName, BOOLEAN ReplaceIfExists);
    NTSTATUS (*GetSecurity)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
                            PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T *PSecurityDescriptorSize);
    NTSTATUS (*SetSecurity)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
                            SECURITY_INFORMATION SecurityInformation,
                            PSECURITY_DESCRIPTOR ModificationDescriptor);
    NTSTATUS (*ReadDirectory)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR Pattern,
                              PWSTR Marker, PVOID Buffer, ULONG Length, PULONG PBytesTransferred);
    NTSTATUS (*ResolveReparsePoints)(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName,
                                     UINT32 ReparsePointIndex, BOOLEAN ResolveLastPathComponent,
                                     PIO_STATUS_BLOCK PIoStatus, PVOID Buffer, PSIZE_T PSize);
    NTSTATUS (*GetReparsePoint)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName,
                                PVOID Buffer, PSIZE_T PSize);
    NTSTATUS (*SetReparsePoint)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName,
                                PVOID Buffer, SIZE_T Size);
    NTSTATUS (*DeleteReparsePoint)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName,
                                   PVOID Buffer, SIZE_T Size);
    NTSTATUS (*GetStreamInfo)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PVOID Buffer,
                              ULONG Length, PULONG PBytesTransferred);
    NTSTATUS (*GetDirInfoByName)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName,
                                 FSP_FSCTL_DIR_INFO *DirInfo);
    NTSTATUS (*Control)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT32 ControlCode,
                        PVOID InputBuffer, ULONG InputBufferLength, PVOID OutputBuffer,
                        ULONG OutputBufferLength, PULONG PBytesTransferred);
    NTSTATUS (*SetDelete)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName,
                          BOOLEAN DeleteFile);
    NTSTATUS (*CreateEx)(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName, UINT32 CreateOptions,
                         UINT32 GrantedAccess, UINT32 FileAttributes,
                         PSECURITY_DESCRIPTOR SecurityDescriptor, UINT64 AllocationSize,
                         PVOID ExtraBuffer, ULONG ExtraLength, BOOLEAN ExtraBufferIsReparsePoint,
                         PVOID *PFileContext, FSP_FSCTL_FILE_INFO *FileInfo);
    NTSTATUS (*OverwriteEx)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT32 FileAttributes,
                            BOOLEAN ReplaceFileAttributes, UINT64 AllocationSize,
                            PFILE_FULL_EA_INFORMATION Ea, ULONG EaLength,
                            FSP_FSCTL_FILE_INFO *FileInfo);
    NTSTATUS (*GetEa)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PFILE_FULL_EA_INFORMATION Ea,
                      ULONG EaLength, PULONG PBytesTransferred);
    NTSTATUS (*SetEa)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PFILE_FULL_EA_INFORMATION Ea,
                      ULONG EaLength, FSP_FSCTL_FILE_INFO *FileInfo);
    NTSTATUS (*Obsolete0)(VOID);
    VOID (*DispatcherStopped)(FSP_FILE_SYSTEM *FileSystem, BOOLEAN Normally);
    PVOID Reserved[31];
};

struct _FSP_FILE_SYSTEM {
    UINT32 Version;
    PVOID UserContext;
    WCHAR VolumeName[256];
    HANDLE VolumeHandle;
    const FSP_FILE_SYSTEM_INTERFACE *Interface;
    PVOID DispatcherThread;
    UINT32 DispatcherThreadCount;
    NTSTATUS DispatcherResult;
    PWSTR MountPoint;
    HANDLE MountHandle;
    UINT32 DebugLog;
    UINT32 UmFileContextIsUserContext2 : 1;
    UINT32 UmFileContextIsFullContext : 1;
};

/* Dynamic WinFsp DLL loader */
typedef NTSTATUS (*PFN_FspFileSystemCreate)(PWSTR DevicePath,
                                            const FSP_FSCTL_VOLUME_PARAMS *VolumeParams,
                                            const FSP_FILE_SYSTEM_INTERFACE *Interface,
                                            FSP_FILE_SYSTEM **PFileSystem);
typedef VOID (*PFN_FspFileSystemDelete)(FSP_FILE_SYSTEM *FileSystem);
typedef NTSTATUS (*PFN_FspFileSystemSetMountPoint)(FSP_FILE_SYSTEM *FileSystem, PWSTR MountPoint);
typedef NTSTATUS (*PFN_FspFileSystemStartDispatcher)(FSP_FILE_SYSTEM *FileSystem, ULONG ThreadCount);
typedef VOID (*PFN_FspFileSystemStopDispatcher)(FSP_FILE_SYSTEM *FileSystem);
typedef BOOLEAN (*PFN_FspFileSystemAddDirInfo)(FSP_FSCTL_DIR_INFO *DirInfo, PVOID Buffer,
                                               ULONG Length, PULONG PBytesTransferred);
typedef NTSTATUS (*PFN_FspFileSystemSetDebugLogF)(FSP_FILE_SYSTEM *FileSystem, UINT32 DebugLog);
typedef VOID (*PFN_FspDebugLogSetHandle)(HANDLE Handle);

typedef struct {
    HMODULE Module;
    PFN_FspFileSystemCreate Create;
    PFN_FspFileSystemDelete Delete;
    PFN_FspFileSystemSetMountPoint SetMountPoint;
    PFN_FspFileSystemStartDispatcher StartDispatcher;
    PFN_FspFileSystemStopDispatcher StopDispatcher;
    PFN_FspFileSystemAddDirInfo AddDirInfo;
    PFN_FspFileSystemSetDebugLogF SetDebugLogF;
    PFN_FspDebugLogSetHandle DebugLogSetHandle;
} FSP_RUNTIME_API;

static __inline HMODULE FspTryLoadDll(void) {
    static const wchar_t *kRegPaths[] = {
        L"SOFTWARE\\WOW6432Node\\WinFsp",
        L"SOFTWARE\\WinFsp",
    };
    HMODULE mod = LoadLibraryW(L"winfsp-x64.dll");
    if (mod) return mod;

    for (size_t i = 0; i < sizeof(kRegPaths) / sizeof(kRegPaths[0]); i++) {
        HKEY hKey = NULL;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegPaths[i], 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            WCHAR dir[MAX_PATH];
            DWORD sz = sizeof(dir);
            DWORD type = 0;
            if (RegQueryValueExW(hKey, L"InstallDir", NULL, &type, (LPBYTE)dir, &sz) == ERROR_SUCCESS &&
                (type == REG_SZ || type == REG_EXPAND_SZ)) {
                WCHAR dllPath[MAX_PATH * 2];
                size_t len = wcslen(dir);
                if (len > 0 && (dir[len - 1] == L'\\' || dir[len - 1] == L'/')) {
                    _snwprintf_s(dllPath, _countof(dllPath), _TRUNCATE, L"%sbin\\winfsp-x64.dll", dir);
                } else {
                    _snwprintf_s(dllPath, _countof(dllPath), _TRUNCATE, L"%s\\bin\\winfsp-x64.dll", dir);
                }
                mod = LoadLibraryW(dllPath);
                if (mod) {
                    RegCloseKey(hKey);
                    return mod;
                }
            }
            RegCloseKey(hKey);
        }
    }

    mod = LoadLibraryW(L"C:\\Program Files (x86)\\WinFsp\\bin\\winfsp-x64.dll");
    if (mod) return mod;
    return LoadLibraryW(L"C:\\Program Files\\WinFsp\\bin\\winfsp-x64.dll");
}

static __inline FSP_RUNTIME_API *FspGetRuntimeApi(void) {
    static FSP_RUNTIME_API api = {0};
    static int initialized = 0;
    if (!initialized) {
        HMODULE mod = FspTryLoadDll();
        if (mod) {
            api.Module = mod;
            api.Create = (PFN_FspFileSystemCreate)(void *)GetProcAddress(mod, "FspFileSystemCreate");
            api.Delete = (PFN_FspFileSystemDelete)(void *)GetProcAddress(mod, "FspFileSystemDelete");
            api.SetMountPoint = (PFN_FspFileSystemSetMountPoint)(void *)GetProcAddress(mod, "FspFileSystemSetMountPoint");
            api.StartDispatcher = (PFN_FspFileSystemStartDispatcher)(void *)GetProcAddress(mod, "FspFileSystemStartDispatcher");
            api.StopDispatcher = (PFN_FspFileSystemStopDispatcher)(void *)GetProcAddress(mod, "FspFileSystemStopDispatcher");
            api.AddDirInfo = (PFN_FspFileSystemAddDirInfo)(void *)GetProcAddress(mod, "FspFileSystemAddDirInfo");
            api.SetDebugLogF = (PFN_FspFileSystemSetDebugLogF)(void *)GetProcAddress(mod, "FspFileSystemSetDebugLogF");
            api.DebugLogSetHandle = (PFN_FspDebugLogSetHandle)(void *)GetProcAddress(mod, "FspDebugLogSetHandle");
        }
        initialized = 1;
    }
    return &api;
}

static __inline BOOLEAN FspFileSystemAddDirInfoFallback(FSP_FSCTL_DIR_INFO *DirInfo, PVOID Buffer,
                                                       ULONG Length, PULONG PBytesTransferred) {
    FSP_RUNTIME_API *api = FspGetRuntimeApi();
    if (api && api->AddDirInfo) {
        return api->AddDirInfo(DirInfo, Buffer, Length, PBytesTransferred);
    }
    if (!DirInfo) {
        if (*PBytesTransferred + sizeof(UINT16) <= Length) {
            ((FSP_FSCTL_DIR_INFO *)((PUINT8)Buffer + *PBytesTransferred))->Size = 0;
        }
        return FALSE;
    }
    {
        ULONG alignedSize = FSP_FSCTL_DEFAULT_ALIGN_UP((ULONG)DirInfo->Size, 8);
        if (*PBytesTransferred + alignedSize > Length) {
            return FALSE;
        }
        memcpy((PUINT8)Buffer + *PBytesTransferred, DirInfo, DirInfo->Size);
        if (alignedSize > DirInfo->Size) {
            memset((PUINT8)Buffer + *PBytesTransferred + DirInfo->Size, 0, alignedSize - DirInfo->Size);
        }
        *PBytesTransferred += alignedSize;
        return TRUE;
    }
}

#ifndef FspFileSystemAddDirInfo
#define FspFileSystemAddDirInfo FspFileSystemAddDirInfoFallback
#endif

#ifdef __cplusplus
}
#endif

#endif /* WINFSP_WINFSP_H_INCLUDED */
