# Third-Party Notices

ZeroExt incorporates or interoperates with the following third-party software components.

## 1. e2fsprogs (`libext2fs` / `libet`)

- **Location**: `third_party/e2fsprogs/`
- **License**: GNU General Public License, version 2 (GPLv2)
- **Usage**: Statically linked into `zeroext_core` (`zeroext.exe`) for ext2/ext3/ext4 metadata parsing, extent tree inspection, and filesystem operations.
- **Note**: Because ZeroExt links with `libext2fs`, ZeroExt itself is distributed under the terms of the GNU General Public License, version 2 (see [LICENSE](LICENSE)).

## 2. WinFsp (Windows File System Proxy)

- **Website**: <https://winfsp.dev/> / <https://github.com/winfsp/winfsp>
- **License**: GNU General Public License, version 3 (GPLv3) with the WinFsp FOSS License Exception
- **Usage**: WinFsp headers are used for FSD interface definitions, and `winfsp-x64.dll` is loaded dynamically at runtime (`FspLoad` / `LoadLibraryW`) from the user-installed WinFsp runtime. It is not bundled in this repository or its binaries, and remains subject to WinFsp's own license terms.
