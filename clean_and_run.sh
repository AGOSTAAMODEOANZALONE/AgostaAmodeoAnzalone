#!/bin/bash
# ===========================================================================
# Clean Outputs & Run AstraLog-HPC
# ===========================================================================
set -e

echo "[clean] Removing outputs folder..."
rm -rf output

echo "[run] Executing AstraLog-HPC single-node engine..."
if [ -f "./build/bin/astralog_processing" ]; then
    ./build/bin/astralog_processing \
        --csv input/telemetry/export_sat_alpha_small.csv \
        --rules input/rules_SAT_ALPHA.json \
        --sensors input/sensors_SAT_ALPHA.yaml \
        --output-dir output/
else
    echo "[run] ERROR: Compiled binary not found at ./build/bin/astralog_processing."
    echo "[run] Please run ./build_and_run.sh first to build the executable."
    exit 1
fi
