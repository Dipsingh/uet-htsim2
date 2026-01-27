#!/bin/bash
# Run NSCC vs TCP Cubic fairness comparison experiments
# Usage: ./run_fairness_experiments.sh [build_dir]

set -e

# Get build directory (default: ../../build/datacenter relative to script)
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD_DIR="${1:-$SCRIPT_DIR/../../build/datacenter}"
DATA_DIR="$SCRIPT_DIR/../.."
RESULTS_DIR="$SCRIPT_DIR/results/fairness"

# Verify executables exist
if [ ! -f "$BUILD_DIR/htsim_uec" ]; then
    echo "Error: htsim_uec not found at $BUILD_DIR/htsim_uec"
    echo "Please build the project first: cmake -S . -B build && cmake --build build"
    exit 1
fi

if [ ! -f "$BUILD_DIR/htsim_tcpcubic" ]; then
    echo "Error: htsim_tcpcubic not found at $BUILD_DIR/htsim_tcpcubic"
    echo "Please build the project first: cmake -S . -B build && cmake --build build"
    exit 1
fi

# Create results directory
mkdir -p "$RESULTS_DIR"

echo "=============================================="
echo "NSCC vs TCP Cubic Fairness Experiments"
echo "=============================================="
echo "Build directory: $BUILD_DIR"
echo "Results directory: $RESULTS_DIR"
echo ""

# Simulation parameters
END_TIME=100000  # 100ms in microseconds
SEED=42

# Define experiments - using existing topology files
TOPOLOGIES=("fat_tree_128_1os")
WORKLOADS=("fairness_perm_128n_2MB" "fairness_incast_16to1_2MB")

for topo in "${TOPOLOGIES[@]}"; do
    for workload in "${WORKLOADS[@]}"; do
        TOPO_FILE="$DATA_DIR/datacenter/topologies/${topo}.topo"
        TM_FILE="$DATA_DIR/datacenter/connection_matrices/fairness/${workload}.cm"

        # Check files exist
        if [ ! -f "$TOPO_FILE" ]; then
            echo "Warning: Topology file not found: $TOPO_FILE"
            continue
        fi
        if [ ! -f "$TM_FILE" ]; then
            echo "Warning: Traffic matrix not found: $TM_FILE"
            continue
        fi

        echo "=============================================="
        echo "Running: $topo / $workload"
        echo "=============================================="

        # Run NSCC (UEC)
        echo ""
        echo "--- NSCC ---"
        NSCC_LOG="$RESULTS_DIR/nscc_${topo}_${workload}.log"
        NSCC_OUT="$RESULTS_DIR/nscc_${topo}_${workload}.dat"

        "$BUILD_DIR/htsim_uec" \
            -topo "$TOPO_FILE" \
            -tm "$TM_FILE" \
            -strat ecmp_host \
            -end "$END_TIME" \
            -seed "$SEED" \
            -o "$NSCC_OUT" \
            2>&1 | tee "$NSCC_LOG"

        # Run TCP Cubic
        echo ""
        echo "--- TCP Cubic ---"
        CUBIC_LOG="$RESULTS_DIR/cubic_${topo}_${workload}.log"
        CUBIC_OUT="$RESULTS_DIR/cubic_${topo}_${workload}.dat"

        "$BUILD_DIR/htsim_tcpcubic" \
            -topo "$TOPO_FILE" \
            -tm "$TM_FILE" \
            -strat ecmp_host \
            -end "$END_TIME" \
            -seed "$SEED" \
            -o "$CUBIC_OUT" \
            2>&1 | tee "$CUBIC_LOG"

        echo ""
    done
done

echo "=============================================="
echo "Experiments complete!"
echo "Results saved to: $RESULTS_DIR"
echo "=============================================="
echo ""
echo "To analyze results, run:"
echo "  python3 $SCRIPT_DIR/../analysis/analyze_fairness.py $RESULTS_DIR"
