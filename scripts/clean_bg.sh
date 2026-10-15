#!/usr/bin/env bash
# Drop the background-compile cache (build/gen-bg, build/kernels/libvgen_*_<hash>.so).
root="$(cd "$(dirname "$0")/.." && pwd)"
rm -rf "$root/build/gen-bg"
find "$root/build/kernels" -name 'libvgen_*_[0-9]*.so' -delete 2>/dev/null || true
