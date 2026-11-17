cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")
project(libext2fs_win C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Locate real Python 3 (avoiding WindowsApps store stub alias)
set(_PY_CANDIDATE "$ENV{LOCALAPPDATA}/Python/bin/python.exe")
if(NOT DEFINED EXT4FSP_PYTHON AND EXISTS "${_PY_CANDIDATE}")
    set(EXT4FSP_PYTHON "${_PY_CANDIDATE}")
endif()
if(NOT DEFINED EXT4FSP_PYTHON)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(EXT4FSP_PYTHON "${Python3_EXECUTABLE}")
endif()

set(E2FSPROGS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/e2fsprogs")
set(E2FSPROGS_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(E2FSPROGS_ET_IN "${E2FSPROGS_ROOT}/lib/ext2fs/ext2_err.et.in")
set(E2FSPROGS_TYPES_IN "${E2FSPROGS_ROOT}/lib/ext2fs/ext2_types.h.in")
set(E2FSPROGS_GEN_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_e2fsprogs_headers.py")

set(E2FSPROGS_GEN_ERR_H "${E2FSPROGS_GEN_DIR}/ext2fs/ext2_err.h")
set(E2FSPROGS_GEN_ERR_C "${E2FSPROGS_GEN_DIR}/ext2_err.c")
set(E2FSPROGS_GEN_TYPES_H "${E2FSPROGS_GEN_DIR}/ext2fs/ext2_types.h")

# Generate once at configure time so initial header scanning sees all generated headers
execute_process(
    COMMAND "${EXT4FSP_PYTHON}" "${E2FSPROGS_GEN_SCRIPT}"
            --et-file "${E2FSPROGS_ET_IN}"
            --types-file "${E2FSPROGS_TYPES_IN}"
            --out-dir "${E2FSPROGS_GEN_DIR}"
    RESULT_VARIABLE _GEN_RES
)
if(NOT _GEN_RES EQUAL 0)
    message(FATAL_ERROR "Failed to generate e2fsprogs headers via ${EXT4FSP_PYTHON}")
endif()

add_custom_command(
    OUTPUT "${E2FSPROGS_GEN_ERR_H}" "${E2FSPROGS_GEN_ERR_C}" "${E2FSPROGS_GEN_TYPES_H}"
    COMMAND "${EXT4FSP_PYTHON}" "${E2FSPROGS_GEN_SCRIPT}"
            --et-file "${E2FSPROGS_ET_IN}"
            --types-file "${E2FSPROGS_TYPES_IN}"
            --out-dir "${E2FSPROGS_GEN_DIR}"
    DEPENDS "${E2FSPROGS_GEN_SCRIPT}" "${E2FSPROGS_ET_IN}" "${E2FSPROGS_TYPES_IN}"
    COMMENT "Generating ext2_err.h, ext2_err.c, and ext2_types.h from upstream e2fsprogs templates"
    VERBATIM
)

add_custom_target(generate_e2fsprogs_headers
    DEPENDS "${E2FSPROGS_GEN_ERR_H}" "${E2FSPROGS_GEN_ERR_C}" "${E2FSPROGS_GEN_TYPES_H}"
)

# Static library: upstream e2fsprogs (lib/et + lib/ext2fs without OS-dependent unix_io.c etc.)
add_library(ext2fs STATIC
    "${E2FSPROGS_GEN_ERR_C}"
    "${E2FSPROGS_ROOT}/lib/et/com_err.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/io_manager.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/openfs.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/closefs.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/freefs.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/initialize.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/blknum.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/bitmaps.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/rw_bitmaps.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/bitops.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/gen_bitmap64.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/alloc.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/alloc_sb.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/alloc_stats.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/alloc_tables.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/inode.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/extent.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/bmap.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/block.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/ind_block.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/fileio.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/inline.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/inline_data.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/dirblock.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/dir_iterate.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/lookup.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/namei.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/mkdir.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/newdir.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/link.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/unlink.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/symlink.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/crc16.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/crc32c.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/csum.c"
)
add_dependencies(ext2fs generate_e2fsprogs_headers)
target_include_directories(ext2fs
    PUBLIC
        "${E2FSPROGS_GEN_DIR}"
        "${E2FSPROGS_ROOT}/lib"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/compat/include"
)
if(MSVC)
    target_compile_options(ext2fs PRIVATE
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/compat/msvc_compat.h"
        /W3
    )
endif()

# Static library: Win32 I/O, Disk/Partition/Journal Probe, Modular VFS, WinFsp Interface
add_library(ext4fsp_core STATIC
    src/common/ext4_errno.c
    src/io/win32_io.c
    src/disk/superblock_probe.c
    src/disk/partition_table.c
    src/disk/disk_scanner.c
    src/vfs/op_init.c
    src/vfs/op_getattr.c
    src/vfs/op_readdir.c
    src/vfs/op_open.c
    src/vfs/op_read.c
    src/vfs/op_write.c
    src/vfs/op_create.c
    src/vfs/op_mkdir.c
    src/vfs/op_unlink.c
    src/vfs/op_rename.c
    src/vfs/op_truncate.c
    src/vfs/op_symlink.c
    src/vfs/op_statfs.c
    src/vfs/op_flush.c
    src/driver/winfsp_driver.c
)
target_include_directories(ext4fsp_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/winfsp/include"
)
target_compile_definitions(ext4fsp_core PUBLIC
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
if(MSVC)
    target_compile_options(ext4fsp_core PRIVATE /W4 /wd4100 /wd4200 /wd4201)
endif()
target_link_libraries(ext4fsp_core PUBLIC ext2fs advapi32 shell32 user32)

# CLI executable: ext4fsp.exe
add_executable(ext4fsp src/cli/main.c)
target_link_libraries(ext4fsp PRIVATE ext4fsp_core ext2fs dbghelp)
# Map file: resolve crash offsets reported by the crash filter (module+0xNNN) to functions
target_link_options(ext4fsp PRIVATE /MAP)
if(MSVC)
    target_compile_options(ext4fsp PRIVATE /W4 /wd4100 /wd4200)
endif()

# Test executable and CTest registration
enable_testing()
add_executable(ext4fsp_tests
    tests/test_main.c
    tests/test_win32_io.c
    tests/test_partition_and_probe.c
    tests/test_vfs_operations.c
    tests/test_winfsp_callbacks.c
)
target_link_libraries(ext4fsp_tests PRIVATE ext4fsp_core ext2fs)
if(MSVC)
    target_compile_options(ext4fsp_tests PRIVATE /W4 /wd4100 /wd4200)
endif()

add_test(NAME win32_io            COMMAND ext4fsp_tests --suite win32_io)
add_test(NAME partition_and_probe COMMAND ext4fsp_tests --suite partition_and_probe)
add_test(NAME vfs_operations      COMMAND ext4fsp_tests --suite vfs_operations)
add_test(NAME winfsp_callbacks    COMMAND ext4fsp_tests --suite winfsp_callbacks)
