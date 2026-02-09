#!/usr/bin/env python3
"""
Time-series plots for NSCC vs TCP Cubic fairness experiments.

Reads the unified CSV produced by PeriodicSampler in htsim_mixed and generates
a 4-panel figure: goodput, buffer occupancy, window evolution, and drops.

The CSV's first line is a metadata comment with ECN thresholds, BDP, and link speed,
e.g.: # ecn_kmin=37500 ecn_kmax=145500 bdp=100000 linkspeed_gbps=100

Usage:
    python3 plot_timeseries.py <timeseries_csv> [output_png]
"""

import sys
import re
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use('Agg')  # non-interactive backend for headless rendering
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter


def parse_metadata(csv_path):
    """Parse the '# key=value ...' metadata line at the top of the CSV."""
    meta = {}
    with open(csv_path, 'r') as f:
        first_line = f.readline().strip()
    if first_line.startswith('#'):
        for m in re.finditer(r'(\w+)=([\d.eE+\-]+)', first_line):
            meta[m.group(1)] = float(m.group(2))
    return meta


def infer_tcp_ecn_from_filename(path):
    """Best-effort fallback when CSV metadata doesn't include tcp_ecn."""
    lower = path.lower()
    if "tcpecnoff" in lower or "tcp_ecn0" in lower or "tcp_ecn_off" in lower:
        return 0
    if "tcpecnon" in lower or "tcp_ecn1" in lower or "tcp_ecn_on" in lower:
        return 1
    return None


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 plot_timeseries.py <timeseries_csv> [output_png]")
        sys.exit(1)

    csv_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else csv_path.replace('.csv', '.png')

    # Read metadata from comment line, then load data (skip comment)
    meta = parse_metadata(csv_path)
    df = pd.read_csv(csv_path, comment='#')
    t = df['time_us'].values

    # --- Extract parameters from metadata (with sensible defaults) ---
    Kmin = int(meta.get('ecn_kmin', 37500))
    Kmax = int(meta.get('ecn_kmax', 145500))
    bdp = int(meta.get('bdp', 100000))
    linkspeed_gbps = meta.get('linkspeed_gbps', 100.0)
    tcp_ecn = meta.get('tcp_ecn', None)
    if tcp_ecn is not None:
        tcp_ecn = int(tcp_ecn)
    else:
        tcp_ecn = infer_tcp_ecn_from_filename(csv_path)

    # --- Detect columns dynamically ---
    tcp_cols = [c for c in df.columns if c.startswith('tcp') and c.endswith('_cwnd')]
    nscc_cols = [c for c in df.columns if c.startswith('nscc') and c.endswith('_cwnd')]
    n_tcp = len(tcp_cols)
    n_nscc = len(nscc_cols)

    # --- Compute rolling goodput (Gbps) ---
    # Use wider smoothing for decision-grade fairness trends.
    window_us = 2000.0
    dt = t[1] - t[0] if len(t) > 1 else 1.0
    window_samples = max(1, int(window_us / dt))

    def rolling_goodput_gbps(bytes_col):
        """Convert cumulative bytes to rolling Gbps."""
        b = df[bytes_col].values.astype(np.float64)
        delta_bytes = np.diff(b, prepend=b[0])
        # Rolling sum over window
        kernel = np.ones(window_samples) / window_samples
        smoothed = np.convolve(delta_bytes, kernel, mode='same')
        # delta_bytes per sample interval -> bytes/us -> Gbps
        return smoothed / dt * 8.0 / 1e3

    tcp_ecn_label = "unknown"
    if tcp_ecn == 0:
        tcp_ecn_label = "off"
    elif tcp_ecn == 1:
        tcp_ecn_label = "on"
    fabric_ecn_label = "on" if Kmin > 0 else "off"

    title = (
        "NSCC vs TCP Cubic "
        f"(fabric_ecn={fabric_ecn_label}, tcp_ecn={tcp_ecn_label}, "
        f"Kmin={Kmin/1000:.0f}KB, Kmax={Kmax/1000:.0f}KB, BDP={bdp/1000:.0f}KB)"
    )

    fig, axes = plt.subplots(4, 1, figsize=(14, 11), sharex=True,
                             gridspec_kw={'height_ratios': [3, 2, 3, 1.5]})
    fig.suptitle(title, fontsize=13, fontweight='bold', y=0.98)

    colors_tcp = ['#1f77b4', '#aec7e8']   # blues
    colors_nscc = ['#ff7f0e', '#ffbb78']  # oranges

    # Detect major regime shift from queue transition (high queue -> near Kmin queue)
    q = df['queue_bytes'].values.astype(np.float64)
    transition_idx = None
    if len(q) > 2000:
        for i in range(1000, len(q) - 1000):
            if np.mean(q[i - 500:i]) > 0.95 * Kmax and np.mean(q[i:i + 500]) < 1.1 * Kmin:
                transition_idx = i
                break
    transition_t = t[transition_idx] if transition_idx is not None else None

    # ========== Panel 1: Goodput ==========
    ax = axes[0]
    tcp_gp_series = []
    nscc_gp_series = []
    for i in range(n_tcp):
        gp = rolling_goodput_gbps(f'tcp{i}_bytes_acked')
        tcp_gp_series.append(gp)
        ax.plot(t, gp, color=colors_tcp[i % len(colors_tcp)],
                linewidth=0.6, label=f'TCP Cubic {i}', alpha=0.85)
    for i in range(n_nscc):
        gp = rolling_goodput_gbps(f'nscc{i}_bytes')
        nscc_gp_series.append(gp)
        ax.plot(t, gp, color=colors_nscc[i % len(colors_nscc)],
                linewidth=0.6, label=f'NSCC {i}', alpha=0.85)
    if n_tcp > 0:
        ax.plot(t, np.sum(np.vstack(tcp_gp_series), axis=0), color=colors_tcp[0],
                linewidth=1.2, alpha=0.35, label='TCP total')
    if n_nscc > 0:
        ax.plot(t, np.sum(np.vstack(nscc_gp_series), axis=0), color=colors_nscc[0],
                linewidth=1.2, alpha=0.35, label='NSCC total')
    ax.axhline(y=linkspeed_gbps, color='gray', linestyle=':', linewidth=0.8, alpha=0.5,
               label=f'Link capacity ({linkspeed_gbps:.0f} Gbps)')
    if transition_t is not None:
        ax.axvline(transition_t, color='black', linestyle='--', linewidth=0.8, alpha=0.6,
                   label=f'Regime shift @ {transition_t/1000:.1f}k us')
    ax.set_ylabel('Goodput (Gbps)')
    ax.set_ylim(bottom=0, top=linkspeed_gbps * 1.1)
    ax.legend(loc='upper right', fontsize=8, framealpha=0.9)
    ax.grid(True, alpha=0.3)
    ax.set_title(f'Rolling goodput ({int(window_us)}us window)', fontsize=10, loc='left')

    # ========== Panel 2: Buffer Occupancy (low-priority queue, matches ECN) ==========
    ax = axes[1]
    ax.fill_between(t, 0, q, alpha=0.35, color='steelblue', label='Queue occupancy (low-pri)')
    ax.plot(t, q, color='steelblue', linewidth=0.3)
    # ECN threshold markers
    if Kmin > 0:
        ax.axhline(y=Kmin, color='green', linestyle='--', linewidth=1.2,
                   label=f'Kmin = {Kmin/1000:.1f} KB (ECN start)')
        ax.axhline(y=Kmax, color='red', linestyle='--', linewidth=1.2,
                   label=f'Kmax = {Kmax/1000:.1f} KB (100% ECN)')
        # Shade ECN regions
        ymax = max(q.max() * 1.1, Kmax * 1.1)
        ax.axhspan(0, Kmin, alpha=0.04, color='green')
        ax.axhspan(Kmin, Kmax, alpha=0.04, color='gold')
        ax.axhspan(Kmax, ymax, alpha=0.04, color='red')
    else:
        ymax = q.max() * 1.1 if q.max() > 0 else 1
    if transition_t is not None:
        ax.axvline(transition_t, color='black', linestyle='--', linewidth=0.8, alpha=0.6)
    ax.set_ylabel('Queue (bytes)')
    ax.set_ylim(bottom=0, top=ymax)
    ax.legend(loc='upper right', fontsize=8, framealpha=0.9)
    ax.grid(True, alpha=0.3)

    # ========== Panel 3: Window Evolution (BDP-normalized, log-scale) ==========
    ax = axes[2]
    bdp_safe = max(float(bdp), 1.0)
    for i in range(n_tcp):
        cwnd_ratio = df[f'tcp{i}_cwnd'].values.astype(np.float64) / bdp_safe
        ax.plot(t, cwnd_ratio, color=colors_tcp[i % len(colors_tcp)],
                linewidth=0.7, label=f'TCP Cubic {i} cwnd/BDP', alpha=0.9)
    for i in range(n_nscc):
        cwnd_ratio = df[f'nscc{i}_cwnd'].values.astype(np.float64) / bdp_safe
        ax.plot(t, cwnd_ratio, color=colors_nscc[i % len(colors_nscc)],
                linewidth=0.7, label=f'NSCC {i} cwnd/BDP', alpha=0.9)
    ax.axhline(y=1.0, color='gray', linestyle=':', linewidth=0.8, alpha=0.6, label='1x BDP')
    if transition_t is not None:
        ax.axvline(transition_t, color='black', linestyle='--', linewidth=0.8, alpha=0.6)
    ax.set_ylabel('cwnd / BDP')
    ax.set_yscale('log')
    ax.set_ylim(bottom=1e-2, top=max(10.0, ax.get_ylim()[1]))
    ax.legend(loc='upper left', fontsize=8, framealpha=0.9)
    ax.grid(True, alpha=0.3)

    # ========== Panel 4: Share + Drops ==========
    ax = axes[3]
    if n_tcp > 0 and n_nscc > 0:
        tcp_total_gp = np.sum(np.vstack(tcp_gp_series), axis=0)
        nscc_total_gp = np.sum(np.vstack(nscc_gp_series), axis=0)
        total_gp = tcp_total_gp + nscc_total_gp
        nscc_share = np.full_like(total_gp, np.nan, dtype=np.float64)
        cubic_share = np.full_like(total_gp, np.nan, dtype=np.float64)
        valid = total_gp > 1e-9
        np.divide(100.0 * nscc_total_gp, total_gp, out=nscc_share, where=valid)
        np.divide(100.0 * tcp_total_gp, total_gp, out=cubic_share, where=valid)

        # Cumulative share (less noisy than instantaneous share).
        tcp_cum = np.zeros_like(t, dtype=np.float64)
        nscc_cum = np.zeros_like(t, dtype=np.float64)
        for i in range(n_tcp):
            tcp_cum += df[f'tcp{i}_bytes_acked'].values.astype(np.float64)
        for i in range(n_nscc):
            nscc_cum += df[f'nscc{i}_bytes'].values.astype(np.float64)
        cum_total = tcp_cum + nscc_cum
        cum_nscc_share = np.full_like(cum_total, np.nan, dtype=np.float64)
        valid_cum = cum_total > 1e-9
        np.divide(100.0 * nscc_cum, cum_total, out=cum_nscc_share, where=valid_cum)

        ax.plot(t, nscc_share, color=colors_nscc[0], linewidth=0.9, label='NSCC share (%)')
        ax.plot(t, cubic_share, color=colors_tcp[0], linewidth=0.9, label='Cubic share (%)')
        ax.plot(t, cum_nscc_share, color='black', linewidth=1.1, linestyle='--', label='NSCC cumulative share (%)')
        ax.axhline(50.0, color='gray', linestyle=':', linewidth=0.8, alpha=0.6, label='50/50')
        if transition_t is not None:
            ax.axvline(transition_t, color='black', linestyle='--', linewidth=0.8, alpha=0.6)
        ax.set_ylabel('Share (%)')
        ax.set_ylim(0, 100)
        ax.grid(True, alpha=0.3)
        ax.legend(loc='upper left', fontsize=8, framealpha=0.9)
    else:
        ax.text(0.5, 0.5, 'Share plot unavailable (need >=1 TCP and >=1 NSCC flow)',
                transform=ax.transAxes, ha='center', va='center', fontsize=9)
        ax.set_ylim(0, 100)

    # Overlay drops on right axis for context (if any)
    drops = df['queue_drops'].values.astype(np.float64)
    axd = ax.twinx()
    if drops.max() > 0:
        axd.step(t, drops, where='post', color='crimson', linewidth=0.8, alpha=0.35, label='Queue drops')
        axd.set_ylabel('Drops', color='crimson')
        axd.tick_params(axis='y', labelcolor='crimson')
        axd.set_ylim(0, drops.max() * 1.2)
    else:
        axd.set_ylabel('')
        axd.set_yticks([])

    ax.set_xlabel('Time (us)')

    # Format x-axis as "XXk" for thousands of microseconds
    for a in axes:
        a.xaxis.set_major_formatter(FuncFormatter(lambda x, _: f'{x/1000:.0f}k' if x >= 1000 else f'{x:.0f}'))

    plt.tight_layout()
    plt.savefig(out_path, dpi=150, bbox_inches='tight')
    print(f"Saved figure to {out_path}")
    plt.close()

if __name__ == '__main__':
    main()
