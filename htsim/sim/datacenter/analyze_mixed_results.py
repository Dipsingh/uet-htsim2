#!/usr/bin/env python3
"""
Analyze results from the mixed NSCC + TCP Cubic co-existence experiment.

Reads CSV files from the results directory and produces summary tables.

Usage:
  python3 analyze_mixed_results.py --results-dir results/mixed_experiment
"""

import argparse
import csv
import os
import sys
from collections import defaultdict


def read_csv(filepath):
    """Read a per-flow CSV file and return list of dicts."""
    rows = []
    with open(filepath) as f:
        reader = csv.DictReader(f)
        for row in reader:
            row['flow_id'] = int(row['flow_id'])
            row['src'] = int(row['src'])
            row['dst'] = int(row['dst'])
            row['size_bytes'] = int(row['size_bytes'])
            row['start_us'] = float(row['start_us'])
            row['fct_us'] = float(row['fct_us'])
            row['throughput_gbps'] = float(row['throughput_gbps'])
            row['finished'] = int(row['finished'])
            row['bytes_received'] = int(row['bytes_received'])
            row['retransmits'] = int(row['retransmits'])
            rows.append(row)
    return rows


def percentile(sorted_vals, p):
    """Return the p-th percentile from a sorted list."""
    if not sorted_vals:
        return 0
    idx = int(len(sorted_vals) * p / 100.0)
    idx = min(idx, len(sorted_vals) - 1)
    return sorted_vals[idx]


def jains_fairness(values):
    """Compute Jain's fairness index."""
    if len(values) < 2:
        return 1.0
    s = sum(values)
    s2 = sum(v * v for v in values)
    n = len(values)
    if s2 == 0:
        return 1.0
    return (s * s) / (n * s2)


def analyze_file(filepath):
    """Analyze a single CSV file and return a summary dict."""
    rows = read_csv(filepath)
    if not rows:
        return None

    nscc_rows = [r for r in rows if r['protocol'] == 'NSCC']
    cubic_rows = [r for r in rows if r['protocol'] == 'CUBIC']

    result = {
        'total_flows': len(rows),
        'nscc_flows': len(nscc_rows),
        'cubic_flows': len(cubic_rows),
    }

    for label, subset in [('nscc', nscc_rows), ('cubic', cubic_rows)]:
        if not subset:
            result[f'{label}_mean_tput'] = None
            result[f'{label}_median_tput'] = None
            result[f'{label}_p99_tput'] = None
            result[f'{label}_total_bytes'] = 0
            result[f'{label}_finished'] = 0
            result[f'{label}_retransmits'] = 0
            continue

        tputs = sorted([r['throughput_gbps'] for r in subset if r['throughput_gbps'] > 0])
        total_bytes = sum(r['bytes_received'] for r in subset)
        finished = sum(1 for r in subset if r['finished'])
        retx = sum(r['retransmits'] for r in subset)

        result[f'{label}_mean_tput'] = sum(tputs) / len(tputs) if tputs else 0
        result[f'{label}_median_tput'] = percentile(tputs, 50)
        result[f'{label}_p99_tput'] = percentile(tputs, 99)
        result[f'{label}_total_bytes'] = total_bytes
        result[f'{label}_finished'] = finished
        result[f'{label}_retransmits'] = retx

    # Bandwidth share
    total_bytes = result['nscc_total_bytes'] + result['cubic_total_bytes']
    if total_bytes > 0:
        result['nscc_share'] = result['nscc_total_bytes'] * 100.0 / total_bytes
        result['cubic_share'] = result['cubic_total_bytes'] * 100.0 / total_bytes
    else:
        result['nscc_share'] = 0
        result['cubic_share'] = 0

    # Jain's FI
    all_tputs = [r['throughput_gbps'] for r in rows if r['throughput_gbps'] > 0]
    result['jains_fi'] = jains_fairness(all_tputs)

    return result


def ratio_from_filename(filename):
    """Extract ratio from filename like scen{X}_ratio{NNN}.csv"""
    base = os.path.basename(filename).replace('.csv', '')
    parts = base.split('_')
    for p in parts:
        if p.startswith('ratio'):
            val = p[5:]
            # "00" -> 0.0, "025" -> 0.25, "05" -> 0.5, "075" -> 0.75, "10" -> 1.0
            if len(val) == 1:
                return float(val) / 10.0 if int(val) <= 1 else float(val)
            elif len(val) == 2:
                return float(val) / 10.0
            elif len(val) == 3:
                return float(val) / 100.0
    return -1


def scenario_from_filename(filename):
    """Extract scenario letter from filename."""
    base = os.path.basename(filename)
    if base.startswith('scen'):
        return base[4]
    return '?'


def main():
    parser = argparse.ArgumentParser(description="Analyze mixed experiment results")
    parser.add_argument("--results-dir", required=True, help="Directory with CSV files")
    parser.add_argument("--output", default=None, help="Output summary file (also prints to stdout)")
    args = parser.parse_args()

    csv_files = sorted([
        os.path.join(args.results_dir, f)
        for f in os.listdir(args.results_dir)
        if f.endswith('.csv')
    ])

    if not csv_files:
        print(f"No CSV files found in {args.results_dir}")
        sys.exit(1)

    # Group by scenario
    scenarios = defaultdict(list)
    for f in csv_files:
        scen = scenario_from_filename(f)
        scenarios[scen].append(f)

    lines = []

    def out(s=""):
        lines.append(s)
        print(s)

    scenario_labels = {
        'A': 'Permutation, 2MB uniform',
        'B': 'Mixed sizes, 10KB-10MB log-uniform',
        'C': '16-to-1 incast + 64 background',
    }

    out("=" * 90)
    out("MIXED NSCC + TCP CUBIC CO-EXISTENCE EXPERIMENT RESULTS")
    out("=" * 90)

    for scen in sorted(scenarios.keys()):
        label = scenario_labels.get(scen, f"Scenario {scen}")
        out(f"\n{'─' * 90}")
        out(f"Scenario {scen}: {label}")
        out(f"{'─' * 90}")
        out(f"{'Ratio':>7} | {'NSCC Flows':>10} | {'Cubic Flows':>11} | "
            f"{'NSCC Tput':>10} | {'Cubic Tput':>11} | "
            f"{'NSCC %':>7} | {'Cubic %':>8} | {'Jain FI':>8}")
        out(f"{'':>7} | {'':>10} | {'':>11} | "
            f"{'(mean Gbps)':>10} | {'(mean Gbps)':>11} | "
            f"{'':>7} | {'':>8} | {'':>8}")
        out("-" * 90)

        for fpath in sorted(scenarios[scen], key=lambda x: ratio_from_filename(x)):
            ratio = ratio_from_filename(fpath)
            result = analyze_file(fpath)
            if result is None:
                continue

            nscc_tput = f"{result['nscc_mean_tput']:.3f}" if result['nscc_mean_tput'] is not None else "-"
            cubic_tput = f"{result['cubic_mean_tput']:.3f}" if result['cubic_mean_tput'] is not None else "-"

            out(f"{ratio:>7.2f} | "
                f"{result['nscc_finished']}/{result['nscc_flows']:>4} done | "
                f"{result['cubic_finished']}/{result['cubic_flows']:>4} done | "
                f"{nscc_tput:>10} | {cubic_tput:>11} | "
                f"{result['nscc_share']:>6.1f}% | {result['cubic_share']:>7.1f}% | "
                f"{result['jains_fi']:>8.4f}")

    out(f"\n{'=' * 90}")
    out("KEY:")
    out("  NSCC/Cubic Tput = mean per-flow throughput in Gbps")
    out("  NSCC/Cubic %    = share of total received bytes")
    out("  Jain FI         = Jain's Fairness Index (1.0 = perfectly fair)")
    out("=" * 90)

    if args.output:
        with open(args.output, 'w') as f:
            f.write('\n'.join(lines) + '\n')
        print(f"\nSummary written to {args.output}")


if __name__ == "__main__":
    main()
