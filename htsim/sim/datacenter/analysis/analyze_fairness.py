#!/usr/bin/env python3
"""
Fairness Analysis Script for NSCC vs TCP Cubic Experiments

Computes:
- Jain's Fairness Index: J = (sum(xi))^2 / (n * sum(xi^2))
- Coefficient of Variation: CV = std / mean
- Min/Max Ratio: min(throughput) / max(throughput)
- FCT Distribution: P50, P95, P99

Usage: python3 analyze_fairness.py <results_dir>
"""

import sys
import os
import re
import glob
from collections import defaultdict
import csv

def parse_nscc_log(log_path):
    """Parse NSCC (UEC) log file to extract flow completion times and throughputs."""
    flows = []

    with open(log_path, 'r') as f:
        for line in f:
            # NSCC format: "Flow Uec_X_Y flowId Z uecSrc W finished at T total messages M total packets P ... total bytes B ..."
            match = re.search(r'Flow\s+\S+\s+flowId\s+(\d+)\s+\S+\s+\d+\s+finished\s+at\s+([\d.]+)\s+.*total\s+bytes\s+(\d+)', line)
            if match:
                flow_id = int(match.group(1))
                fct_us = float(match.group(2))  # microseconds
                total_bytes = int(match.group(3))
                fct_ms = fct_us / 1000.0  # convert to milliseconds
                throughput_mbps = (total_bytes * 8) / (fct_us) if fct_us > 0 else 0  # Mbps
                flows.append({
                    'flow_id': flow_id,
                    'size': total_bytes,
                    'fct_ms': fct_ms,
                    'throughput_mbps': throughput_mbps
                })

    return flows

def parse_cubic_log(log_path):
    """Parse TCP Cubic log file to extract flow completion times and throughputs."""
    flows = []

    # First, try to find flow size from "Setting flow size" lines
    flow_size = 2000000  # default

    with open(log_path, 'r') as f:
        content = f.read()

        # Find flow size
        size_match = re.search(r'Setting flow size to (\d+)', content)
        if size_match:
            flow_size = int(size_match.group(1))

        # TCP Cubic format: "Flow tcpsrc finished at T" (T in ms)
        flow_id = 1
        for match in re.finditer(r'Flow\s+\S+\s+finished\s+at\s+([\d.]+)', content):
            fct_ms = float(match.group(1))
            throughput_mbps = (flow_size * 8) / (fct_ms * 1000) if fct_ms > 0 else 0  # Mbps
            flows.append({
                'flow_id': flow_id,
                'size': flow_size,
                'fct_ms': fct_ms,
                'throughput_mbps': throughput_mbps
            })
            flow_id += 1

    return flows

def calculate_jains_fairness(values):
    """Calculate Jain's Fairness Index."""
    if not values or len(values) == 0:
        return 0
    n = len(values)
    sum_x = sum(values)
    sum_x_sq = sum(x*x for x in values)
    if sum_x_sq == 0:
        return 1.0
    return (sum_x * sum_x) / (n * sum_x_sq)

def calculate_cv(values):
    """Calculate Coefficient of Variation."""
    if not values or len(values) == 0:
        return 0
    mean = sum(values) / len(values)
    if mean == 0:
        return 0
    variance = sum((x - mean)**2 for x in values) / len(values)
    std = variance ** 0.5
    return std / mean

def calculate_min_max_ratio(values):
    """Calculate min/max ratio."""
    if not values or len(values) == 0:
        return 0
    min_val = min(values)
    max_val = max(values)
    if max_val == 0:
        return 1.0
    return min_val / max_val

def percentile(values, p):
    """Calculate percentile."""
    if not values:
        return 0
    sorted_vals = sorted(values)
    k = (len(sorted_vals) - 1) * p / 100
    f = int(k)
    c = f + 1 if f + 1 < len(sorted_vals) else f
    return sorted_vals[f] + (sorted_vals[c] - sorted_vals[f]) * (k - f)

def analyze_experiment(log_path, protocol):
    """Analyze a single experiment log file."""
    if protocol.lower() == 'nscc':
        flows = parse_nscc_log(log_path)
    else:
        flows = parse_cubic_log(log_path)

    if not flows:
        return None

    throughputs = [f['throughput_mbps'] for f in flows]
    fcts = [f['fct_ms'] for f in flows]

    return {
        'num_flows': len(flows),
        'jains_fairness': calculate_jains_fairness(throughputs),
        'cv': calculate_cv(throughputs),
        'min_max_ratio': calculate_min_max_ratio(throughputs),
        'mean_throughput_mbps': sum(throughputs) / len(throughputs) if throughputs else 0,
        'fct_p50_ms': percentile(fcts, 50),
        'fct_p95_ms': percentile(fcts, 95),
        'fct_p99_ms': percentile(fcts, 99),
        'fct_mean_ms': sum(fcts) / len(fcts) if fcts else 0,
        'fct_min_ms': min(fcts) if fcts else 0,
        'fct_max_ms': max(fcts) if fcts else 0,
    }

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 analyze_fairness.py <results_dir>")
        sys.exit(1)

    results_dir = sys.argv[1]

    if not os.path.isdir(results_dir):
        print(f"Error: {results_dir} is not a directory")
        sys.exit(1)

    # Find all log files
    log_files = glob.glob(os.path.join(results_dir, "*.log"))

    if not log_files:
        print(f"No log files found in {results_dir}")
        sys.exit(1)

    # Parse experiment names and group by topology/workload
    results = defaultdict(dict)

    for log_path in log_files:
        filename = os.path.basename(log_path)
        # Parse filename: protocol_topology_workload.log
        match = re.match(r'(nscc|cubic)_(.+)_(fairness_.+)\.log', filename)
        if match:
            protocol = match.group(1).upper()
            topology = match.group(2)
            workload = match.group(3)

            key = f"{topology}/{workload}"
            metrics = analyze_experiment(log_path, protocol)

            if metrics:
                results[key][protocol] = metrics
                print(f"Analyzed: {protocol} - {key} ({metrics['num_flows']} flows)")
            else:
                print(f"Warning: No flows found in {filename}")

    if not results:
        print("No valid results found")
        sys.exit(1)

    # Print summary table
    print("\n" + "=" * 100)
    print("FAIRNESS COMPARISON: NSCC vs TCP Cubic")
    print("=" * 100)

    for experiment, protocols in sorted(results.items()):
        print(f"\n{experiment}")
        print("-" * 80)

        headers = ["Metric", "NSCC", "TCP Cubic", "Difference", "Better"]
        rows = []

        nscc = protocols.get('NSCC', {})
        cubic = protocols.get('CUBIC', {})

        metrics = [
            ("Jain's Fairness Index", 'jains_fairness', '.4f', 'higher'),
            ("Coefficient of Variation", 'cv', '.4f', 'lower'),
            ("Min/Max Ratio", 'min_max_ratio', '.4f', 'higher'),
            ("Mean Throughput (Mbps)", 'mean_throughput_mbps', '.2f', 'higher'),
            ("FCT P50 (ms)", 'fct_p50_ms', '.3f', 'lower'),
            ("FCT P95 (ms)", 'fct_p95_ms', '.3f', 'lower'),
            ("FCT P99 (ms)", 'fct_p99_ms', '.3f', 'lower'),
            ("FCT Mean (ms)", 'fct_mean_ms', '.3f', 'lower'),
            ("FCT Min (ms)", 'fct_min_ms', '.3f', 'lower'),
            ("FCT Max (ms)", 'fct_max_ms', '.3f', 'lower'),
            ("Number of Flows", 'num_flows', 'd', None),
        ]

        for name, key, fmt, better_dir in metrics:
            nscc_val = nscc.get(key, 0)
            cubic_val = cubic.get(key, 0)
            diff = nscc_val - cubic_val if nscc_val and cubic_val else 0

            # Determine which is better
            better = ""
            if better_dir and nscc_val and cubic_val:
                if better_dir == 'higher':
                    better = "NSCC" if nscc_val > cubic_val else "CUBIC"
                else:
                    better = "NSCC" if nscc_val < cubic_val else "CUBIC"

            if fmt == 'd':
                row = [name, f"{nscc_val}", f"{cubic_val}", f"{diff:+d}", better]
            else:
                row = [name, f"{nscc_val:{fmt}}", f"{cubic_val:{fmt}}", f"{diff:+{fmt}}", better]
            rows.append(row)

        # Print table
        col_widths = [max(len(str(row[i])) for row in [headers] + rows) for i in range(5)]
        fmt_str = "  ".join(f"{{:<{w}}}" for w in col_widths)

        print(fmt_str.format(*headers))
        print("  ".join("-" * w for w in col_widths))
        for row in rows:
            print(fmt_str.format(*row))

    # Save to CSV
    csv_path = os.path.join(results_dir, "fairness_metrics.csv")
    with open(csv_path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['Experiment', 'Protocol', 'Jains_Fairness', 'CV', 'Min_Max_Ratio',
                        'Mean_Throughput_Mbps', 'FCT_P50_ms', 'FCT_P95_ms', 'FCT_P99_ms',
                        'FCT_Mean_ms', 'FCT_Min_ms', 'FCT_Max_ms', 'Num_Flows'])

        for experiment, protocols in sorted(results.items()):
            for protocol, metrics in protocols.items():
                writer.writerow([
                    experiment, protocol,
                    metrics.get('jains_fairness', ''),
                    metrics.get('cv', ''),
                    metrics.get('min_max_ratio', ''),
                    metrics.get('mean_throughput_mbps', ''),
                    metrics.get('fct_p50_ms', ''),
                    metrics.get('fct_p95_ms', ''),
                    metrics.get('fct_p99_ms', ''),
                    metrics.get('fct_mean_ms', ''),
                    metrics.get('fct_min_ms', ''),
                    metrics.get('fct_max_ms', ''),
                    metrics.get('num_flows', ''),
                ])

    print(f"\nResults saved to: {csv_path}")

    print("\n" + "=" * 100)
    print("INTERPRETATION")
    print("=" * 100)
    print("""
Jain's Fairness Index:
  - Range: 0 to 1 (1 = perfect fairness)
  - Values above 0.9 indicate good fairness
  - Higher is better

Coefficient of Variation:
  - Lower is better (more consistent throughput)
  - CV < 0.1 indicates very consistent performance

Min/Max Ratio:
  - Range: 0 to 1 (1 = all flows equal)
  - Higher values indicate better worst-case fairness

FCT (Flow Completion Time):
  - Lower is better
  - P99 indicates tail latency performance
""")

if __name__ == "__main__":
    main()
