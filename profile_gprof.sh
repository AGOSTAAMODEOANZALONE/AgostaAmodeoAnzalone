#!/bin/bash
# ===========================================================================
# AstraLog-HPC — gprof Profiling Automation
# ===========================================================================
set -e

CSV_PATH="${CSV_PATH:-input/telemetry/export_sat_alpha_large.csv}"
RULES_PATH="${RULES_PATH:-input/rules_SAT_ALPHA.json}"
SENSORS_PATH="${SENSORS_PATH:-input/sensors_SAT_ALPHA.yaml}"
OUTPUT_DIR="${OUTPUT_DIR:-output_profile/}"
BATCH_SIZE="${BATCH_SIZE:-100000}"
THREADS="${THREADS:-}"

echo "[profile] Cleaning build and outputs..."
rm -rf build_profile "${OUTPUT_DIR}"
rm -f gmon.out
mkdir -p build_profile/bin
mkdir -p "${OUTPUT_DIR}"

echo "[profile] Compiling with gprof flags (-pg)..."
echo "[profile] NOTE: this build is for hotspot ranking, not benchmark timing."
# We compile src/main.cpp with -pg for profiling.
# We also use -O2 and -fno-inline-functions so gprof can measure actual functions 
# rather than letting the compiler inline everything.
g++ -O2 -pg -g0 -fno-inline-functions -std=c++17 -fopenmp \
    -Isrc src/main.cpp -o build_profile/bin/astralog_processing

echo "[profile] Running engine to gather profile data..."
echo "[profile] CSV: ${CSV_PATH}"
# Run the executable. It will generate a 'gmon.out' file in the current directory.
run_args=(
    --csv "${CSV_PATH}"
    --rules "${RULES_PATH}"
    --sensors "${SENSORS_PATH}"
    --output-dir "${OUTPUT_DIR}"
    --batch-strategy count
    --batch-size "${BATCH_SIZE}"
    --benchmark
)

if [ -n "${THREADS}" ]; then
    run_args+=(--threads "${THREADS}")
fi

./build_profile/bin/astralog_processing "${run_args[@]}"

echo "[profile] Generating gprof analysis..."
# Run gprof and direct output to profile_analysis.txt
gprof ./build_profile/bin/astralog_processing gmon.out > "${OUTPUT_DIR}/profile_analysis.txt"

echo "[profile] Profiling completed! Top 15 functions by execution time:"
echo "---------------------------------------------------------------------"
# Filter and display the top functions from the flat profile
awk '
    BEGIN { in_profile = 0; printed = 0 }
    /^[Ff]lat profile:/ { in_profile = 1; next }
    in_profile && printed < 18 { print; printed++ }
' "${OUTPUT_DIR}/profile_analysis.txt"
echo "---------------------------------------------------------------------"
echo "Full profile analysis saved to: ${OUTPUT_DIR}/profile_analysis.txt"
