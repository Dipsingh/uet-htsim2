#!/bin/bash
# ==============================================================================
# Mixed NSCC + TCP Cubic Co-existence Experiment
# ==============================================================================
#
# Runs a parameter sweep across 3 traffic scenarios and 5 NSCC/Cubic ratios
# on a 2-tier leaf-spine fat-tree topology (128 nodes, 100 Gbps).
#
# Usage:
#   ./run_mixed_experiment.sh [--build-only] [--skip-build] [--skip-gen]
#
# Output:
#   results/mixed_experiment/  -- CSV files and summary
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../../../build"
RESULTS_DIR="${SCRIPT_DIR}/results/mixed_experiment"
TOPO_FILE="${SCRIPT_DIR}/topologies/leaf_spine_128_1to1.topo"
CM_DIR="${SCRIPT_DIR}/connection_matrices"
BINARY="${SCRIPT_DIR}/htsim_mixed"

NODES=128
QUEUE_SIZE=100       # packets
END_TIME=500000      # 500ms in microseconds
SEED=42

# NSCC parameters
TARGET_Q_DELAY=5     # microseconds
QA_GATE=2
PATH_ENTROPY=16

# TCP Cubic parameters
CWND=10              # packets
HYSTART=1
FAST_CONV=1

# Sweep: NSCC ratio values
RATIOS="0.0 0.25 0.5 0.75 1.0"

# Scenarios
declare -A SCENARIO_FILES
declare -A SCENARIO_NAMES

BUILD_ONLY=false
SKIP_BUILD=false
SKIP_GEN=false

for arg in "$@"; do
    case $arg in
        --build-only) BUILD_ONLY=true ;;
        --skip-build) SKIP_BUILD=true ;;
        --skip-gen)   SKIP_GEN=true ;;
    esac
done

# ==============================================================================
# Step 1: Build
# ==============================================================================
if [ "$SKIP_BUILD" = false ]; then
    echo "=========================================="
    echo "Step 1: Building htsim_mixed"
    echo "=========================================="
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -5
    make htsim_mixed -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) 2>&1
    cd "$SCRIPT_DIR"
    echo "Build complete."
fi

if [ "$BUILD_ONLY" = true ]; then
    echo "Build-only mode, exiting."
    exit 0
fi

# ==============================================================================
# Step 2: Generate traffic matrices
# ==============================================================================
if [ "$SKIP_GEN" = false ]; then
    echo ""
    echo "=========================================="
    echo "Step 2: Generating traffic matrices"
    echo "=========================================="
    python3 "${SCRIPT_DIR}/gen_mixed_traffic.py" \
        --nodes "$NODES" \
        --outdir "$CM_DIR" \
        --seed "$SEED"
fi

SCENARIO_FILES[A]="${CM_DIR}/mixed_scenA_perm_${NODES}n_2MB.cm"
SCENARIO_FILES[B]="${CM_DIR}/mixed_scenB_mixed_${NODES}n_256c.cm"
SCENARIO_FILES[C]="${CM_DIR}/mixed_scenC_incast_${NODES}n.cm"
SCENARIO_NAMES[A]="Permutation_2MB"
SCENARIO_NAMES[B]="MixedSizes_10KB-10MB"
SCENARIO_NAMES[C]="Incast16to1_plus_BG"

# ==============================================================================
# Step 3: Run the sweep
# ==============================================================================
echo ""
echo "=========================================="
echo "Step 3: Running experiment sweep"
echo "=========================================="
echo "  Topology: $TOPO_FILE"
echo "  Queue size: $QUEUE_SIZE packets"
echo "  End time: $END_TIME us"
echo "  Ratios: $RATIOS"
echo ""

mkdir -p "$RESULTS_DIR"

# Check binary exists
if [ ! -f "$BINARY" ]; then
    echo "ERROR: Binary not found at $BINARY"
    echo "  Try running without --skip-build"
    exit 1
fi

TOTAL_RUNS=0
COMPLETED_RUNS=0

for scenario in A B C; do
    for ratio in $RATIOS; do
        TOTAL_RUNS=$((TOTAL_RUNS + 1))
    done
done

for scenario in A B C; do
    cm_file="${SCENARIO_FILES[$scenario]}"
    scen_name="${SCENARIO_NAMES[$scenario]}"

    if [ ! -f "$cm_file" ]; then
        echo "WARNING: Traffic matrix not found: $cm_file -- skipping scenario $scenario"
        continue
    fi

    for ratio in $RATIOS; do
        COMPLETED_RUNS=$((COMPLETED_RUNS + 1))
        ratio_label=$(echo "$ratio" | sed 's/\.//g')
        csv_out="${RESULTS_DIR}/scen${scenario}_ratio${ratio_label}.csv"
        log_out="${RESULTS_DIR}/scen${scenario}_ratio${ratio_label}.dat"

        echo "[$COMPLETED_RUNS/$TOTAL_RUNS] Scenario $scenario ($scen_name), ratio=$ratio"

        "$BINARY" \
            -topo "$TOPO_FILE" \
            -q "$QUEUE_SIZE" \
            -ecn \
            -tm "$cm_file" \
            -nscc_ratio "$ratio" \
            -target_q_delay "$TARGET_Q_DELAY" \
            -qa_gate "$QA_GATE" \
            -path_entropy "$PATH_ENTROPY" \
            -cwnd "$CWND" \
            -hystart "$HYSTART" \
            -fast_conv "$FAST_CONV" \
            -end "$END_TIME" \
            -seed "$SEED" \
            -csv "$csv_out" \
            -o "$log_out" \
            2>&1 | grep -E "(NSCC|TCP Cubic|Bandwidth|Jain|INTER-PROTOCOL|===|Done)" || true

        echo ""
    done
done

# ==============================================================================
# Step 4: Summarize results
# ==============================================================================
echo "=========================================="
echo "Step 4: Analyzing results"
echo "=========================================="

if command -v python3 &>/dev/null; then
    python3 "${SCRIPT_DIR}/analyze_mixed_results.py" \
        --results-dir "$RESULTS_DIR" \
        --output "${RESULTS_DIR}/summary.txt"
else
    echo "Python3 not found -- skipping analysis. Run manually:"
    echo "  python3 analyze_mixed_results.py --results-dir $RESULTS_DIR"
fi

echo ""
echo "=========================================="
echo "Experiment complete!"
echo "Results in: $RESULTS_DIR"
echo "=========================================="
