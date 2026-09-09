#!/bin/bash
# Compiles MasterBoy (with the single-game launcher patch) inside the PSP container.
# Run from the repo root via build/docker-build.sh, or manually:
#   docker run --rm -v "$PWD:/project" masterboy-psp bash build/build.sh
set -e

export PSPDEV=/usr/local/pspdev
export PSPSDK=$PSPDEV/psp/sdk
export PATH=$PSPDEV/bin:$PATH

echo "=== MasterBoy single-game build ==="
psp-gcc --version | head -1

cd /project/_masterboy

echo "--- cleaning ---"
make -f Makefile.psp clean || true

echo "--- compiling ---"
make -f Makefile.psp all -j"$(nproc)"

if [ ! -f EBOOT.PBP ]; then
    echo "ERROR: EBOOT.PBP was not produced" >&2
    exit 1
fi

mkdir -p /project/build/out
cp EBOOT.PBP /project/build/out/EBOOT.PBP
echo "=== OK: build/out/EBOOT.PBP ($(stat -c%s EBOOT.PBP) bytes) ==="
