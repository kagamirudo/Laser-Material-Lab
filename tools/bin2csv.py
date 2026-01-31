#!/usr/bin/env python3
"""
Convert laser bench binary file to CSV.

Binary format (must match 002.c bench_record_t):
  - 12 bytes per record: uint64 timestamp_us (little-endian), int32 adc_value (little-endian)
  - No file header; raw sequence of records.

Usage:
  python3 bin2csv.py data.bin              # print CSV to stdout
  python3 bin2csv.py data.bin out.csv     # write CSV to out.csv
"""

import argparse
import struct
import sys

RECORD_FMT = "<Qi"  # uint64 LE, int32 LE
RECORD_SIZE = struct.calcsize(RECORD_FMT)


def main():
    parser = argparse.ArgumentParser(
        description="Convert laser bench .bin (timestamp_us, adc_value) to CSV"
    )
    parser.add_argument("input", help="Input .bin file path")
    parser.add_argument(
        "output",
        nargs="?",
        default=None,
        help="Output .csv path (default: stdout, or input name with .csv)",
    )
    args = parser.parse_args()

    out_path = args.output
    if out_path is None:
        if args.input.lower().endswith(".bin"):
            out_path = args.input[:-4] + ".csv"
        else:
            out_path = args.input + ".csv"
    write_to_stdout = out_path == "-"

    try:
        with open(args.input, "rb") as f:
            data = f.read()
    except OSError as e:
        print("Error: cannot read '{}': {}".format(args.input, e), file=sys.stderr)
        sys.exit(1)

    if len(data) % RECORD_SIZE != 0:
        print(
            "Warning: file size {} is not a multiple of record size {}; trailing bytes ignored".format(
                len(data), RECORD_SIZE
            ),
            file=sys.stderr,
        )
    num_records = len(data) // RECORD_SIZE

    try:
        out_file = sys.stdout if write_to_stdout else open(out_path, "w")
    except OSError as e:
        print("Error: cannot write '{}': {}".format(out_path, e), file=sys.stderr)
        sys.exit(1)

    try:
        out_file.write("timestamp_us,adc_value\n")
        for i in range(num_records):
            offset = i * RECORD_SIZE
            timestamp_us, adc_value = struct.unpack_from(RECORD_FMT, data, offset)
            out_file.write("{},{}\n".format(timestamp_us, adc_value))
    finally:
        if not write_to_stdout:
            out_file.close()

    if not write_to_stdout:
        print("Wrote {} records to {}".format(num_records, out_path), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
