#!/bin/bash

mkdir -p shaders_out
./buildShaders.sh
gcc -g -Wall -Wextra -o main main.c -I/usr/include/SDL2 -L/usr/lib -lSDL2 -I/usr/include/vulkan -lvulkan -I/usr/include -lVulkanMemoryAllocator
