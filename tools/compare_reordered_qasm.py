#!/usr/bin/env python3
"""Compare reordered QASM by scheduled quantum operations.

Byte equality is handled by the calling shell check.  This comparator accepts
formatting/header/register-name differences, but requires the same operation
with the same parameters and flattened qubit operands in the same slice.
Measurements and barriers are intentionally ignored.
"""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
import re
import sys


SLICE_RE = re.compile(r"^\s*//\s*Slice\s+(\d+)\s*:")
QREG_RE = re.compile(
    r"^\s*qreg\s+([A-Za-z_][A-Za-z0-9_]*)\s*\[\s*(\d+)\s*\]\s*;"
)
QUBIT_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\[\s*(\d+)\s*\]")
IGNORED_OPERATIONS = {"measure", "barrier"}


@dataclass(frozen=True, order=True)
class ScheduledOperation:
    slice_index: int
    operation: str
    qubits: tuple[int, ...]


def qreg_offsets(text: str) -> dict[str, tuple[int, int]]:
    offsets: dict[str, tuple[int, int]] = {}
    offset = 0
    for line in text.splitlines():
        match = QREG_RE.match(line)
        if match is None:
            continue
        name = match.group(1)
        size = int(match.group(2))
        offsets[name] = (offset, size)
        offset += size
    return offsets


def operation_name_and_base(statement: str) -> tuple[str, str]:
    conditional = re.match(
        r"if\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*==\s*(\d+)\s*\)\s*"
        r"([^\s]+)",
        statement,
    )
    if conditional is not None:
        register, value, gate = conditional.groups()
        return f"if({register}=={value}){gate}", gate.split("(", 1)[0]
    first_space = re.search(r"\s", statement)
    token = statement if first_space is None else statement[: first_space.start()]
    name = re.sub(r"\s+", "", token)
    return name, name.split("(", 1)[0]


def scheduled_operations(path: Path) -> Counter[ScheduledOperation]:
    text = path.read_text(encoding="utf-8")
    registers = qreg_offsets(text)
    operations: Counter[ScheduledOperation] = Counter()
    current_slice: int | None = None

    for raw_line in text.splitlines():
        slice_match = SLICE_RE.match(raw_line)
        if slice_match is not None:
            current_slice = int(slice_match.group(1))
            continue
        if raw_line.lstrip().startswith("//") or current_slice is None:
            continue

        source = raw_line.split("//", 1)[0]
        for raw_statement in source.split(";"):
            statement = raw_statement.strip()
            if not statement:
                continue
            name, base_name = operation_name_and_base(statement)
            if base_name in IGNORED_OPERATIONS:
                continue

            qubits: list[int] = []
            for register_name, index_text in QUBIT_RE.findall(statement):
                register = registers.get(register_name)
                if register is None:
                    raise ValueError(
                        f"{path}: scheduled operation references unknown qreg "
                        f"{register_name}[{index_text}]"
                    )
                offset, size = register
                index = int(index_text)
                if index >= size:
                    raise ValueError(
                        f"{path}: qubit {register_name}[{index}] exceeds qreg size {size}"
                    )
                qubits.append(offset + index)

            operations[ScheduledOperation(current_slice, name, tuple(qubits))] += 1
    return operations


def format_count(operation: ScheduledOperation, count: int) -> str:
    qubits = ",".join(f"q[{qubit}]" for qubit in operation.qubits)
    suffix = f" x{count}" if count != 1 else ""
    return f"slice {operation.slice_index}: {operation.operation} {qubits}{suffix}".rstrip()


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Require equal (slice, operation, flattened-qubits) multisets while "
            "ignoring measure and barrier statements."
        )
    )
    parser.add_argument("expected", type=Path)
    parser.add_argument("actual", type=Path)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    expected = scheduled_operations(args.expected)
    actual = scheduled_operations(args.actual)
    if expected == actual:
        return 0

    if not args.quiet:
        for operation, count in sorted((expected - actual).items()):
            print(f"missing: {format_count(operation, count)}")
        for operation, count in sorted((actual - expected).items()):
            print(f"extra:   {format_count(operation, count)}")
    return 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(2)
