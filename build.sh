#!/bin/bash

mkdir -p shaders_out
./buildShaders.sh
# Compile VMA implementation
g++ -g -Wall -Wextra -std=c++20 -c vma/vma_usage.cpp -o obj/vma_usage.o -I/usr/include -lVulkanMemoryAllocator
# Compile Vulkan application
for basename in main frame; do
    gcc -g -Wall -Wextra -c -o "obj/${basename}.o" "${basename}.c" -I/usr/include/SDL2 -I/usr/include/vulkan -I/usr/include
done
# Link everything
gcc -lstdc++ -o main obj/*.o -L/usr/lib -lSDL2 -lvulkan
