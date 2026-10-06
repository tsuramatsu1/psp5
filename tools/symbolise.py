#!/usr/bin/env python3
"""Turn a console crash report into function names and lines.

    python3 tools/symbolise.py < crash.txt
    python3 tools/symbolise.py --elf build/ps5/link/llvm-pie.elf 0x430b34

Paste the klog block that starts at "A user thread receives a fatal signal", or
pass addresses on the command line. Reads the load base from the report's own
`xotext:` line when it is there, so the subtraction is never guessed.

Three things this exists to stop going wrong:

  * The title is linked at 0 and loaded at the xotext base (0x400000 for an
    eboot.bin). An address symbolised without subtracting it names a confident,
    wrong function.
  * It must be the ELF of the build that crashed - build/ps5/link/llvm-pie.elf,
    before conversion. A map from another link is just as confidently wrong.
  * A call through NULL pushes no frame, so a backtrace naming a function that
    plainly cannot fault is pointing at its caller. Ask what that function calls:
    that is the unfilled-import signature, and `rip: 0` with the fault address
    equal to rip is the same thing seen from the other side.
"""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys

DEFAULT_ELF = "build/ps5/link/llvm-pie.elf"
DEFAULT_BASE = 0x400000

HEX = re.compile(r"\b(?:0x)?([0-9a-fA-F]{6,16})\b")
XOTEXT = re.compile(r"xotext:\s*([0-9a-fA-F]+)\s*:", re.IGNORECASE)
RIP = re.compile(r"^\s*#?\s*rip:\s*(?:0x)?([0-9a-fA-F]+)", re.IGNORECASE | re.MULTILINE)
REASON = re.compile(r"^\s*#?\s*(reason|fault address|signal):\s*(.+)$",
                    re.IGNORECASE | re.MULTILINE)


def find_addr2line(sdk):
    for candidate in (
        sdk and pathlib.Path(sdk) / "bin" / "llvm-addr2line",
        shutil.which("llvm-addr2line"),
        shutil.which("addr2line"),
    ):
        if candidate and pathlib.Path(candidate).exists():
            return str(candidate)
    sys.exit("error: no llvm-addr2line; set PS5_PAYLOAD_SDK or put it on PATH")


def symbolise(tool, elf, addresses, base):
    if not addresses:
        return
    offsets = [a - base for a in addresses]
    if any(o < 0 for o in offsets):
        print(f"warning: an address is below the load base {base:#x}; "
              f"is the base right?", file=sys.stderr)
    result = subprocess.run(
        [tool, "-f", "-C", "-i", "-e", str(elf)] + [f"{o:#x}" for o in offsets],
        capture_output=True, text=True)
    lines = [l for l in result.stdout.splitlines() if l.strip()]

    # addr2line prints name then location, and more of each pair when a call was
    # inlined; pairing them back to one address is best-effort, so the raw address
    # is always printed beside the answer.
    pairs = list(zip(lines[0::2], lines[1::2]))
    for i, address in enumerate(addresses):
        name, where = pairs[i] if i < len(pairs) else ("??", "??")
        marker = "rip " if i == 0 else f"  #{i - 1:<2d}"
        print(f"{marker} {address:#018x}  ->  {address - base:#x}")
        print(f"      {name}")
        print(f"      {where}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("addresses", nargs="*", help="addresses, if not piping a report")
    parser.add_argument("--elf", default=DEFAULT_ELF, type=pathlib.Path)
    parser.add_argument("--base", help="load base; read from the report when absent")
    parser.add_argument("--sdk", default=None, help="the payload SDK, for llvm-addr2line")
    args = parser.parse_args()

    import os
    tool = find_addr2line(args.sdk or os.environ.get("PS5_PAYLOAD_SDK"))
    if not args.elf.exists():
        sys.exit(f"error: no {args.elf} - symbolise against the build that crashed")

    if args.addresses:
        base = int(args.base, 0) if args.base else DEFAULT_BASE
        print(f"elf {args.elf}, base {base:#x}")
        symbolise(tool, args.elf, [int(a, 0) for a in args.addresses], base)
        return

    report = sys.stdin.read()
    if not report.strip():
        sys.exit("error: nothing on stdin; paste the crash report or pass addresses")

    for match in REASON.finditer(report):
        print(f"{match.group(1)}: {match.group(2).strip()}")

    if args.base:
        base = int(args.base, 0)
    else:
        found = XOTEXT.search(report)
        base = int(found.group(1), 16) if found else DEFAULT_BASE
        print(f"load base: {base:#x}" + ("" if found else "  (assumed; no xotext line)"))
    print(f"elf: {args.elf}\n")

    addresses = []
    rip = RIP.search(report)
    if rip:
        addresses.append(int(rip.group(1), 16))

    # Everything after "backtrace:" that looks like an address, in order.
    tail = report.split("backtrace:", 1)[1] if "backtrace:" in report else ""
    tail = tail.split("dynamic libraries:", 1)[0]
    for match in HEX.finditer(tail):
        value = int(match.group(1), 16)
        if value > 0x1000:
            addresses.append(value)

    if not addresses:
        sys.exit("error: no rip and no backtrace addresses found in that text")
    symbolise(tool, args.elf, addresses, base)


if __name__ == "__main__":
    main()
