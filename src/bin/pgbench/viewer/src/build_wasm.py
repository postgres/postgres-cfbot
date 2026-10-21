#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
src/build_wasm.py — Zero-dependency WebAssembly binary assembler for wasm/pgbench_chart.wasm.

Reproducibly constructs the 439-byte `wasm/pgbench_chart.wasm` module from structured
WebAssembly section and opcode definitions (matching `src/pgbench_chart.wat` and
`src/pgbench_chart.c`) without requiring wabt (`wat2wasm`) or LLVM (`clang`).
"""

import argparse
import base64
import hashlib
import os
import struct
import sys

I32 = 0x7F
F64 = 0x7C
VOID = 0x40

EXPECTED_SIZE = 439
EXPECTED_SHA256 = "9fb72bbd451e5ea177841d4aa799ab466221bfaa71a03d37cfade1ab45866833"


def uleb128(val: int) -> bytes:
    out = bytearray()
    while True:
        byte = val & 0x7F
        val >>= 7
        if val:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            break
    return bytes(out)


def f64_bytes(val: float) -> bytes:
    return b"\x44" + struct.pack("<d", val)


def section(sec_id: int, payload: bytes) -> bytes:
    return bytes([sec_id]) + uleb128(len(payload)) + payload


def encode_func(locals_spec: list, body_ops: bytes) -> bytes:
    loc = bytearray(uleb128(len(locals_spec)))
    for count, val_type in locals_spec:
        loc.extend(uleb128(count))
        loc.append(val_type)
    full = bytes(loc) + body_ops + b"\x0b"
    return uleb128(len(full)) + full


def build_wasm_bytes() -> bytes:
    # 1. Type Section:
    #    type 0: (param i32 i32) -> (result f64)
    #    type 1: (param i32 i32 f64 f64 f64 f64 i32) -> ()
    type_sec = section(
        1,
        b"\x02"
        b"\x60\x02\x7f\x7f\x01\x7c"
        b"\x60\x07\x7f\x7f\x7c\x7c\x7c\x7c\x7f\x00",
    )

    # 3. Function Section: 4 functions with types [0, 0, 0, 1]
    func_sec = section(3, b"\x04\x00\x00\x00\x01")

    # 5. Memory Section: 1 memory, minimum 2 pages (128 KiB)
    mem_sec = section(5, b"\x01\x00\x02")

    # 7. Export Section: memory, series_min, series_max, series_mean, project_1d
    exports = [
        ("memory", 0x02, 0),
        ("series_min", 0x00, 0),
        ("series_max", 0x00, 1),
        ("series_mean", 0x00, 2),
        ("project_1d", 0x00, 3),
    ]
    exp_payload = bytearray(uleb128(len(exports)))
    for name, kind, idx in exports:
        nb = name.encode("utf-8")
        exp_payload.extend(uleb128(len(nb)) + nb + bytes([kind]) + uleb128(idx))
    export_sec = section(7, bytes(exp_payload))

    # 10. Code Section:
    def stat_minmax_ops(reduce_opcode: int) -> bytes:
        return (
            b"\x20\x01\x45"                  # local.get 1; i32.eqz
            b"\x04\x7c"                      # if (result f64)
            + f64_bytes(0.0)
            + b"\x05"                        # else
            b"\x20\x00\x2b\x03\x00\x21\x03"  # local.get 0; f64.load align=8 offset=0; local.set 3
            b"\x41\x01\x21\x02"             # i32.const 1; local.set 2
            b"\x02\x40\x03\x40"             # block; loop
            b"\x20\x02\x20\x01\x4f\x0d\x01"  # local.get 2; local.get 1; i32.ge_u; br_if 1
            b"\x20\x03"                     # local.get 3
            b"\x20\x00\x20\x02\x41\x03\x74\x6a\x2b\x03\x00"  # f64.load(ptr + (i << 3))
            + bytes([reduce_opcode])
            + b"\x21\x03"                   # local.set 3
            b"\x20\x02\x41\x01\x6a\x21\x02"  # i = i + 1
            b"\x0c\x00\x0b\x0b"             # br 0; end; end
            b"\x20\x03"                     # local.get 3
            b"\x0b"                         # end if
        )

    fn_series_min = encode_func([(1, I32), (1, F64)], stat_minmax_ops(0xA4))
    fn_series_max = encode_func([(1, I32), (1, F64)], stat_minmax_ops(0xA5))

    fn_series_mean = encode_func(
        [(1, I32), (1, F64)],
        b"\x20\x01\x45"                      # local.get 1; i32.eqz
        b"\x04\x7c"                          # if (result f64)
        + f64_bytes(0.0)
        + b"\x05"                            # else
        + f64_bytes(0.0)
        + b"\x21\x03"                        # local.set 3
        b"\x41\x00\x21\x02"                 # i32.const 0; local.set 2
        b"\x02\x40\x03\x40"                 # block; loop
        b"\x20\x02\x20\x01\x4f\x0d\x01"      # local.get 2; local.get 1; i32.ge_u; br_if 1
        b"\x20\x03"                         # local.get 3
        b"\x20\x00\x20\x02\x41\x03\x74\x6a\x2b\x03\x00"  # f64.load(ptr + (i << 3))
        b"\xa0\x21\x03"                     # f64.add; local.set 3
        b"\x20\x02\x41\x01\x6a\x21\x02"      # i = i + 1
        b"\x0c\x00\x0b\x0b"                 # br 0; end; end
        b"\x20\x03\x20\x01\xb8\xa3"         # acc / f64.convert_i32_u(n)
        b"\x0b",                            # end if
    )

    fn_project_1d = encode_func(
        [(1, I32), (1, F64)],
        b"\x20\x03\x20\x02\xa1\x21\x08"     # span = max_v - min_v
        b"\x20\x08\x99"                     # f64.abs(span)
        + f64_bytes(1e-12)
        + b"\x63\x04\x40"                    # f64.lt; if
        + f64_bytes(1.0)
        + b"\x21\x08\x0b"                    # span = 1.0; end
        b"\x41\x00\x21\x07"                 # i = 0
        b"\x02\x40\x03\x40"                 # block; loop
        b"\x20\x07\x20\x01\x4f\x0d\x01"      # if i >= n: break
        b"\x20\x06\x20\x07\x41\x03\x74\x6a"  # out_ptr + (i << 3)
        b"\x20\x04"                         # out_start
        b"\x20\x00\x20\x07\x41\x03\x74\x6a\x2b\x03\x00"  # f64.load(in_ptr + (i << 3))
        b"\x20\x02\xa1\x20\x08\xa3\x20\x05\xa2\xa0"      # ((val - min_v) / span) * out_span + out_start
        b"\x39\x03\x00"                     # f64.store align=8 offset=0
        b"\x20\x07\x41\x01\x6a\x21\x07"      # i = i + 1
        b"\x0c\x00\x0b\x0b",                # br 0; end; end
    )

    code_payload = (
        b"\x04" + fn_series_min + fn_series_max + fn_series_mean + fn_project_1d
    )
    code_sec = section(10, code_payload)

    return b"\x00asm\x01\x00\x00\x00" + type_sec + func_sec + mem_sec + export_sec + code_sec


def default_wasm_path() -> str:
    viewer_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(viewer_dir, "wasm", "pgbench_chart.wasm")


def main():
    parser = argparse.ArgumentParser(
        description="Assemble or verify wasm/pgbench_chart.wasm from structured opcodes."
    )
    parser.add_argument(
        "--output",
        "-o",
        default=default_wasm_path(),
        help="Target path for pgbench_chart.wasm (default: ../wasm/pgbench_chart.wasm)",
    )
    parser.add_argument(
        "--verify",
        action="store_true",
        help="Verify assembled bytes and on-disk wasm/pgbench_chart.wasm against expected SHA-256",
    )
    parser.add_argument(
        "--print-b64",
        action="store_true",
        help="Print Base64-encoded WebAssembly binary to stdout",
    )
    args = parser.parse_args()

    wasm_bytes = build_wasm_bytes()
    digest = hashlib.sha256(wasm_bytes).hexdigest()

    if args.print_b64:
        print(base64.b64encode(wasm_bytes).decode("ascii"))
        return

    if args.verify:
        if len(wasm_bytes) != EXPECTED_SIZE or digest != EXPECTED_SHA256:
            sys.stderr.write(
                f"ERROR: Assembled WASM mismatch: size={len(wasm_bytes)} (expected {EXPECTED_SIZE}), "
                f"sha256={digest} (expected {EXPECTED_SHA256})\n"
            )
            sys.exit(1)
        if os.path.exists(args.output):
            with open(args.output, "rb") as f:
                disk_bytes = f.read()
            if disk_bytes != wasm_bytes:
                sys.stderr.write(f"ERROR: On-disk {args.output} differs from assembled WASM bytes\n")
                sys.exit(1)
        print(f"Verified {args.output} ({len(wasm_bytes)} bytes, sha256={digest})")
        return

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as f:
        f.write(wasm_bytes)
    print(f"Wrote {args.output} ({len(wasm_bytes)} bytes, sha256={digest})")


if __name__ == "__main__":
    main()
