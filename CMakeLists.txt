cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")
project(ZeroExt4 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(ZEROEXT4_ENABLE_ASAN "Enable AddressSanitizer (/fsanitize=address)" OFF)
option(ZEROEXT4_ENABLE_LEAK_CHECK "Enable MSVC CRT Debug Heap leak check in zeroext4_tests" OFF)
option(ZEROEXT4_ENABLE_ANALYZE "Enable MSVC static code analysis (/analyze) on ZeroExt4 sources" OFF)

if(MSVC AND ZEROEXT4_ENABLE_ASAN)
    foreach(_flag_var CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_C_FLAGS)
        string(REGEX REPLACE "/RTC[1csu]+" "" ${_flag_var} "${${_flag_var}}")
    endforeach()
    add_compile_options(/fsanitize=address)
    add_link_options(/INCREMENTAL:NO)
endif()

# Locate real Python 3 (avoiding WindowsApps store stub alias)
set(_PY_CANDIDATE "$ENV{LOCALAPPDATA}/Python/bin/python.exe")
if(NOT DEFINED ZEROEXT4_PYTHON AND EXISTS "${_PY_CANDIDATE}")
    set(ZEROEXT4_PYTHON "${_PY_CANDIDATE}")
endif()
if(NOT DEFINED ZEROEXT4_PYTHON)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(ZEROEXT4_PYTHON "${Python3_EXECUTABLE}")
endif()

set(E2FSPROGS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/e2fsprogs")
set(E2FSPROGS_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(E2FSPROGS_ET_IN "${E2FSPROGS_ROOT}/lib/ext2fs/ext2_err.et.in")
set(E2FSPROGS_TYPES_IN "${E2FSPROGS_ROOT}/lib/ext2fs/ext2_types.h.in")
set(E2FSPROGS_VERSION_H "${E2FSPROGS_ROOT}/version.h")
set(E2FSPROGS_GEN_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_e2fsprogs_headers.py")

set(E2FSPROGS_GEN_ERR_H "${E2FSPROGS_GEN_DIR}/ext2fs/ext2_err.h")
set(E2FSPROGS_GEN_ERR_C "${E2FSPROGS_GEN_DIR}/ext2_err.c")
set(E2FSPROGS_GEN_TYPES_H "${E2FSPROGS_GEN_DIR}/ext2fs/ext2_types.h")
set(E2FSPROGS_GEN_CRC_H "${E2FSPROGS_GEN_DIR}/ext2fs/crc32c_table.h")

# Generate once at configure time so initial header scanning sees all generated headers
execute_process(
    COMMAND "${ZEROEXT4_PYTHON}" "${E2FSPROGS_GEN_SCRIPT}"
            --et-file "${E2FSPROGS_ET_IN}"
            --types-file "${E2FSPROGS_TYPES_IN}"
            --version-file "${E2FSPROGS_VERSION_H}"
            --out-dir "${E2FSPROGS_GEN_DIR}"
    RESULT_VARIABLE _GEN_RES
)
if(NOT _GEN_RES EQUAL 0)
    message(FATAL_ERROR "Failed to generate e2fsprogs headers via ${ZEROEXT4_PYTHON}")
endif()

add_custom_command(
    OUTPUT "${E2FSPROGS_GEN_ERR_H}" "${E2FSPROGS_GEN_ERR_C}" "${E2FSPROGS_GEN_TYPES_H}" "${E2FSPROGS_GEN_CRC_H}"
    COMMAND "${ZEROEXT4_PYTHON}" "${E2FSPROGS_GEN_SCRIPT}"
            --et-file "${E2FSPROGS_ET_IN}"
            --types-file "${E2FSPROGS_TYPES_IN}"
            --version-file "${E2FSPROGS_VERSION_H}"
            --out-dir "${E2FSPROGS_GEN_DIR}"
    DEPENDS "${E2FSPROGS_GEN_SCRIPT}" "${E2FSPROGS_ET_IN}" "${E2FSPROGS_TYPES_IN}" "${E2FSPROGS_VERSION_H}"
    COMMENT "Generating ext2_err.h, ext2_err.c, ext2_types.h, and crc32c_table.h from upstream e2fsprogs templates"
    VERBATIM
)

add_custom_target(generate_e2fsprogs_headers
    DEPENDS "${E2FSPROGS_GEN_ERR_H}" "${E2FSPROGS_GEN_ERR_C}" "${E2FSPROGS_GEN_TYPES_H}" "${E2FSPROGS_GEN_CRC_H}"
)

# Static library: upstream e2fsprogs 1.47.4 (lib/et + platform-independent lib/ext2fs)
add_library(ext2fs STATIC
    "${E2FSPROGS_GEN_ERR_C}"
    "${E2FSPROGS_ROOT}/lib/et/com_err.c"
    "${E2FSPROGS_ROOT}/lib/et/error_message.c"
    "${E2FSPROGS_ROOT}/lib/et/et_name.c"
    "${E2FSPROGS_ROOT}/lib/et/init_et.c"
    "${E2FSPROGS_ROOT}/lib/et/com_right.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/alloc.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/alloc_sb.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/alloc_stats.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/alloc_tables.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/badblocks.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/bitmaps.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/bitops.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/blkmap64_ba.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/blkmap64_rb.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/blknum.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/block.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/bmap.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/check_desc.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/closefs.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/crc16.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/crc32c.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/csum.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/dir_iterate.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/dirblock.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/dirhash.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/expanddir.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/ext_attr.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/extent.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/fallocate.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/fileio.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/freefs.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/gen_bitmap.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/gen_bitmap64.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/get_num_dirs.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/getenv.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/hashmap.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/i_block.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/ind_block.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/initialize.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/inline.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/inline_data.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/inode.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/io_manager.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/ismounted.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/link.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/lookup.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/mkdir.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/mkjournal.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/mmp.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/namei.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/native.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/newdir.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/nls_utf8.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/openfs.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/punch.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/rbtree.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/read_bb.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/rw_bitmaps.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/sha512.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/swapfs.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/symlink.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/unlink.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/valid_blk.c"
    "${E2FSPROGS_ROOT}/lib/ext2fs/version.c"
)
add_dependencies(ext2fs generate_e2fsprogs_headers)
target_include_directories(ext2fs
    PUBLIC
        "${E2FSPROGS_GEN_DIR}"
        "${E2FSPROGS_ROOT}/lib"
    PRIVATE
        "${E2FSPROGS_GEN_DIR}/ext2fs"
        "${E2FSPROGS_ROOT}/lib/ext2fs"
        "${CMAKE_CURRENT_SOURCE_DIR}/compat/include"
)
if(MSVC)
    target_compile_options(ext2fs PRIVATE
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/compat/msvc_compat.h"
        /W3
    )
    target_compile_options(ext2fs INTERFACE
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/compat/msvc_public_compat.h"
    )
endif()

# Static library: Win32 I/O, Disk/Partition/Journal Probe, Modular VFS, WinFsp Interface
add_library(zeroext4_core STATIC
    src/common/ext4_errno.c
    src/trace/perfetto_trace.c
    src/io/win32_io.c
    src/disk/superblock_probe.c
    src/disk/partition_table.c
    src/disk/disk_scanner.c
    src/extent/extent_map.c
    src/overlay/overlay_vfs.c
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
target_include_directories(zeroext4_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/winfsp/include"
)
target_compile_definitions(zeroext4_core PUBLIC
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
if(MSVC)
    target_compile_options(zeroext4_core PRIVATE /W4 /wd4100 /wd4200 /wd4201)
    if(ZEROEXT4_ENABLE_ANALYZE)
        target_compile_options(zeroext4_core PRIVATE /analyze)
    endif()
endif()
target_link_libraries(zeroext4_core PUBLIC ext2fs advapi32 shell32 user32 version)

# CLI executable: zeroext4.exe
add_executable(zeroext4 src/cli/main.c)
target_link_libraries(zeroext4 PRIVATE zeroext4_core ext2fs dbghelp)
# Map file: resolve crash offsets reported by the crash filter (module+0xNNN) to functions
target_link_options(zeroext4 PRIVATE /MAP)
if(MSVC)
    target_compile_options(zeroext4 PRIVATE /W4 /wd4100 /wd4200)
    if(ZEROEXT4_ENABLE_ANALYZE)
        target_compile_options(zeroext4 PRIVATE /analyze)
    endif()
endif()

# Test executable and CTest registration
enable_testing()
add_executable(zeroext4_tests
    tests/test_main.c
    tests/test_win32_io.c
    tests/test_partition_and_probe.c
    tests/test_vfs_operations.c
    tests/test_winfsp_callbacks.c
    tests/test_extent_and_overlay.c
    tests/test_perfetto_trace.c
)
target_link_libraries(zeroext4_tests PRIVATE zeroext4_core ext2fs)
if(MSVC)
    target_compile_options(zeroext4_tests PRIVATE /W4 /wd4100 /wd4200)
    if(ZEROEXT4_ENABLE_LEAK_CHECK)
        target_compile_definitions(zeroext4_tests PRIVATE ZEROEXT4_ENABLE_LEAK_CHECK=1)
    endif()
    if(ZEROEXT4_ENABLE_ANALYZE)
        target_compile_options(zeroext4_tests PRIVATE /analyze)
    endif()
endif()

add_test(NAME win32_io            COMMAND zeroext4_tests --suite win32_io)
add_test(NAME partition_and_probe COMMAND zeroext4_tests --suite partition_and_probe)
add_test(NAME vfs_operations      COMMAND zeroext4_tests --suite vfs_operations)
add_test(NAME winfsp_callbacks    COMMAND zeroext4_tests --suite winfsp_callbacks)
add_test(NAME extent_and_overlay  COMMAND zeroext4_tests --suite extent_and_overlay)
add_test(NAME perfetto_trace      COMMAND zeroext4_tests --suite perfetto_trace)

# Benchmark executable: zeroext4_bench.exe (Direct I/O vs standard VFS/WinFsp read)
add_executable(zeroext4_bench tests/bench_direct_io.c)
target_link_libraries(zeroext4_bench PRIVATE zeroext4_core ext2fs)
if(MSVC)
    target_compile_options(zeroext4_bench PRIVATE /W4 /wd4100 /wd4200)
endif()
