#!/usr/bin/env bash
set -euo pipefail

cd -- "$(dirname -- "$0")"
if [[ ${MSYSTEM:-} != UCRT64 && ${MSYSTEM:-} != MINGW64 ]]; then
    printf '%s\n' 'Abre MSYS2 UCRT64 o MINGW64 para compilar la DLL de Windows.' >&2
    exit 1
fi
for program in gcc g++ git cmake ninja; do
    if ! command -v "$program" >/dev/null; then
        printf 'Falta %s. Consulta LEEME.txt para instalarlo.\n' "$program" >&2
        exit 1
    fi
done

build_dir="build-${MSYSTEM,,}"
cmake -S . -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_OPENGL=OFF -DENABLE_JIT=ON -DENABLE_LTO_RELEASE=OFF
cmake --build "$build_dir" --parallel

printf '\nDLL: %s\nINFO: %s\n' \
    "$PWD/$build_dir/src/libretro/melondsds_splitscreen_libretro.dll" \
    "$PWD/$build_dir/melondsds_splitscreen_libretro.info"
