---
name: Bug Report
about: Report a reproducible bug, mount failure, or unexpected behavior in ZeroExt
title: "[Bug]: "
labels: bug
---

## Summary

A concise description of the bug.

## Environment

- **ZeroExt version / commit**: (e.g., `v0.7.0` or commit SHA)
- **Windows version & architecture**: (e.g., Windows 11 Pro 24H2 x64)
- **WinFsp version**: (e.g., `2025` / `2.1.x` or Not Installed)
- **Interface**: `zeroext.exe` (CLI) / `ZeroExtGui.exe` (GUI)
- **Target source & mount mode**: Physical Disk / `.img` | Read-Only / `--overlay` / `--rw`

## Steps to Reproduce

1. Run `zeroext.exe ...` or select options in `ZeroExtGui.exe`
2. Perform `...`
3. Observe `...`

## Expected Behavior

What you expected to happen.

## Actual Behavior / Logs

Paste relevant console output, `zeroext inspect` / `--debug` output, or Perfetto trace notes (do not include sensitive local paths or personal data).
