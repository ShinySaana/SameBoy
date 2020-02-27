#!/usr/bin/env bash

set -e

cd ../libretro/

make clean
emmake make

cd -

mkdir -p ./so
cp ../build/libretro/* ./so


mkdir -p ./build
emcc ./so/* -o ./build/libretro.html -s WASM=1 -s LINKABLE=1 -s EXPORT_ALL=1 -s RESERVED_FUNCTION_POINTERS=256 -s NO_EXIT_RUNTIME=0

cd ./build

cd -

