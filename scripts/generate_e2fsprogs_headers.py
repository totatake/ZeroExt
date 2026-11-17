#!/usr/bin/env python3
"""
Generate ext2fs/ext2_err.h, ext2_err.c, and ext2fs/ext2_types.h from upstream
e2fsprogs .in templates without requiring autotools, awk, or compile_et.
"""

import argparse
import os
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


def parse_et_file(et_path: Path):
    text = et_path.read_text(encoding="utf-8")
    # Strip C-style or shell-style comments carefully
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
            # Join continuation lines until at least one complete quoted string is present
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


def generate_error_table(et_path: Path, out_dir: Path):
    table_name, entries = parse_et_file(et_path)
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
            f"/* Backward compatibility alias */",
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
            f"        add_to_error_table(&link);",
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
    }
    out = template
    for key, val in replacements.items():
        out = out.replace(key, val)
    write_if_changed(out_dir / "ext2fs" / "ext2_types.h", out)


def main():
    parser = argparse.ArgumentParser(description="Generate e2fsprogs headers for MSVC/CMake build")
    parser.add_argument("--et-file", required=True, type=Path)
    parser.add_argument("--types-file", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    args = parser.parse_args()

    if not args.et_file.exists():
        print(f"ERROR: missing {args.et_file}", file=sys.stderr)
        sys.exit(1)
    if not args.types_file.exists():
        print(f"ERROR: missing {args.types_file}", file=sys.stderr)
        sys.exit(1)

    generate_error_table(args.et_file, args.out_dir)
    generate_types_header(args.types_file, args.out_dir)
    print(f"Generated e2fsprogs headers into {args.out_dir}")


if __name__ == "__main__":
    main()
