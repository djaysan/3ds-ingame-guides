#!/bin/sh
# Build default.3gx in Docker (libctrpf 0.8.0 image, amd64; runs emulated on Apple Silicon, seconds).
# Always cleans first: the template Makefile can repackage a stale .elf into a new .3gx.
set -e
cd "$(dirname "$0")"
NAME=$(basename "$PWD")   # the Makefile names its output after the folder
rm -rf Build "$NAME.elf" "$NAME.3gx"
docker run --rm --platform linux/amd64 -v "$PWD":/work -w /work pablomk7/libctrpf:0.8.0 make
cp "$NAME.3gx" default.3gx
ls -l default.3gx
