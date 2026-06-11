#!/bin/bash
# ===========================================================================
# Build & Run AstraLog-HPC
# ===========================================================================
set -euo pipefail

usage() {
    cat <<EOF
Usage: ./build_and_run.sh [options]

Options:
  --clean       Remove the build and output directories before building.
  --benchmark   Run with --benchmark to disable batch audit files and phase logs.
  --help        Show this help message.

Environment overrides:
  CSV_PATH          input/telemetry/export_sat_alpha_small.csv
  RULES_PATH        input/rules_SAT_ALPHA.json
  SENSORS_PATH      input/sensors_SAT_ALPHA.yaml
  OUTPUT_DIR        output
  BUILD_DIR         build
  THREADS           unset, lets OpenMP choose
  BATCH_STRATEGY    count
  BATCH_SIZE        1000
  BATCH_INTERVAL    5000
EOF
}

# Detect number of available cores for parallel compilation.
CORES=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

CLEAN=false
BENCHMARK=false

while [ "$#" -gt 0 ]; do
    case "$1" in
        --clean)
            CLEAN=true
            ;;
        --benchmark)
            BENCHMARK=true
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "[args] ERROR: unknown option: $1"
            usage
            exit 1
            ;;
    esac
    shift
done

CSV_PATH="${CSV_PATH:-input/telemetry/export_sat_alpha_small.csv}"
RULES_PATH="${RULES_PATH:-input/rules_SAT_ALPHA.json}"
SENSORS_PATH="${SENSORS_PATH:-input/sensors_SAT_ALPHA.yaml}"
OUTPUT_DIR="${OUTPUT_DIR:-output}"
BUILD_DIR="${BUILD_DIR:-build}"
THREADS="${THREADS:-}"
BATCH_STRATEGY="${BATCH_STRATEGY:-count}"
BATCH_SIZE="${BATCH_SIZE:-1000}"
BATCH_INTERVAL="${BATCH_INTERVAL:-5000}"

if [ "${CLEAN}" = true ]; then
    echo "[clean] Purging build and output directories..."
    rm -rf "${BUILD_DIR}" "${OUTPUT_DIR}"
fi

echo "[build] Configuring CMake in ${BUILD_DIR}..."
cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release

echo "[build] Compiling binary with ${CORES} cores..."
cmake --build "${BUILD_DIR}" --parallel "${CORES}"

run_args=(
    --csv "${CSV_PATH}"
    --rules "${RULES_PATH}"
    --sensors "${SENSORS_PATH}"
    --output-dir "${OUTPUT_DIR}"
    --batch-strategy "${BATCH_STRATEGY}"
    --batch-size "${BATCH_SIZE}"
    --batch-interval "${BATCH_INTERVAL}"
)

if [ -n "${THREADS}" ]; then
    run_args+=(--threads "${THREADS}")
fi

if [ "${BENCHMARK}" = true ]; then
    run_args+=(--benchmark)
fi

echo "[run] Executing AstraLog-HPC single-node engine..."
"./${BUILD_DIR}/bin/astralog_processing" "${run_args[@]}"
