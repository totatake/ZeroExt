#!/usr/bin/env python3
"""
Generate ext2fs/ext2_err.h, ext2_err.c, ext2fs/ext2_types.h, and
ext2fs/crc32c_table.h from upstream e2fsprogs templates without requiring
autotools, awk, compile_et, or a host build of gen_crc32ctable.
"""

import argparse
import re
import sys
from pathlib import Path


def com_err_char_code(ch: str) -> int:
    if "A" <= ch <= "Z":
        return ord(ch) - ord("A") + 1
    if "a" <= ch <= "z":
        return ord(ch) - ord("a") + 27
    if "0" <= ch <= "9":
        return ord(ch) - ord("0") + 53
    if ch == "_":
        return 63
    return 0


def compute_error_table_base(table_name: str) -> int:
    table_num = 0
    for ch in table_name[:4]:
        table_num = (table_num << 6) | com_err_char_code(ch)
    base = table_num << 8
    # Convert to signed 32-bit if needed; for 'ext2' this is 2133571328 (0x7F2B3700)
    if base >= 0x80000000:
        base -= 0x100000000
    return base


def read_e2fsprogs_version(version_file: Path | None, et_path: Path) -> str:
    candidates = []
    if version_file is not None:
        candidates.append(version_file)
    if len(et_path.parents) >= 3:
        candidates.append(et_path.parents[2] / "version.h")
    for cand in candidates:
        if cand.exists():
            txt = cand.read_text(encoding="utf-8", errors="ignore")
            m = re.search(r'#define\s+E2FSPROGS_VERSION\s+"([^"]+)"', txt)
            if m:
                return m.group(1)
    return "1.47.4"


def parse_et_file(et_path: Path, version_str: str):
    text = et_path.read_text(encoding="utf-8").replace("@E2FSPROGS_VERSION@", version_str)
    lines = text.splitlines()
    table_name = "ext2"
    entries = []

    i = 0
    while i < len(lines):
        line = lines[i].strip()
        i += 1
        if not line or line.startswith("#"):
            continue
        m_table = re.match(r"^error_table\s+([A-Za-z0-9_]+)", line)
        if m_table:
            table_name = m_table.group(1)
            continue
        if line == "end":
            break
        if line.startswith("ec ") or line.startswith("error_code ") or line.startswith("ec\t") or line.startswith("error_code\t"):
            combined = line
            while (combined.count('"') < 2 or combined.count('"') % 2 != 0) and i < len(lines):
                combined += " " + lines[i].strip()
                i += 1
            m_ec = re.match(
                r'^(?:ec|error_code)\s+([A-Za-z0-9_]+)\s*,\s*"((?:[^"\\]|\\.)*)"',
                combined,
            )
            if m_ec:
                entries.append((m_ec.group(1), m_ec.group(2)))
    return table_name, entries


def write_if_changed(path: Path, content: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        old = path.read_text(encoding="utf-8")
        if old == content:
            return
    path.write_text(content, encoding="utf-8", newline="\n")


def generate_error_table(et_path: Path, out_dir: Path, version_str: str = "1.47.4"):
    table_name, entries = parse_et_file(et_path, version_str)
    base = compute_error_table_base(table_name)
    upper_name = table_name.upper()

    h_lines = [
        "/* Auto-generated from ext2_err.et.in by generate_e2fsprogs_headers.py */",
        f"#ifndef __{upper_name}_ERR_H__",
        f"#define __{upper_name}_ERR_H__",
        "",
        "#include <et/com_err.h>",
        "",
        f"#define {upper_name}_ET_BASE ({base}L)",
        "",
    ]
    for idx, (sym, _) in enumerate(entries):
        h_lines.append(f"#define {sym} ({base + idx}L)")
    h_lines.extend(
        [
            "",
            f"extern const struct error_table et_{table_name}_error_table;",
            f"extern void initialize_{table_name}_error_table(void);",
            "",
            "/* Backward compatibility alias */",
            f"#define initialize_{table_name}_error_table_r(list) add_error_table(list, &et_{table_name}_error_table)",
            "",
            f"#endif /* __{upper_name}_ERR_H__ */",
            "",
        ]
    )
    write_if_changed(out_dir / "ext2fs" / "ext2_err.h", "\n".join(h_lines))

    c_lines = [
        "/* Auto-generated from ext2_err.et.in by generate_e2fsprogs_headers.py */",
        "#include <stddef.h>",
        "#include <et/com_err.h>",
        "#include <et/error_table.h>",
        "#include <ext2fs/ext2_err.h>",
        "",
        "static const char *const text[] = {",
    ]
    for _, msg in entries:
        c_lines.append(f'    "{msg}",')
    c_lines.extend(
        [
            "    NULL",
            "};",
            "",
            f"const struct error_table et_{table_name}_error_table = {{ text, {base}L, {len(entries)} }};",
            "",
            f"static struct et_list link = {{ NULL, &et_{table_name}_error_table }};",
            "void initialize_" + table_name + "_error_table(void) {",
            "    if (!link.next) {",
            "        add_to_error_table(&link);",
            "    }",
            "}",
            "",
        ]
    )
    write_if_changed(out_dir / "ext2_err.c", "\n".join(c_lines))


def generate_types_header(types_in_path: Path, out_dir: Path):
    template = types_in_path.read_text(encoding="utf-8")
    replacements = {
        "@SIZEOF_SHORT@": "2",
        "@SIZEOF_INT@": "4",
        "@SIZEOF_LONG@": "4",
        "@SIZEOF_LONG_LONG@": "8",
        "@ASM_TYPES_HEADER@": "",
        "@PUBLIC_CONFIG_HEADER@": "",
    }
    out = template
    for key, val in replacements.items():
        out = out.replace(key, val)
    write_if_changed(out_dir / "ext2fs" / "ext2_types.h", out)


def generate_crc32c_table(out_dir: Path):
    """Reproduce lib/ext2fs/gen_crc32ctable.c output for CRC_LE_BITS=64, CRC_BE_BITS=64."""
    crcpoly_be = 0x04C11DB7
    crc32c_poly_le = 0x82F63B78
    rows = 8
    size = 256

    table_be = [[0] * size for _ in range(rows)]
    crc = 0x80000000
    i = 1
    while i < size:
        crc = ((crc << 1) & 0xFFFFFFFF) ^ (crcpoly_be if (crc & 0x80000000) else 0)
        for j in range(i):
            table_be[0][i + j] = crc ^ table_be[0][j]
        i <<= 1
    for i in range(size):
        crc = table_be[0][i]
        for j in range(1, rows):
            crc = table_be[0][(crc >> 24) & 0xFF] ^ ((crc << 8) & 0xFFFFFFFF)
            table_be[j][i] = crc

    table_le = [[0] * size for _ in range(rows)]
    crc = 1
    i = size >> 1
    while i:
        crc = (crc >> 1) ^ (crc32c_poly_le if (crc & 1) else 0)
        for j in range(0, size, 2 * i):
            table_le[0][i + j] = crc ^ table_le[0][j]
        i >>= 1
    for i in range(size):
        crc = table_le[0][i]
        for j in range(1, rows):
            crc = table_le[0][crc & 0xFF] ^ (crc >> 8)
            table_le[j][i] = crc

    def format_table(name: str, tbl, trans: str):
        lines = [f"static const uint32_t {name}[{rows}][{size}] = {{"]
        for r in range(rows):
            lines.append("{")
            for idx in range(size - 1):
                if idx % 4 == 0:
                    lines.append("")
                lines[-1] += f"{trans}(0x{tbl[r][idx]:08x}L), "
            lines[-1] += f"{trans}(0x{tbl[r][size - 1]:08x}L)}},"
        lines.append("};")
        return "\n".join(lines)

    out_lines = [
        "/* this file is generated - do not edit */",
        "",
        format_table("crc32table_be", table_be, "tobe"),
        format_table("crc32ctable_le", table_le, "tole"),
        "",
    ]
    write_if_changed(out_dir / "ext2fs" / "crc32c_table.h", "\n".join(out_lines))


def main():
    parser = argparse.ArgumentParser(description="Generate e2fsprogs headers for MSVC/CMake build")
    parser.add_argument("--et-file", required=True, type=Path)
    parser.add_argument("--types-file", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--version-file", required=False, type=Path, default=None)
    args = parser.parse_args()

    if not args.et_file.exists():
        print(f"ERROR: missing {args.et_file}", file=sys.stderr)
        sys.exit(1)
    if not args.types_file.exists():
        print(f"ERROR: missing {args.types_file}", file=sys.stderr)
        sys.exit(1)

    version_str = read_e2fsprogs_version(args.version_file, args.et_file)
    generate_error_table(args.et_file, args.out_dir, version_str)
    generate_types_header(args.types_file, args.out_dir)
    generate_crc32c_table(args.out_dir)
    print(f"Generated e2fsprogs {version_str} headers into {args.out_dir}")


if __name__ == "__main__":
    main()
