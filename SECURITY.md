# Security Policy

## Supported Versions

| Version | Supported |
| :--- | :--- |
| Latest release (`v0.10.x` / `main`) | Yes |
| Older releases (`<= v0.9.x`) | Best-effort / No |

## Security Boundaries in ZeroExt4

ZeroExt4 parses untrusted filesystem images and interacts with raw Windows disk volumes and elevated mount points. Reports touching any of the following boundaries are treated with high priority:

- **Partition & Disk Boundary Isolation**: Out-of-bounds reads/writes escaping `[partition_offset_bytes, partition_offset_bytes + partition_size_bytes)` across GPT / MBR / EBR or `\\.\PhysicalDriveN`.
- **Dirty Journal & Corruption Protection**: Bypassing `NEEDS_RECOVERY` / `HAS_ERRORS` checks during Read-Write mounts without explicit override flags.
- **Overlay Isolation & Path Safety**: Lower-volume modification when `--overlay` is active, `.wh.__ext4_lower_id` bypasses, path traversal, or WinFsp reparse/namespace edge cases.
- **Elevation & Token Impersonation**: Privilege boundaries around `ADR-0009` shell token duplication (`ImpersonateLoggedOnUser`) and IPC handling between `ZeroExt4Gui.exe` and `zeroext4.exe`.
- **Unmounting & Drive Cleanup**: Accidental removal of non-ZeroExt4 / non-WinFsp Windows drive letters or volume mount points.
- **Memory Safety**: Heap/stack buffer overflows, use-after-free, or race conditions when parsing untrusted ext2/ext3/ext4 images or WinFsp callbacks.

## Reporting a Vulnerability

Please **do not** report security vulnerabilities through public GitHub issues, discussions, or pull requests.

Instead, report them privately via:
- **GitHub Security Advisories** (`Security` tab -> `Report a vulnerability`) for [`totatake/ZeroExt4`](https://github.com/totatake/ZeroExt4), or
- Direct private contact with the repository maintainer (`totatake`).

Please include:
1. Affected version / commit SHA and Windows / WinFsp version.
2. Minimal reproduction steps, CLI flags, or anonymized/sanitized disk image details.
3. Expected vs. observed impact (e.g., unintended write, incorrect drive unmount, privilege issue, crash, or sanitizer finding).

We aim to acknowledge reports within 72 hours and coordinate a fix before public disclosure.
