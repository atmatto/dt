cd shaders
for f in *; do
    glslc $f -o "../shaders_out/${f}.spv"
    xxd -n "$f" -i "../shaders_out/${f}.spv" "../shaders_out/${f}.h"
done
