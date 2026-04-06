#!/usr/bin/env python3
"""Symbolize TrinityCore Linux crash log frames using addr2line.

Example:
  python3 symbolize_crashlog.py \
      --binary /path/to/worldserver \
      --log /path/to/Server.log
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

FRAME_RE = re.compile(r"^\s*\[(\d+)\]\s+([^\s(]+)\(\+0x([0-9a-fA-F]+)\)")


@dataclass(frozen=True)
class Frame:
    index: int
    module: str
    offset_hex: str


@dataclass
class StackTrace:
    frames: list[Frame]

    def signature(self) -> tuple[tuple[str, str], ...]:
        return tuple((frame.module, frame.offset_hex.lower()) for frame in self.frames)


def parse_stack_traces(log_text: str) -> list[StackTrace]:
    traces: list[StackTrace] = []
    current: list[Frame] = []

    for line in log_text.splitlines():
        if "Stack trace:" in line:
            if current:
                traces.append(StackTrace(current))
                current = []
            continue

        match = FRAME_RE.match(line)
        if match:
            current.append(
                Frame(index=int(match.group(1)), module=match.group(2), offset_hex=match.group(3))
            )
        elif current:
            traces.append(StackTrace(current))
            current = []

    if current:
        traces.append(StackTrace(current))

    return traces


def run_addr2line(binary: Path, offset_hex: str) -> str:
    command = [
        "addr2line",
        "-f",
        "-C",
        "-p",
        "-e",
        str(binary),
        f"0x{offset_hex}",
    ]

    result = subprocess.run(command, check=False, capture_output=True, text=True)
    if result.returncode != 0:
        return f"<addr2line failed: {result.stderr.strip() or result.stdout.strip()}>"

    output = result.stdout.strip()
    return output if output else "<no symbol information>"


def dedupe_traces(traces: Iterable[StackTrace]) -> list[StackTrace]:
    seen: set[tuple[tuple[str, str], ...]] = set()
    unique: list[StackTrace] = []

    for trace in traces:
        signature = trace.signature()
        if signature in seen:
            continue
        seen.add(signature)
        unique.append(trace)

    return unique


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path, help="Path to worldserver/authserver executable")
    parser.add_argument("--log", required=True, type=Path, help="Path to crash log file")
    parser.add_argument("--show-all", action="store_true", help="Do not deduplicate repeated stack traces")
    args = parser.parse_args()

    if not args.binary.is_file():
        print(f"error: binary file not found: {args.binary}", file=sys.stderr)
        return 2

    if not args.log.is_file():
        print(f"error: log file not found: {args.log}", file=sys.stderr)
        return 2

    traces = parse_stack_traces(args.log.read_text(encoding="utf-8", errors="replace"))
    if not traces:
        print("No stack traces found in log.")
        return 1

    rendered_traces = traces if args.show_all else dedupe_traces(traces)
    if not args.show_all and len(rendered_traces) < len(traces):
        print(f"Found {len(traces)} traces, {len(rendered_traces)} unique signatures.\n")

    for trace_index, trace in enumerate(rendered_traces, start=1):
        print(f"Trace #{trace_index} ({len(trace.frames)} frame(s))")
        for frame in trace.frames:
            symbol = run_addr2line(args.binary, frame.offset_hex)
            print(f"  [{frame.index}] {frame.module}(+0x{frame.offset_hex}) => {symbol}")
        print()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
