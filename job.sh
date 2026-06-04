#!/bin/bash
# ===========================================================================
# AstraLog-HPC — Single-Node OpenMP Slurm Job Script
# ===========================================================================
#
# Targets a single 48-core Galileo 100 compute node.
# All parallelism is shared-memory via OpenMP — no MPI, no multi-node,
# no ZeroMQ networking, no Python dependencies.
#
# Usage:
#   sbatch job.sh
#   sbatch job.sh --export=CSV_PATH=input/telemetry/export_sat_alpha_large.csv
# ===========================================================================

#SBATCH --job-name=astralog_hpc
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=48
#SBATCH --partition=g100_usr_prod
#SBATCH --time=00:30:00
#SBATCH --output=astralog_%j.out
#SBATCH --error=astralog_%j.err
#SBATCH --mem=0

# === Thread Configuration ===
# Bind OMP threads 1:1 to physical cores — zero over-subscription
export OMP_NUM_THREADS=${SLURM_CPUS_PER_TASK}
export OMP_PROC_BIND=close
export OMP_PLACES=cores
export OMP_SCHEDULE="dynamic,1024"
export OMP_STACKSIZE=8M

# === Paths ===
# Allow override via sbatch --export or environment variables
CSV_PATH="${CSV_PATH:-input/telemetry/export_sat_alpha_large.csv}"
RULES_PATH="${RULES_PATH:-input/rules_SAT_ALPHA.json}"
SENSORS_PATH="${SENSORS_PATH:-input/sensors_SAT_ALPHA.yaml}"
OUTPUT_DIR="${OUTPUT_DIR:-output}"
EXECUTABLE="${EXECUTABLE:-build/bin/astralog_processing}"

# === Container Support ===
# If a Singularity container is present, run inside it.
# Otherwise, run the executable directly.
CONTAINER="astralog.sif"

echo "============================================================="
echo "  AstraLog-HPC — Single-Node OpenMP"
echo "============================================================="
echo "  Job ID:         ${SLURM_JOB_ID}"
echo "  Node:           $(hostname)"
echo "  CPUs per task:  ${SLURM_CPUS_PER_TASK}"
echo "  OMP threads:    ${OMP_NUM_THREADS}"
echo "  OMP bind:       ${OMP_PROC_BIND}"
echo "  OMP places:     ${OMP_PLACES}"
echo "  CSV file:       ${CSV_PATH}"
echo "  Rules file:     ${RULES_PATH}"
echo "  Sensors file:   ${SENSORS_PATH}"
echo "  Output dir:     ${OUTPUT_DIR}"
echo "  Executable:     ${EXECUTABLE}"
echo "============================================================="

# === Create output directory ===
mkdir -p "${OUTPUT_DIR}"

# === Build (if build directory doesn't exist) ===
if [ ! -f "${EXECUTABLE}" ]; then
    echo "[job] Executable not found. Building..."
    mkdir -p build
    cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j${SLURM_CPUS_PER_TASK}
    cd ..
    echo "[job] Build complete."
fi

# === Run ===
echo "[job] Starting AstraLog-HPC processing at $(date)"
START_TIME=$(date +%s%N)

if [ -f "${CONTAINER}" ]; then
    echo "[job] Running inside Singularity container: ${CONTAINER}"
    singularity exec "${CONTAINER}" "${EXECUTABLE}" \
        --csv "${CSV_PATH}" \
        --rules "${RULES_PATH}" \
        --sensors "${SENSORS_PATH}" \
        --output-dir "${OUTPUT_DIR}" \
        --threads "${OMP_NUM_THREADS}"
else
    echo "[job] Running native executable"
    "${EXECUTABLE}" \
        --csv "${CSV_PATH}" \
        --rules "${RULES_PATH}" \
        --sensors "${SENSORS_PATH}" \
        --output-dir "${OUTPUT_DIR}" \
        --threads "${OMP_NUM_THREADS}"
fi

EXIT_CODE=$?
END_TIME=$(date +%s%N)

# === Summary ===
ELAPSED_MS=$(( (END_TIME - START_TIME) / 1000000 ))
echo ""
echo "============================================================="
echo "  AstraLog-HPC — Job Complete"
echo "============================================================="
echo "  Exit code:      ${EXIT_CODE}"
echo "  Wall time:      ${ELAPSED_MS} ms"
echo "  Output files:"
echo "    ${OUTPUT_DIR}/valid_data.csv"
echo "    ${OUTPUT_DIR}/alarms.log"
echo "  Finished at:    $(date)"
echo "============================================================="

exit ${EXIT_CODE}
