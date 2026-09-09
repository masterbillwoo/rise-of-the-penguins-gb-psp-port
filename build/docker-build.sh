#!/bin/bash
# Builds the container image (once) and then compiles MasterBoy inside it.
# Usage, from the repo root:   bash build/docker-build.sh
set -e

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

echo "=== building image masterboy-psp ==="
docker build -t masterboy-psp -f build/Dockerfile build/

echo "=== compiling ==="
docker run --rm -v "$ROOT:/project" masterboy-psp bash /project/build/build.sh
