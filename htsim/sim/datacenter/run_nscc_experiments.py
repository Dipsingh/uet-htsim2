#!/usr/bin/env python3
"""
NSCC Deep Dive Experiment Runner
=================================

Builds the simulator, generates traffic matrices, runs parameter sweeps,
and collects per-flow CSV stats + time-series traces for the NSCC Deep Dive
notebook and markdown.

Usage:
    python3 run_nscc_experiments.py [--build-dir BUILD] [--jobs N] [--experiments EXP1,EXP2,...]

Experiments:
    fairness         - Fairness vs flow count (Section 3)
    qa_gate          - QA gate sensitivity under incast (Section 5)
    incast_degree    - Throughput vs incast fan-in (Section 5)
    target_delay     - Target delay sensitivity (Section 4/7)
    coexistence      - NSCC vs Cubic coexistence (Section 9)
    traffic_pattern  - Traffic pattern comparison (Section 10)
    trace_quadrant   - Time-series quadrant traces (Phase 2, Section 2)
    trace_cwnd       - Time-series cwnd convergence (Phase 2, Section 3)
    trace_delay      - Time-series delay traces (Phase 2, Section 4)
    trace_qa         - Time-series QA firing (Phase 2, Section 5)
    trace_coexist    - Time-series NSCC vs Cubic cwnd (Phase 2, Section 9)
    all              - Run all experiments

Results are written to: results/deep_dive/
Figures are saved to:   figures/
"""

import argparse
import os
import subprocess
import sys
import shutil
from multiprocessing import Pool, cpu_count
from pathlib import Path

# ── Paths ──────────────────────────────────────────────────────────────
SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent.parent  # htsim/sim/datacenter -> repo root
CMAKE_SOURCE_DIR = REPO_ROOT / "htsim" / "sim"  # CMakeLists.txt lives here
BUILD_DIR = REPO_ROOT / "build"
CM_DIR = SCRIPT_DIR / "connection_matrices"
RESULTS_DIR = SCRIPT_DIR / "results" / "deep_dive"
FIGURES_DIR = SCRIPT_DIR / "figures"
TOPO_DIR = SCRIPT_DIR / "topologies"
GEN_INCAST = CM_DIR / "gen_incast.py"
GEN_PERM = CM_DIR / "gen_permutation.py"

# Default simulation parameters
NODES = 128
TOPO_FILE = TOPO_DIR / "leaf_spine_128_1to1.topo"
LINKSPEED = 100000  # 100 Gbps in Mbps
FLOW_SIZE = 2_000_000  # 2MB default
SEED = 42


def ensure_dir(path):
    """Create directory if it doesn't exist."""
    Path(path).mkdir(parents=True, exist_ok=True)


def build_simulator(build_dir, jobs):
    """Build htsim_uec and htsim_mixed if needed."""
    build_dir = Path(build_dir)
    build_dir.mkdir(parents=True, exist_ok=True)

    print(f"[BUILD] Configuring in {build_dir}...")
    subprocess.run(["cmake", str(CMAKE_SOURCE_DIR)], cwd=str(build_dir), check=True)

    print(f"[BUILD] Building htsim_uec and htsim_mixed with {jobs} jobs...")
    subprocess.run(["make", "htsim_uec", "htsim_mixed", f"-j{jobs}"],
                   cwd=str(build_dir), check=True)

    # Verify binaries exist (they get symlinked to datacenter dir)
    for binary in ["htsim_uec", "htsim_mixed"]:
        path = SCRIPT_DIR / binary
        if not path.exists():
            # Try build directory
            alt = build_dir / "datacenter" / binary
            if alt.exists():
                print(f"[BUILD] Binary at {alt}")
            else:
                print(f"[BUILD] WARNING: {binary} not found at {path} or {alt}")
        else:
            print(f"[BUILD] {binary} ready at {path}")


def gen_traffic_matrix(tm_path, gen_script, args):
    """Generate a traffic matrix file using a gen_*.py script."""
    if Path(tm_path).exists():
        print(f"  [TM] Reusing existing {tm_path}")
        return
    cmd = ["python3", str(gen_script)] + [str(a) for a in args]
    print(f"  [TM] Generating {tm_path}")
    subprocess.run(cmd, check=True)


def run_sim(binary, args, label, cwd=None):
    """Run a simulation binary with arguments, return (label, returncode, stdout_snippet)."""
    binary_path = SCRIPT_DIR / binary
    if not binary_path.exists():
        # Try build directory
        binary_path = BUILD_DIR / "datacenter" / binary
    cmd = [str(binary_path)] + [str(a) for a in args]
    print(f"  [RUN] {label}: {' '.join(cmd[:6])}...")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True,
                                timeout=300, cwd=cwd or str(SCRIPT_DIR))
        if result.returncode != 0:
            print(f"  [ERR] {label} failed (rc={result.returncode})")
            print(f"         stderr: {result.stderr[:500]}")
        return label, result.returncode, result.stdout[-500:] if result.stdout else ""
    except subprocess.TimeoutExpired:
        print(f"  [ERR] {label} timed out after 300s")
        return label, -1, "TIMEOUT"


def run_sim_task(task):
    """Wrapper for Pool.map."""
    return run_sim(*task)


# ═══════════════════════════════════════════════════════════════════════
# EXPERIMENT DEFINITIONS
# ═══════════════════════════════════════════════════════════════════════

def exp_fairness(parallel_jobs=4):
    """Fairness vs flow count: N=8,16,32,64,128 permutation flows."""
    print("\n[EXP] Fairness vs Flow Count (Section 3)")
    exp_dir = RESULTS_DIR / "fairness"
    ensure_dir(exp_dir)

    tasks = []
    for n_flows in [8, 16, 32, 64, 128]:
        tm_path = exp_dir / f"perm_{n_flows}f.cm"
        csv_path = exp_dir / f"fairness_{n_flows}f.csv"

        gen_traffic_matrix(tm_path, GEN_PERM,
                           [str(tm_path), NODES, n_flows, FLOW_SIZE, 0, SEED])

        tasks.append(("htsim_uec", [
            "-topo", str(TOPO_FILE), "-tm", str(tm_path),
            "-end", "200", "-seed", SEED,
            "-strat", "ecmp_host",
            "-csv", str(csv_path),
        ], f"fairness_{n_flows}f"))

    with Pool(min(parallel_jobs, len(tasks))) as pool:
        results = pool.map(run_sim_task, tasks)

    for label, rc, _ in results:
        status = "OK" if rc == 0 else "FAIL"
        print(f"  [{status}] {label}")


def exp_qa_gate(parallel_jobs=4):
    """QA gate sensitivity: qa_gate=0..4 under 32-to-1 incast."""
    print("\n[EXP] QA Gate Sensitivity (Section 5)")
    exp_dir = RESULTS_DIR / "qa_gate"
    ensure_dir(exp_dir)

    # Generate 32-to-1 incast traffic
    tm_path = exp_dir / "incast_32to1.cm"
    gen_traffic_matrix(tm_path, GEN_INCAST,
                       [str(tm_path), NODES, 32, FLOW_SIZE, 0, SEED, 0])

    tasks = []
    for qa in range(0, 5):
        csv_path = exp_dir / f"qa_gate_{qa}.csv"
        tasks.append(("htsim_uec", [
            "-topo", str(TOPO_FILE), "-tm", str(tm_path),
            "-end", "200", "-seed", SEED,
            "-qa_gate", qa,
            "-strat", "ecmp_host",
            "-csv", str(csv_path),
        ], f"qa_gate_{qa}"))

    with Pool(min(parallel_jobs, len(tasks))) as pool:
        results = pool.map(run_sim_task, tasks)

    for label, rc, _ in results:
        status = "OK" if rc == 0 else "FAIL"
        print(f"  [{status}] {label}")


def exp_incast_degree(parallel_jobs=4):
    """Incast degree: N=8,16,32,64 senders to 1 receiver."""
    print("\n[EXP] Incast Degree Sweep (Section 5)")
    exp_dir = RESULTS_DIR / "incast_degree"
    ensure_dir(exp_dir)

    tasks = []
    for degree in [8, 16, 32, 64]:
        tm_path = exp_dir / f"incast_{degree}to1.cm"
        csv_path = exp_dir / f"incast_{degree}to1.csv"

        gen_traffic_matrix(tm_path, GEN_INCAST,
                           [str(tm_path), NODES, degree, FLOW_SIZE, 0, SEED, 0])

        tasks.append(("htsim_uec", [
            "-topo", str(TOPO_FILE), "-tm", str(tm_path),
            "-end", "200", "-seed", SEED,
            "-strat", "ecmp_host",
            "-csv", str(csv_path),
        ], f"incast_{degree}to1"))

    with Pool(min(parallel_jobs, len(tasks))) as pool:
        results = pool.map(run_sim_task, tasks)

    for label, rc, _ in results:
        status = "OK" if rc == 0 else "FAIL"
        print(f"  [{status}] {label}")


def exp_target_delay(parallel_jobs=4):
    """Target delay sensitivity: target_q_delay=3,5,7,9us."""
    print("\n[EXP] Target Delay Sensitivity (Section 4/7)")
    exp_dir = RESULTS_DIR / "target_delay"
    ensure_dir(exp_dir)

    # Use permutation traffic for this
    tm_path = exp_dir / "perm_128f.cm"
    gen_traffic_matrix(tm_path, GEN_PERM,
                       [str(tm_path), NODES, NODES, FLOW_SIZE, 0, SEED])

    tasks = []
    for delay_us in [3, 5, 7, 9]:
        csv_path = exp_dir / f"target_delay_{delay_us}us.csv"
        tasks.append(("htsim_uec", [
            "-topo", str(TOPO_FILE), "-tm", str(tm_path),
            "-end", "200", "-seed", SEED,
            "-target_q_delay", delay_us,
            "-strat", "ecmp_host",
            "-csv", str(csv_path),
        ], f"target_delay_{delay_us}us"))

    with Pool(min(parallel_jobs, len(tasks))) as pool:
        results = pool.map(run_sim_task, tasks)

    for label, rc, _ in results:
        status = "OK" if rc == 0 else "FAIL"
        print(f"  [{status}] {label}")


def exp_coexistence(parallel_jobs=4):
    """NSCC vs TCP Cubic coexistence: nscc_ratio sweep for scenarios A,B,C."""
    print("\n[EXP] NSCC vs Cubic Coexistence (Section 9)")
    exp_dir = RESULTS_DIR / "coexistence"
    ensure_dir(exp_dir)

    # Generate traffic matrices using gen_mixed_traffic.py
    gen_script = SCRIPT_DIR / "gen_mixed_traffic.py"
    subprocess.run(["python3", str(gen_script),
                    "--nodes", str(NODES),
                    "--outdir", str(CM_DIR),
                    "--seed", str(SEED)], check=True)

    scenarios = {
        "A": CM_DIR / f"mixed_scenA_perm_{NODES}n_2MB.cm",
        "B": CM_DIR / f"mixed_scenB_mixed_{NODES}n_256c.cm",
        "C": CM_DIR / f"mixed_scenC_incast_{NODES}n.cm",
    }

    tasks = []
    for ratio_pct in [0, 25, 50, 75, 100]:
        ratio = ratio_pct / 100.0
        for scen_name, tm_path in scenarios.items():
            csv_path = exp_dir / f"coexist_scen{scen_name}_ratio{ratio_pct}.csv"
            tasks.append(("htsim_mixed", [
                "-topo", str(TOPO_FILE), "-tm", str(tm_path),
                "-end", "200", "-seed", SEED,
                "-nscc_ratio", ratio,
                "-csv", str(csv_path),
                "-ecn",
            ], f"coexist_{scen_name}_r{ratio_pct}"))

    with Pool(min(parallel_jobs, len(tasks))) as pool:
        results = pool.map(run_sim_task, tasks)

    for label, rc, _ in results:
        status = "OK" if rc == 0 else "FAIL"
        print(f"  [{status}] {label}")


def exp_traffic_pattern(parallel_jobs=4):
    """Traffic pattern comparison: permutation, incast, and mixed."""
    print("\n[EXP] Traffic Pattern Comparison (Section 10)")
    exp_dir = RESULTS_DIR / "traffic_pattern"
    ensure_dir(exp_dir)

    # Permutation: 128 flows
    tm_perm = exp_dir / "perm_128f.cm"
    gen_traffic_matrix(tm_perm, GEN_PERM,
                       [str(tm_perm), NODES, NODES, FLOW_SIZE, 0, SEED])

    # Incast: 32-to-1
    tm_incast = exp_dir / "incast_32to1.cm"
    gen_traffic_matrix(tm_incast, GEN_INCAST,
                       [str(tm_incast), NODES, 32, FLOW_SIZE, 0, SEED, 0])

    # Mixed: reuse scenario B from coexistence
    gen_script = SCRIPT_DIR / "gen_mixed_traffic.py"
    subprocess.run(["python3", str(gen_script),
                    "--nodes", str(NODES),
                    "--outdir", str(CM_DIR),
                    "--seed", str(SEED)], check=True)
    tm_mixed = CM_DIR / f"mixed_scenB_mixed_{NODES}n_256c.cm"

    tasks = []
    for pattern, tm in [("permutation", tm_perm), ("incast", tm_incast), ("mixed", tm_mixed)]:
        csv_path = exp_dir / f"pattern_{pattern}.csv"
        tasks.append(("htsim_uec", [
            "-topo", str(TOPO_FILE), "-tm", str(tm),
            "-end", "200", "-seed", SEED,
            "-strat", "ecmp_host",
            "-csv", str(csv_path),
        ], f"pattern_{pattern}"))

    with Pool(min(parallel_jobs, len(tasks))) as pool:
        results = pool.map(run_sim_task, tasks)

    for label, rc, _ in results:
        status = "OK" if rc == 0 else "FAIL"
        print(f"  [{status}] {label}")


# ── Phase 2: Time-Series Trace Experiments ─────────────────────────────

def exp_trace_quadrant():
    """Time-series: 4 flows with quadrant coloring."""
    print("\n[EXP] Trace: Quadrant Decisions (Section 2)")
    exp_dir = RESULTS_DIR / "trace_quadrant"
    ensure_dir(exp_dir)

    tm_path = exp_dir / "perm_4f.cm"
    gen_traffic_matrix(tm_path, GEN_PERM,
                       [str(tm_path), NODES, 4, FLOW_SIZE, 0, SEED])

    csv_path = exp_dir / "quadrant_4f.csv"
    trace_path = exp_dir / "quadrant_4f_trace.csv"
    run_sim("htsim_uec", [
        "-topo", str(TOPO_FILE), "-tm", str(tm_path),
        "-end", "200", "-seed", SEED,
        "-strat", "ecmp_host",
        "-csv", str(csv_path),
        "-trace", str(trace_path),
    ], "trace_quadrant_4f")


def exp_trace_cwnd():
    """Time-series: cwnd convergence for 8+ flows."""
    print("\n[EXP] Trace: CWND Evolution (Section 3)")
    exp_dir = RESULTS_DIR / "trace_cwnd"
    ensure_dir(exp_dir)

    tm_path = exp_dir / "perm_16f.cm"
    gen_traffic_matrix(tm_path, GEN_PERM,
                       [str(tm_path), NODES, 16, FLOW_SIZE, 0, SEED])

    csv_path = exp_dir / "cwnd_16f.csv"
    trace_path = exp_dir / "cwnd_16f_trace.csv"
    run_sim("htsim_uec", [
        "-topo", str(TOPO_FILE), "-tm", str(tm_path),
        "-end", "200", "-seed", SEED,
        "-strat", "ecmp_host",
        "-csv", str(csv_path),
        "-trace", str(trace_path),
    ], "trace_cwnd_16f")


def exp_trace_delay():
    """Time-series: raw_delay vs avg_delay for one flow."""
    print("\n[EXP] Trace: Delay Filtering (Section 4)")
    exp_dir = RESULTS_DIR / "trace_delay"
    ensure_dir(exp_dir)

    tm_path = exp_dir / "perm_32f.cm"
    gen_traffic_matrix(tm_path, GEN_PERM,
                       [str(tm_path), NODES, 32, FLOW_SIZE, 0, SEED])

    csv_path = exp_dir / "delay_32f.csv"
    trace_path = exp_dir / "delay_32f_trace.csv"
    run_sim("htsim_uec", [
        "-topo", str(TOPO_FILE), "-tm", str(tm_path),
        "-end", "200", "-seed", SEED,
        "-strat", "ecmp_host",
        "-csv", str(csv_path),
        "-trace", str(trace_path),
    ], "trace_delay_32f")


def exp_trace_qa():
    """Time-series: QA firing during 64-to-1 incast."""
    print("\n[EXP] Trace: Quick Adapt Firing (Section 5)")
    exp_dir = RESULTS_DIR / "trace_qa"
    ensure_dir(exp_dir)

    tm_path = exp_dir / "incast_64to1.cm"
    gen_traffic_matrix(tm_path, GEN_INCAST,
                       [str(tm_path), NODES, 64, FLOW_SIZE, 0, SEED, 0])

    csv_path = exp_dir / "qa_64to1.csv"
    trace_path = exp_dir / "qa_64to1_trace.csv"
    run_sim("htsim_uec", [
        "-topo", str(TOPO_FILE), "-tm", str(tm_path),
        "-end", "200", "-seed", SEED,
        "-strat", "ecmp_host",
        "-csv", str(csv_path),
        "-trace", str(trace_path),
    ], "trace_qa_64to1")


def exp_trace_coexist():
    """Time-series: NSCC vs Cubic cwnd under shared bottleneck."""
    print("\n[EXP] Trace: NSCC vs Cubic cwnd (Section 9)")
    exp_dir = RESULTS_DIR / "trace_coexist"
    ensure_dir(exp_dir)

    # Use scenario A (permutation, uniform)
    gen_script = SCRIPT_DIR / "gen_mixed_traffic.py"
    subprocess.run(["python3", str(gen_script),
                    "--nodes", str(NODES),
                    "--outdir", str(CM_DIR),
                    "--seed", str(SEED)], check=True)
    tm_path = CM_DIR / f"mixed_scenA_perm_{NODES}n_2MB.cm"

    csv_path = exp_dir / "coexist_50_50.csv"
    trace_path = exp_dir / "coexist_50_50_trace.csv"
    run_sim("htsim_mixed", [
        "-topo", str(TOPO_FILE), "-tm", str(tm_path),
        "-end", "200", "-seed", SEED,
        "-nscc_ratio", "0.5",
        "-csv", str(csv_path),
        "-trace", str(trace_path),
        "-ecn",
    ], "trace_coexist_50_50")


# ═══════════════════════════════════════════════════════════════════════

EXPERIMENTS = {
    "fairness": exp_fairness,
    "qa_gate": exp_qa_gate,
    "incast_degree": exp_incast_degree,
    "target_delay": exp_target_delay,
    "coexistence": exp_coexistence,
    "traffic_pattern": exp_traffic_pattern,
    "trace_quadrant": exp_trace_quadrant,
    "trace_cwnd": exp_trace_cwnd,
    "trace_delay": exp_trace_delay,
    "trace_qa": exp_trace_qa,
    "trace_coexist": exp_trace_coexist,
}


def main():
    parser = argparse.ArgumentParser(
        description="NSCC Deep Dive Experiment Runner",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    parser.add_argument("--build-dir", type=str, default=str(BUILD_DIR),
                        help=f"Build directory (default: {BUILD_DIR})")
    parser.add_argument("--jobs", "-j", type=int, default=max(1, cpu_count() // 2),
                        help="Parallel build/run jobs")
    parser.add_argument("--experiments", "-e", type=str, default="all",
                        help="Comma-separated experiments to run, or 'all'")
    parser.add_argument("--skip-build", action="store_true",
                        help="Skip building the simulator")
    parser.add_argument("--parallel", "-p", type=int, default=4,
                        help="Max parallel simulations per experiment")
    args = parser.parse_args()

    ensure_dir(RESULTS_DIR)
    ensure_dir(FIGURES_DIR)

    if not args.skip_build:
        build_simulator(args.build_dir, args.jobs)

    if args.experiments == "all":
        exp_list = list(EXPERIMENTS.keys())
    else:
        exp_list = [e.strip() for e in args.experiments.split(",")]

    print(f"\n{'='*60}")
    print(f"Running {len(exp_list)} experiment(s): {', '.join(exp_list)}")
    print(f"{'='*60}")

    for exp_name in exp_list:
        if exp_name not in EXPERIMENTS:
            print(f"[WARN] Unknown experiment: {exp_name}")
            continue
        func = EXPERIMENTS[exp_name]
        # Phase 1 experiments accept parallel_jobs, Phase 2 trace experiments don't
        import inspect
        sig = inspect.signature(func)
        if 'parallel_jobs' in sig.parameters:
            func(parallel_jobs=args.parallel)
        else:
            func()

    print(f"\n{'='*60}")
    print(f"All experiments complete.")
    print(f"Results in: {RESULTS_DIR}")
    print(f"Run the notebook to generate figures: NSCC_DEEP_DIVE.ipynb")
    print(f"{'='*60}")


if __name__ == "__main__":
    main()
