#!/usr/bin/env python3
"""Rename tests/parser_cases/case*.pr into ok_/warn_/err_ prefixed files.

Rule priority:
1) //@expect-error ...   -> err_
2) //@expect-warning ... -> warn_
3) //@expect-no-parser-error -> ok_
4) fallback -> ok_

A migration report is written to tests/parser_cases/MIGRATION_REPORT.md.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re


CASE_RE = re.compile(r"^case\d+_.*\.pr$")


@dataclass
class RenameOp:
    old: Path
    new: Path
    reason: str


def classify_case(path: Path) -> tuple[str, str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        t = line.strip()
        if t.startswith("//@expect-error "):
            return "err", "directive //@expect-error"
        if t.startswith("//@expect-warning "):
            return "warn", "directive //@expect-warning"
        if t == "//@expect-no-parser-error":
            return "ok", "directive //@expect-no-parser-error"
    return "ok", "fallback (legacy case* => ok_)"


def choose_unique_target(base_dir: Path, stem: str, ext: str) -> Path:
    candidate = base_dir / f"{stem}{ext}"
    if not candidate.exists():
        return candidate

    v = 2
    while True:
        candidate = base_dir / f"{stem}_v{v}{ext}"
        if not candidate.exists():
            return candidate
        v += 1


def collect_ops(case_dir: Path) -> list[RenameOp]:
    ops: list[RenameOp] = []
    for p in sorted(case_dir.glob("case*.pr")):
        if not p.is_file():
            continue
        if not CASE_RE.match(p.name):
            continue

        cls, reason = classify_case(p)
        stem = p.stem
        target_stem = f"{cls}_{stem}"
        target = choose_unique_target(case_dir, target_stem, p.suffix)
        ops.append(RenameOp(old=p, new=target, reason=reason))
    return ops


def apply_ops(ops: list[RenameOp]) -> None:
    for op in ops:
        op.old.rename(op.new)


def write_report(case_dir: Path, ops: list[RenameOp]) -> Path:
    report = case_dir / "MIGRATION_REPORT.md"
    lines = [
        "# parser_cases case* prefix migration",
        "",
        "Renamed legacy `case*.pr` files to `ok_/warn_/err_` prefixed names.",
        "",
        "## Rules",
        "",
        "1. `//@expect-error` -> `err_`",
        "2. `//@expect-warning` -> `warn_`",
        "3. `//@expect-no-parser-error` -> `ok_`",
        "4. fallback -> `ok_`",
        "",
        "## Mapping",
        "",
    ]

    if not ops:
        lines.append("No `case*.pr` files were found.")
    else:
        for op in ops:
            lines.append(f"- `{op.old.name}` -> `{op.new.name}` ({op.reason})")

    report.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--case-dir",
        default="tests/parser_cases",
        help="Directory containing parser case files",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Preview rename operations without applying them",
    )
    args = parser.parse_args()

    case_dir = Path(args.case_dir)
    if not case_dir.exists() or not case_dir.is_dir():
        raise SystemExit(f"case dir missing: {case_dir}")

    ops = collect_ops(case_dir)

    if args.dry_run:
        for op in ops:
            print(f"DRY-RUN: {op.old.name} -> {op.new.name} ({op.reason})")
        print(f"total={len(ops)}")
        return 0

    apply_ops(ops)
    report = write_report(case_dir, ops)
    print(f"renamed={len(ops)}")
    print(f"report={report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
