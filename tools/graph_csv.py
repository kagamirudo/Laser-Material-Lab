#!/usr/bin/env python3
"""
Graph laser ADC CSV data (timestamp_us, adc_value).

CSV format: header "timestamp_us,adc_value", then one row per sample.
Time is plotted in milliseconds (relative to first sample).
All data points are plotted, including multiple samples per millisecond.

Usage:
  python3 graph_csv.py data.csv
  python3 graph_csv.py data.csv -o plot.png
  python3 graph_csv.py data.csv --show
"""

import argparse
import csv
import sys

try:
    import matplotlib
    matplotlib.use("Agg")  # non-interactive backend by default
    import matplotlib.pyplot as plt
except ImportError:
    print("graph_csv.py requires matplotlib. Install with: pip install matplotlib", file=sys.stderr)
    sys.exit(1)

try:
    import numpy as np
except ImportError:
    np = None


def _median(xs):
    xs = sorted(xs)
    n = len(xs)
    if n == 0:
        return 0.0
    mid = n // 2
    if n % 2 == 1:
        return float(xs[mid])
    return 0.5 * (xs[mid - 1] + xs[mid])


def _mad(xs, med=None):
    if not xs:
        return 0.0
    if med is None:
        med = _median(xs)
    dev = [abs(x - med) for x in xs]
    return _median(dev)


def load_csv(path: str, max_rows: int | None = None):
    """Load CSV.

    Supports:
      - Raw sample CSV:  timestamp_us,adc_value
      - Chunk CSV:       chunk_start_ms,chunk_end_ms,count,mean,median,min,max

    Returns (times_ms, values).
    """
    times_ms: list[float] = []
    values: list[float] = []

    with open(path, newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        header = next(reader, None)

        if header:
            lower = [h.strip().lower() for h in header]
            # Case 1: chunked CSV (data_500ms.csv)
            if lower[0] == "chunk_start_ms":
                # Choose mean as the plotted value if present, else median, else min
                try:
                    idx_start = lower.index("chunk_start_ms")
                except ValueError:
                    idx_start = 0
                idx_end = lower.index("chunk_end_ms") if "chunk_end_ms" in lower else None
                value_idx = None
                for key in ("mean", "median", "min"):
                    if key in lower:
                        value_idx = lower.index(key)
                        break
                if value_idx is None:
                    value_idx = 0

                for row in reader:
                    if len(row) <= max(idx_start, value_idx):
                        continue
                    try:
                        start_ms = float(row[idx_start])
                        if idx_end is not None and len(row) > idx_end:
                            end_ms = float(row[idx_end])
                            t_ms = 0.5 * (start_ms + end_ms)
                        else:
                            t_ms = start_ms
                        v = float(row[value_idx])
                    except ValueError:
                        continue
                    times_ms.append(t_ms)
                    values.append(v)
                    if max_rows is not None and len(values) >= max_rows:
                        break
                return times_ms, values

        # Case 2: raw timestamp_us,adc_value CSV
        times_us: list[int] = []
        # If header looks like timestamp header, skip it; else treat as data
        if header and (
            header[0].strip().lower() == "timestamp_us"
            or "timestamp" in header[0].lower()
        ):
            pass
        elif header:
            try:
                t = int(header[0])
                v = int(header[1])
                times_us.append(t)
                values.append(float(v))
            except (ValueError, IndexError):
                pass

        for row in reader:
            if len(row) < 2:
                continue
            try:
                t = int(row[0])
                v = int(row[1])
            except ValueError:
                continue
            times_us.append(t)
            values.append(float(v))
            if max_rows is not None and len(values) >= max_rows:
                break

    if not times_us:
        return [], []
    t0 = times_us[0]
    times_ms = [(t - t0) / 1000.0 for t in times_us]
    return times_ms, values


def bin_by_interval_ms(times_ms, values, interval_ms: int = 1, agg: str = "mean"):
    """Aggregate samples into fixed-width time bins.

    interval_ms=1   -> 1ms bins
    interval_ms=500 -> 500ms bins ("chunk" the data)

    Returns (bin_centers_ms, binned_values, counts_per_bin).
    """
    if not times_ms:
        return [], [], []

    if interval_ms <= 0:
        raise ValueError("interval_ms must be > 0")

    bins = {}  # bin_index -> (sum, count, list_values)
    for t, v in zip(times_ms, values):
        k = int(t // interval_ms)
        if k not in bins:
            if agg == "median":
                bins[k] = (0.0, 0, [v])
            else:
                bins[k] = (float(v), 1, None)
        else:
            s, c, lst = bins[k]
            if agg == "median":
                lst.append(v)
                bins[k] = (s, c + 1, lst)
            else:
                bins[k] = (s + float(v), c + 1, None)

    keys = sorted(bins.keys())
    out_t = []
    out_v = []
    out_c = []
    for k in keys:
        s, c, lst = bins[k]
        out_t.append((k + 0.5) * float(interval_ms))  # bin center (ms)
        out_c.append(int(c))
        if agg == "median":
            out_v.append(_median(lst))
        else:
            out_v.append(s / float(c))
    return out_t, out_v, out_c


def chunk_stats(times_ms, values, interval_ms: int):
    """Return per-chunk stats for a fixed interval."""
    if not times_ms or interval_ms <= 0:
        return []

    bins = {}
    for t, v in zip(times_ms, values):
        k = int(t // interval_ms)
        bins.setdefault(k, []).append(v)

    rows = []
    for k in sorted(bins.keys()):
        vs = bins[k]
        c = len(vs)
        vmin = min(vs)
        vmax = max(vs)
        vmed = _median(vs)
        mean = (sum(vs) / float(c)) if c else 0.0
        rows.append(
            {
                "chunk_start_ms": k * interval_ms,
                "chunk_end_ms": (k + 1) * interval_ms,
                "count": c,
                "mean": mean,
                "median": vmed,
                "min": vmin,
                "max": vmax,
            }
        )
    return rows


def write_chunk_csv(path: str, rows):
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["chunk_start_ms", "chunk_end_ms", "count", "mean", "median", "min", "max"])
        for r in rows:
            w.writerow(
                [
                    r["chunk_start_ms"],
                    r["chunk_end_ms"],
                    r["count"],
                    f"{r['mean']:.6f}",
                    f"{r['median']:.6f}",
                    r["min"],
                    r["max"],
                ]
            )


def moving_average(values, window: int):
    if window <= 1 or len(values) < 2:
        return values
    if np is not None:
        v = np.asarray(values, dtype=np.float64)
        k = np.ones(int(window), dtype=np.float64) / float(window)
        y = np.convolve(v, k, mode="same")
        return y.tolist()

    # pure python
    half = window // 2
    out = []
    for i in range(len(values)):
        a = max(0, i - half)
        b = min(len(values), i + half + 1)
        out.append(sum(values[a:b]) / float(b - a))
    return out


def median_filter(values, window: int):
    if window <= 1 or len(values) < 2:
        return values
    # odd window is typical for median filters
    if window % 2 == 0:
        window += 1
    half = window // 2
    out = []
    for i in range(len(values)):
        a = max(0, i - half)
        b = min(len(values), i + half + 1)
        out.append(_median(values[a:b]))
    return out


def robust_outlier_mask(values, z_thresh: float = 6.0):
    """Return boolean mask where True means 'keep' (not an outlier).

    Uses median + MAD; robust for ugly data.
    """
    if not values:
        return []
    med = _median(values)
    mad = _mad(values, med=med)
    if mad <= 0:
        return [True] * len(values)
    # 1.4826 scales MAD to ~sigma for normal distributions
    scale = 1.4826 * mad
    keep = []
    for v in values:
        z = abs(v - med) / scale
        keep.append(z <= z_thresh)
    return keep


def summarize(times_ms, values, label: str, adc_bits: int | None = None):
    n = len(values)
    if n == 0:
        return {
            "label": label,
            "n": 0,
        }
    duration_ms = (times_ms[-1] - times_ms[0]) if len(times_ms) > 1 else 0.0
    vmin = min(values)
    vmax = max(values)
    vmed = _median(values)
    # robust noise estimate via MAD
    mad = _mad(values, med=vmed)
    sigma_robust = 1.4826 * mad
    # derivative "spikiness"
    dv = [values[i] - values[i - 1] for i in range(1, n)]
    dv_med = _median(dv) if dv else 0.0
    dv_mad = _mad(dv, med=dv_med) if dv else 0.0
    dv_sigma = 1.4826 * dv_mad
    s = {
        "label": label,
        "n": n,
        "duration_ms": duration_ms,
        "min": vmin,
        "max": vmax,
        "median": vmed,
        "sigma_robust": sigma_robust,
        "dv_sigma_robust": dv_sigma,
    }
    if adc_bits is not None:
        full_scale = (1 << adc_bits) - 1
        s["sat_low_pct"] = 100.0 * sum(1 for v in values if v <= 0) / n
        s["sat_high_pct"] = 100.0 * sum(1 for v in values if v >= full_scale) / n
    return s


def main():
    parser = argparse.ArgumentParser(
        description="Graph + analyze laser ADC CSV (timestamp_us, adc_value)"
    )
    parser.add_argument(
        "csv_file",
        nargs="?",
        default="data.csv",
        help="Input CSV path (default: data.csv)",
    )
    parser.add_argument(
        "-o", "--output",
        default=None,
        help="Save figure to this path (e.g. plot.png)",
    )
    parser.add_argument(
        "--show",
        action="store_true",
        help="Show interactive plot window",
    )
    parser.add_argument(
        "--dpi",
        type=int,
        default=100,
        help="Figure DPI for saved image (default: 100)",
    )
    parser.add_argument(
        "--max-rows",
        type=int,
        default=None,
        help="Limit rows loaded (for huge files / quick look). Default: load all.",
    )
    parser.add_argument(
        "--bin-ms",
        action="store_true",
        help="Aggregate into 1ms bins (reduces noise + makes long files plottable).",
    )
    parser.add_argument(
        "--bin-interval-ms",
        type=int,
        default=1,
        help="Bin width in milliseconds when using --bin-ms (default: 1). Use 500 for 500ms chunks.",
    )
    parser.add_argument(
        "--bin-agg",
        choices=("mean", "median"),
        default="mean",
        help="Aggregation for 1ms bins (default: mean).",
    )
    parser.add_argument(
        "--export-chunks",
        default=None,
        help="Write per-bin stats to a CSV (use with --bin-ms --bin-interval-ms 500).",
    )
    parser.add_argument(
        "--median-filter",
        type=int,
        default=0,
        help="Apply median filter with this window (in samples for raw, or ms for binned).",
    )
    parser.add_argument(
        "--ma",
        type=int,
        default=0,
        help="Apply moving average with this window (in samples for raw, or ms for binned).",
    )
    parser.add_argument(
        "--despike",
        action="store_true",
        help="Drop extreme outliers using robust MAD thresholding (good for terrible data).",
    )
    parser.add_argument(
        "--despike-z",
        type=float,
        default=6.0,
        help="Outlier threshold in robust-z (default: 6.0).",
    )
    parser.add_argument(
        "--adc-bits",
        type=int,
        default=None,
        help="If set (e.g. 12), report saturation %% near 0 and full-scale.",
    )
    args = parser.parse_args()

    times_ms, values = load_csv(args.csv_file, max_rows=args.max_rows)
    if not times_ms:
        print(f"No data loaded from {args.csv_file}", file=sys.stderr)
        sys.exit(1)

    raw_times_ms = times_ms
    raw_values = values

    raw_summary = summarize(raw_times_ms, raw_values, label="raw", adc_bits=args.adc_bits)

    proc_times_ms = raw_times_ms
    proc_values = raw_values
    counts_per_bin = None

    if args.bin_ms:
        proc_times_ms, proc_values, counts_per_bin = bin_by_interval_ms(
            raw_times_ms, raw_values, interval_ms=args.bin_interval_ms, agg=args.bin_agg
        )

    if args.export_chunks is not None:
        rows = chunk_stats(raw_times_ms, raw_values, interval_ms=args.bin_interval_ms)
        write_chunk_csv(args.export_chunks, rows)
        print(
            f"Wrote chunked stats CSV: {args.export_chunks} "
            f"(interval={args.bin_interval_ms}ms, chunks={len(rows):,})"
        )

    if args.despike:
        keep = robust_outlier_mask(proc_values, z_thresh=args.despike_z)
        proc_times_ms = [t for t, k in zip(proc_times_ms, keep) if k]
        proc_values = [v for v, k in zip(proc_values, keep) if k]
        if counts_per_bin is not None:
            counts_per_bin = [c for c, k in zip(counts_per_bin, keep) if k]

    # Denoise filters (order: median then moving average)
    if args.median_filter and args.median_filter > 1:
        proc_values = median_filter(proc_values, args.median_filter)
    if args.ma and args.ma > 1:
        proc_values = moving_average(proc_values, args.ma)

    proc_summary = summarize(proc_times_ms, proc_values, label="processed", adc_bits=args.adc_bits)

    def _print_summary(s):
        if s.get("n", 0) == 0:
            print(f"{s['label']}: no data")
            return
        dur_ms = s["duration_ms"]
        dur_s = dur_ms / 1000.0
        print(
            f"{s['label']}: n={s['n']:,}  duration={dur_ms:.2f} ms ({dur_s:.2f} s)  "
            f"min={s['min']}  median={s['median']:.2f}  max={s['max']}  "
            f"robust_sigma≈{s['sigma_robust']:.2f}  dv_sigma≈{s['dv_sigma_robust']:.2f}"
        )
        if "sat_low_pct" in s:
            print(f"  saturation: low={s['sat_low_pct']:.3f}%  high={s['sat_high_pct']:.3f}%")

    _print_summary(raw_summary)
    if proc_values is not raw_values or args.bin_ms or args.despike or args.median_filter or args.ma:
        _print_summary(proc_summary)

    # Plot raw vs processed (processed can be binned/filtered)
    fig, ax = plt.subplots(figsize=(12, 6))
    ax.plot(raw_times_ms, raw_values, linewidth=0.4, color="steelblue", alpha=0.25, label="raw", marker=".", markersize=0.8)
    if proc_times_ms != raw_times_ms or proc_values != raw_values:
        ax.plot(proc_times_ms, proc_values, linewidth=1.0, color="orange", alpha=0.95, label="processed")
    ax.set_xlabel("Time (ms)")
    ax.set_ylabel("ADC value")

    title_bits = []
    title_bits.append(f"{raw_summary['n']:,} samples")
    if args.bin_ms:
        title_bits.append(f"{args.bin_interval_ms}ms bins ({args.bin_agg})")
    if args.despike:
        title_bits.append(f"despike z≤{args.despike_z:g}")
    if args.median_filter and args.median_filter > 1:
        title_bits.append(f"med{args.median_filter}")
    if args.ma and args.ma > 1:
        title_bits.append(f"ma{args.ma}")
    ax.set_title("Laser ADC — " + ", ".join(title_bits))
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper right")

    # Side plot: distribution (histogram) for quick quality check
    ax2 = fig.add_axes([0.72, 0.16, 0.25, 0.25])
    try:
        if np is not None:
            ax2.hist(np.asarray(proc_values, dtype=np.float64), bins=80, color="orange", alpha=0.85)
        else:
            ax2.hist([float(v) for v in proc_values], bins=80, color="orange", alpha=0.85)
        ax2.set_title("Value histogram", fontsize=9)
        ax2.tick_params(axis="both", labelsize=8)
        ax2.grid(True, alpha=0.2)
    except Exception:
        pass

    # Overlay key stats on the plot
    stats_lines = [
        f"RAW n={raw_summary['n']:,}",
        f"RAW σ≈{raw_summary['sigma_robust']:.2f} (robust)",
        f"RAW dσ≈{raw_summary['dv_sigma_robust']:.2f}",
    ]
    if proc_summary.get("n", 0) > 0:
        stats_lines += [
            f"PROC n={proc_summary['n']:,}",
            f"PROC σ≈{proc_summary['sigma_robust']:.2f} (robust)",
            f"PROC dσ≈{proc_summary['dv_sigma_robust']:.2f}",
        ]
    ax.text(
        0.02,
        0.98,
        "\n".join(stats_lines),
        transform=ax.transAxes,
        fontsize=10,
        verticalalignment="top",
        horizontalalignment="left",
        bbox=dict(boxstyle="round", facecolor="wheat", alpha=0.8),
    )

    # fig.tight_layout()

    if args.output:
        fig.savefig(args.output, dpi=args.dpi)
        print(f"Saved: {args.output}")
    if args.show:
        plt.show()
    if not args.output and not args.show:
        out = args.csv_file.rsplit(".", 1)[0] + "_plot.png"
        fig.savefig(out, dpi=args.dpi)
        print(f"Saved: {out}")


if __name__ == "__main__":
    main()
