#!/bin/bash
# Builds the Apptainer (Singularity) image using Docker
# This is especially useful for macOS users who cannot run Apptainer natively.

# Exit immediately if a command exits with a non-zero status
set -e

IMAGE_NAME="astralog.sif"
DEF_FILE="Singularity.def"

echo "============================================================"
echo "Building AstraLog Apptainer Image via Docker"
echo "============================================================"

# Ensure Docker is running
if ! docker info > /dev/null 2>&1; then
    echo "Error: Docker is not running or not installed. Please start Docker Desktop and try again."
    exit 1
fi

echo "Detected OS: $(uname -s)"
echo "Detected Arch: $(uname -m)"
echo ""
echo "Note: The image is explicitly built for linux/amd64 (x86_64) to be compatible with CINECA G100."
echo "Running Apptainer build inside ghcr.io/apptainer/apptainer container..."

# We use --platform linux/amd64 to ensure the build outputs an x86_64 image,
# which is required for running on the G100 cluster.
docker run --rm \
  --privileged \
  -v "${PWD}:/workdir" \
  -w /workdir \
  --platform linux/amd64 \
  ghcr.io/apptainer/apptainer:latest \
  apptainer build "${IMAGE_NAME}" "${DEF_FILE}"

echo ""
echo "============================================================"
echo "Build complete! The Apptainer image has been saved as ${IMAGE_NAME}."
echo ""
echo "You can now copy it to G100 using scp:"
echo "scp ${IMAGE_NAME} <username>@login.g100.cineca.it:/path/to/destination/"
echo "============================================================"
