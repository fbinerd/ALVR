#!/usr/bin/env bash
# Compile CPU and offscreen GPU checks; execute neither one.
set -euo pipefail
checkout="$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
motion_output="${1:?Usage: tests/build_linux_native_motion.sh OUTPUT_DIRECTORY}"
mkdir -p "$motion_output"
motion_output="$(cd "$motion_output" && pwd)"
native_source="$checkout/alvr/server_openvr/cpp/platform/linux"
shader_compiler="${GLSLANG_VALIDATOR:-glslangValidator}"
for shader in luma warp; do
    "$shader_compiler" -V --target-env vulkan1.2 \
        -o "$motion_output/motion_$shader.spv" "$native_source/shader/motion_$shader.comp"
done
python3 - "$motion_output" <<'PY'
import pathlib
import struct
import sys

output = pathlib.Path(sys.argv[1])
header = '#pragma once\n#include <cstdint>\nnamespace alvr::motion::shader {\n'
for name in ('luma', 'warp'):
    binary = (output / f'motion_{name}.spv').read_bytes()
    words = struct.unpack('<' + 'I' * (len(binary) // 4), binary)
    header += f'inline constexpr uint32_t {name}[]={{'
    header += ','.join(f'0x{word:08x}' for word in words) + '};\n'
(output / 'NativeMotionShaders.hpp').write_text(header + '}\n')
PY
compiler="${CXX:-g++}"
includes=("-I$checkout/alvr/server_openvr/cpp" "-I$checkout/alvr/server_openvr/cpp/shared"
    "-I$checkout/deps/linux/vulkan-headers/include" "-I$checkout/deps/linux/ffmpeg/alvr_build/include"
    "-I$checkout/openvr/headers" "-I$motion_output")
"$compiler" -std=c++20 -O2 -Wall -Wextra -Werror "${includes[@]}" \
    "$checkout/tests/linux_native_motion_timing.cpp" -o "$motion_output/timing_test"
"$compiler" -std=c++20 -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter \
    "${includes[@]}" "$checkout/tests/linux_native_motion_images.cpp" \
    "$native_source/motion/NativeMotion.cpp" "$native_source/motion/NativeOpticalFlow.cpp" \
    -Wl,-l:libvulkan.so.1 -pthread -o "$motion_output/image_test"
printf 'Built checks in %s; no tests executed.\n' "$motion_output"
