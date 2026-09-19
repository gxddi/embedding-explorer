#!/usr/bin/env bash

# Setup the dependencies: libtorch, oneAPI (if applicable), nlohmann's json and sqlite3

# Stop on errors, unset variables, and failed commands within pipelines
set -euo pipefail

# Resolve paths relative to script
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
deps_dir=$project_dir/deps
cache_dir=$deps_dir/cache

mkdir -p "$deps_dir"
mkdir -p "$cache_dir"

# Detect Intel display controller, USE_XPU=0 or USE_XPU=1 can hardcode detection
case "${USE_XPU:-auto}" in
1|on|true) use_xpu=1 ;;
0|off|false) use_xpu=0 ;;
auto)
    lspci -D | grep -Ei 'VGA|3D|Display' |
        grep -i Intel |
        grep -qv '^0000:00:02\.0' &&
        use_xpu=1 || use_xpu=0
    ;;
*) echo "USE_XPU must be auto, 1, or 0" >&2; exit 2 ;;
esac

# TORCH_VERSION optionally pins the downloaded PyTorch/LibTorch version.
torch_requirement=torch
if [[ -n "${TORCH_VERSION:-}" ]]; then
    torch_requirement="torch==${TORCH_VERSION}"
fi

# PyTorch wheel index based on target backend
if [[ "${use_xpu}" == 1 ]]; then
    torch_index=https://download.pytorch.org/whl/xpu
    echo "Intel GPU detected: installing LibTorch and oneAPI runtime"
else
    torch_index=https://download.pytorch.org/whl/cpu
    echo "No Intel GPU detected: installing CPU LibTorch"
fi

# Install PyTorch into a temporary venv to preserve wheel structure
rm -rf "$cache_dir"/*
python3 -m venv "$cache_dir/venv"
"$cache_dir/venv/bin/pip" install --no-cache-dir "$torch_requirement" \
    --index-url "$torch_index"

# Locate site-packages inside venv
site_packages="$("$cache_dir/venv/bin/python" -c 'import site; print(site.getsitepackages()[0])')"

# Extract LibTorch into deps/
mkdir -p "$deps_dir/torch"
rm -rf "$deps_dir/torch"/*
mv "$site_packages/torch/include" "$deps_dir/torch/include"
mv "$site_packages/torch/lib" "$deps_dir/torch/lib"

# Extract oneAPI into deps/ (if applicable)
if [[ "${use_xpu}" == 1 ]]; then
    rm -rf "$deps_dir/oneapi"/*
    mkdir -p "$deps_dir/oneapi/lib" "$deps_dir/oneapi/include"

    # Copy oneAPI/SYCL headers (CL/, sycl/, oneapi/, umf/, etc.), skipping python headers
    if [[ -d "$cache_dir/venv/include" ]]; then
        shopt -s nullglob
        for item in "$cache_dir/venv/include"/*; do
            base_item="$(basename "$item")"
            if [[ "$base_item" != python* ]]; then
                cp -rL "$item" "$deps_dir/oneapi/include/"
            fi
        done
        shopt -u nullglob
    fi

    # Filter only required runtime libraries and their dlopen dependencies
    wanted_libs=(
        # Explicitly linked by CMake
        "libsycl.so*"
        "libOpenCL.so*"
        "libpti_view.so*"
        "libmkl_sycl_blas.so*"
        "libmkl_sycl_dft.so*"
        "libmkl_sycl_lapack.so*"
        "libmkl_intel_lp64.so*"
        "libmkl_core.so*"
        "libmkl_gnu_thread.so*"

        # Hardware abstraction and runtime loaders
        "libur_loader.so*"
        "libur_adapter_level_zero*.so*"
        "libur_adapter_opencl.so*"
        "libxptifw.so*"
        "libiomp5.so*"
        "libtbb.so*"

        # Intel compiler runtimes (needed by libOpenCL.so for _intel_fast_mem*)
        "libimf.so*"
        "libsvml.so*"
        "libintlc.so*"
        "libirng.so*"

        # oneCCL runtime (needed by libtorch_xpu.so)
        "libccl.so*"

        # MKL dispatch kernels
        "libmkl_def.so*"
        "libmkl_avx*.so*"
    )

    shopt -s nullglob
    for pattern in "${wanted_libs[@]}"; do
        for matched_file in "$cache_dir/venv/lib"/$pattern; do
            [[ -f "$matched_file" || -L "$matched_file" ]] && mv "$matched_file" "$deps_dir/oneapi/lib/"
        done
    done
    shopt -u nullglob

    # Create unversioned .so symlinks for any versioned .so.* binaries
    (
        cd "$deps_dir/oneapi/lib"
        shopt -s nullglob
        for f in *.so.*; do
            base="${f%%.so.*}.so"
            [[ ! -e "$base" ]] && ln -s "$f" "$base"
        done
        shopt -u nullglob
    )
else
    rm -rf "$deps_dir/oneapi"
fi

# nlohmann/json
echo "nlohmann/json..."
rm -rf "$cache_dir/json"
git clone -q --depth 1 --branch v3.12.0 \
    https://github.com/nlohmann/json.git "$cache_dir/json"

mkdir -p "$deps_dir/nlohmann/include/nlohmann"
cp "$cache_dir/json/single_include/nlohmann/json.hpp" \
   "$deps_dir/nlohmann/include/nlohmann/json.hpp"

# sqlite3
echo "sqlite3..."
rm -rf "$cache_dir/sqlite"
git clone -q --depth 1 --branch version-3.50.4 \
    https://github.com/sqlite/sqlite.git "$cache_dir/sqlite"

(
    cd "$cache_dir/sqlite"
    ./configure >/dev/null
    make -s sqlite3.c
)

mkdir -p "$deps_dir/sqlite3/src" "$deps_dir/sqlite3/include/sqlite3"
cp "$cache_dir/sqlite/sqlite3.c" "$deps_dir/sqlite3/src/"
cp "$cache_dir/sqlite/sqlite3.h" "$deps_dir/sqlite3/include/sqlite3/"

# Delete cache
rm -rf "$cache_dir"
