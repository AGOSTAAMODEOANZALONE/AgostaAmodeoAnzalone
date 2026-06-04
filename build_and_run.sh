#!/bin/bash
# ===========================================================================
# Build & Run AstraLog-HPC
# ===========================================================================
set -e

# Detect number of available cores for parallel compilation
CORES=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Check for --clean argument
CLEAN=false
for arg in "$@"; do
    if [ "$arg" = "--clean" ]; then
        CLEAN=true
    fi
done

if [ "$CLEAN" = true ]; then
    echo "[clean] Purging build and output directories..."
    rm -rf build
    rm -rf output
fi

echo "[build] Setting up build folder and configuring CMake..."
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release

echo "[build] Compiling binary with ${CORES} cores..."
make -j"${CORES}"
cd ..

echo "[run] Executing AstraLog-HPC single-node engine..."
./build/bin/astralog_processing \
    --csv input/telemetry/export_sat_alpha_small.csv \
    --rules input/rules_SAT_ALPHA.json \
    --sensors input/sensors_SAT_ALPHA.yaml \
    --output-dir output/
