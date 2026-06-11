# AstraLog-HPC Execution Guide

This guide summarizes how to run AstraLog-HPC locally, on the Galileo100 cluster and how to perform profiling.

## 1. Local Execution

Use local execution for quick correctness checks, output inspection, and rough timing. Do not use login-node or local WSL timings as final HPC performance numbers.

From the repository root:

```bash
cd ~/AgostaAmodeoAnzalone
```

Load tools if needed:

```bash
module load gcc/12.2.0
module load cmake/3.27.7
```

Run the default local dataset:

```bash
bash build_and_run.sh
```

Clean build/output folders and run:

```bash
bash build_and_run.sh --clean
```

Run in benchmark mode:

```bash
bash build_and_run.sh --benchmark
```

Run a specific CSV:

```bash
CSV_PATH=input/telemetry/export_sat_alpha_large.csv bash build_and_run.sh --benchmark
```

Run the fixed timestamp CSV:

```bash
CSV_PATH=input/telemetry/export_sat_alpha_large_fixed.csv bash build_and_run.sh --benchmark
```

Set batch size:

```bash
BATCH_SIZE=100000 CSV_PATH=input/telemetry/export_sat_alpha_large_fixed.csv bash build_and_run.sh --benchmark
```

Set number of OpenMP threads:

```bash
THREADS=8 CSV_PATH=input/telemetry/export_sat_alpha_large_fixed.csv bash build_and_run.sh --benchmark
```

Useful environment variables for `build_and_run.sh`:

```text
CSV_PATH          default: input/telemetry/export_sat_alpha_small.csv
RULES_PATH        default: input/rules_SAT_ALPHA.json
SENSORS_PATH      default: input/sensors_SAT_ALPHA.yaml
OUTPUT_DIR        default: output
BUILD_DIR         default: build
THREADS           default: unset, OpenMP chooses
BATCH_STRATEGY    default: count
BATCH_SIZE        default: 1000
BATCH_INTERVAL    default: 5000
```

## 2. Fixing Repeated Timestamps

Some provided telemetry files contain repeated timestamps. The helper script `input/telemetry/fix.sh` generates a new file where timestamps are incremented every 12 lines.

Generate a fixed file:

```bash
cd input/telemetry
bash fix.sh export_sat_alpha_large.csv
cd ../..
```

This creates:

```text
input/telemetry/export_sat_alpha_large_fixed.csv
```

Use the fixed CSV locally:

```bash
CSV_PATH=input/telemetry/export_sat_alpha_large_fixed.csv bash build_and_run.sh --benchmark
```

Use the fixed CSV on the cluster:

```bash
sbatch --export=ALL,CSV_PATH=input/telemetry/export_sat_alpha_large_fixed.csv job.sh
```

Generated `*_fixed.csv` files are ignored by Git.

## 3. Cluster Execution with SLURM

Use `job.sh` for real HPC benchmark numbers. It runs on a compute node with one task and 48 OpenMP threads.

From the repository root on Galileo100:

```bash
cd ~/AgostaAmodeoAnzalone
```

Submit the default cluster job:

```bash
sbatch job.sh
```

Submit with the fixed CSV:

```bash
sbatch --export=ALL,CSV_PATH=input/telemetry/export_sat_alpha_large_fixed.csv job.sh
```

Submit with fixed CSV and explicit batch size:

```bash
sbatch --export=ALL,CSV_PATH=input/telemetry/export_sat_alpha_large_fixed.csv,BATCH_SIZE=100000 job.sh
```

Keep batch audit files enabled:

```bash
sbatch --export=ALL,BENCHMARK=0 job.sh
```

Important: `sbatch` options must be placed before `job.sh`. This is correct:

```bash
sbatch --export=ALL,CSV_PATH=input/telemetry/export_sat_alpha_large_fixed.csv job.sh
```

Check job status:

```bash
squeue -j <job_id>
```

Read SLURM output:

```bash
cat astralog_<job_id>.out
cat astralog_<job_id>.err
```

Check generated output files:

```bash
ls -lh output
wc -l output/valid_data.csv output/alarms.log
head output/valid_data.csv
head output/alarms.log
```

Useful `job.sh` environment variables:

```text
CSV_PATH          default: input/telemetry/export_sat_alpha_large.csv
RULES_PATH        default: input/rules_SAT_ALPHA.json
SENSORS_PATH      default: input/sensors_SAT_ALPHA.yaml
OUTPUT_DIR        default: output
EXECUTABLE        default: build/bin/astralog_processing
BATCH_STRATEGY    default: count
BATCH_SIZE        default: 100000
BATCH_INTERVAL    default: 5000
BENCHMARK         default: 1
```

## 4. Profiling

Profiling and benchmarking are different:

```text
job.sh              real cluster benchmark timing
profile_gprof.sh    function-level hotspot analysis
build_and_run.sh    local quick run / functional check
```

`profile_gprof.sh` compiles with:

```text
-pg -g0 -fno-inline-functions
```

This changes the binary and makes it slower. Use profiling output to understand where time is spent, not as final performance.

Run profiling locally or on a login node:

```bash
CSV_PATH=input/telemetry/export_sat_alpha_large_fixed.csv bash profile_gprof.sh
```

Profile with one thread to get a cleaner `gprof` call graph:

```bash
THREADS=1 CSV_PATH=input/telemetry/export_sat_alpha_large_fixed.csv bash profile_gprof.sh
```

Profile with a custom batch size:

```bash
BATCH_SIZE=100000 CSV_PATH=input/telemetry/export_sat_alpha_large_fixed.csv bash profile_gprof.sh
```

Profiling output is written to:

```text
output_profile/profile_analysis.txt
```

Inspect it with:

```bash
less output_profile/profile_analysis.txt
head -80 output_profile/profile_analysis.txt
```

Run profiling on a compute node through SLURM:

```bash
sbatch --cpus-per-task=48 --nodes=1 --time=00:30:00 --partition=g100_usr_prod \
  --export=ALL,CSV_PATH=input/telemetry/export_sat_alpha_large_fixed.csv \
  --wrap="cd $PWD && bash profile_gprof.sh"
```

Notes about `gprof`:

- `gprof` call counts can be misleading with OpenMP.
- Trust the program phase timings more than surprising call counts.
- Use `THREADS=1` when you want cleaner function attribution.
- Use `job.sh` when you need final report performance numbers.

## 5. Build and Test

Build and run tests manually:

```bash
module load gcc/12.2.0
module load cmake/3.27.7

cmake -S . -B build_test -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build_test -j
ctest --test-dir build_test --output-on-failure
```

## 6. Recommended Final Workflow

For final correctness and performance collection:

```bash
cd ~/AgostaAmodeoAnzalone

module load gcc/12.2.0
module load cmake/3.27.7

cd input/telemetry
bash fix.sh export_sat_alpha_large.csv
cd ../..

cmake -S . -B build_test -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build_test -j
ctest --test-dir build_test --output-on-failure

sbatch --export=ALL,CSV_PATH=input/telemetry/export_sat_alpha_large_fixed.csv job.sh
```

After the job finishes:

```bash
cat astralog_<job_id>.out
cat astralog_<job_id>.err
wc -l output/valid_data.csv output/alarms.log
```
