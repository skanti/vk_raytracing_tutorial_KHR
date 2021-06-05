#!/bin/bash


cd build
make -j 8
ret=$?
if [ $ret -eq 0 ]; then
  ../../bin_x64/Release/vk_ray_tracing_gltf_KHR.exe
fi
