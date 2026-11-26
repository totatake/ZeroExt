---
name: Bug Report
about: Report a reproducible bug, mount failure, or unexpected behavior in ZeroExt4
title: "[Bug]: "
labels: bug
---

## Summary

A concise description of the bug.

## Environment

- **ZeroExt4 version / commit**: (e.g., `v0.10.0` or commit SHA)
- **Windows version & architecture**: (e.g., Windows 11 Pro 24H2 x64)
- **WinFsp version**: (e.g., `2025` / `2.1.x` or Not Installed)
- **Interface**: `zeroext4.exe` (CLI) / `ZeroExt4Gui.exe` (GUI)
- **Target source & mount mode**: Physical Disk / `.img` | Read-Only / `--overlay` / `--rw`

## Steps to Reproduce

1. Run `zeroext4.exe ...` or select options in `ZeroExt4Gui.exe`
2. Perform `...`
3. Observe `...`

## Expected Behavior

What you expected to happen.

## Actual Behavior / Logs

Paste relevant console output, `zeroext4 inspect` / `--debug` output, or Perfetto trace notes (do not include sensitive local paths or personal data).
