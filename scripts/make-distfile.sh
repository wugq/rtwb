#!/bin/sh
# Create the distfile of ports/net/rtwb-kmod from a git ref, under the
# name the ports framework expects for USE_GITHUB:
#   wugq-rtwb-v<version>_GH0.tar.gz, top directory rtwb-<version>/
#
# usage: scripts/make-distfile.sh version [outdir [ref]]
#   e.g. scripts/make-distfile.sh 0.1.0 /usr/ports/distfiles v0.1.0
set -eu

version=${1:?usage: $0 version [outdir [ref]]}
outdir=${2:-.}
ref=${3:-HEAD}
top=$(git -C "$(dirname "$0")/.." rev-parse --show-toplevel)
out="${outdir}/wugq-rtwb-v${version}_GH0.tar.gz"

git -C "$top" archive --format=tar.gz --prefix="rtwb-${version}/" \
    -o "$out" "$ref"
echo "$out"
