#!/usr/bin/env bash

set -e

cd ..

# clean build directory
make clean

# make the bootroms
# requires clang and not emcc
make bootroms

cd -

cd ../libretro/
# make wasm libretro core object files
emmake make

# copy every interesting objs into one place
cd -
mkdir -p ./obj
cp ../build/libretro/*.o ./obj
cp ../build/Core/*.o ./obj

mkdir -p ./bootroms
cp ../build/bin/BootROMs/*.bin ./bootroms

# link statically the object files
mkdir -p ./build
emcc ./obj/* -o ./build/libretro.html -s WASM=1 -s LINKABLE=1 -s EXPORT_ALL=1 -s RESERVED_FUNCTION_POINTERS=256 -s NO_EXIT_RUNTIME=0 -O2


