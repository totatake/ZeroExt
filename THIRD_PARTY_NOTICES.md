# Third-Party Notices

ZeroExt incorporates or interoperates with the following third-party software components.

## 1. e2fsprogs 1.47.4 (`libext2fs` / `libet` / `libe2p` header)

- **Location**: `third_party/e2fsprogs/`
- **Version**: `1.47.4` (`version.h`: `E2FSPROGS_VERSION "1.47.4"`)
- **License**: GNU General Public License, version 2 (GPLv2) / GNU Library General Public License, version 2 (LGPLv2)
- **Usage**: Statically linked into `zeroext_core` (`zeroext.exe`) for ext2/ext3/ext4 metadata parsing, extent tree inspection, and filesystem operations.
- **MSVC C11 Compatibility Adjustments** (reproducibly applied by `scripts/update_e2fsprogs.ps1`):
  - `lib/ext2fs/ext2fs.h`: changed `EXT2_QSORT_TYPE` from `int __cdecl` to `int` (`int __cdecl (*sortfunc)` triggers MSVC syntax error `C2059`).
  - `lib/ext2fs/alloc.c`, `lib/ext2fs/punch.c`, `lib/ext2fs/fallocate.c`: converted GNU named variadic macro `dbg_printf(f, a...)` / `## a` to standard C99 `dbg_printf(f, ...)` / `__VA_ARGS__`.
- **Note**: Because ZeroExt links with `libext2fs`, ZeroExt itself is distributed under the terms of the GNU General Public License, version 2 (see [LICENSE](LICENSE)).

## 2. WinFsp (Windows File System Proxy)

- **Website**: <https://winfsp.dev/> / <https://github.com/winfsp/winfsp>
- **License**: GNU General Public License, version 3 (GPLv3) with the WinFsp FOSS License Exception
- **Usage**: WinFsp headers are used for FSD interface definitions, and `winfsp-x64.dll` is loaded dynamically at runtime (`FspLoad` / `LoadLibraryW`) from the user-installed WinFsp runtime. It is not bundled in this repository or its binaries, and remains subject to WinFsp's own license terms.
