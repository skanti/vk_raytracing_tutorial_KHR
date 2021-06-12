#!/bin/bash


cd build

# cmake
cmake ..
ret=$?
if [ $ret -ne 0 ]; then
  exit 1
fi

# build
make -j 8
ret=$?
if [ $ret -ne 0 ]; then
  exit 1
fi
../../bin_x64/Release/vk_ray_tracing_gltf_KHR.exe
