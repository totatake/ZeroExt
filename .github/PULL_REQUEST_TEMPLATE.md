## Summary

<!-- Explain what this PR changes and why. Link related issues (e.g., Fixes #123). -->

## Type of Change

- [ ] Bug fix (`fix`)
- [ ] New feature (`feat`)
- [ ] Performance improvement (`perf`)
- [ ] Documentation / ADR (`docs`)
- [ ] Refactor / Build / CI (`refactor`, `chore`)

## Verification

- [ ] `powershell -ExecutionPolicy Bypass -File .\scripts\build_and_test.ps1` passes (CTest + `ZeroExt4.Gui.Tests` + `--self-test`)
- [ ] `powershell -ExecutionPolicy Bypass -File .\scripts\run_sanitizers.ps1` passes (ASan + Debug CRT leak check + `/analyze`, when C sources changed)
- [ ] No direct edits to `third_party/e2fsprogs/` outside reproducible `scripts/update_e2fsprogs.ps1`
- [ ] Documentation updated (`docs/`, `CHANGELOG.md`, `README*.md`, or `docs/adr/` if architectural)
- [ ] Verified environment:
  - Windows version / MSVC / `.NET 10` / WinFsp runtime: `...`
