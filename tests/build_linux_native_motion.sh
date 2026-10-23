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
# The depth check can estimate depth from the stereo pair.
for shader in stereo_luma stereo_depth stereo_fill; do
    "$shader_compiler" -V --target-env vulkan1.2 \
        -o "$motion_output/$shader.spv" "$native_source/shader/$shader.comp"
done
python3 - "$motion_output" <<'PY'
import pathlib, struct, sys
output = pathlib.Path(sys.argv[1])
header = '#pragma once\n#include <cstdint>\nnamespace alvr::motion::shader {\n'
for name in ('stereo_luma', 'stereo_depth', 'stereo_fill'):
    words = struct.unpack('<' + 'I' * ((output / f'{name}.spv').stat().st_size // 4), (output / f'{name}.spv').read_bytes())
    header += f'inline constexpr uint32_t {name}[]={{' + ','.join(f'0x{w:08x}' for w in words) + '};\n'
(output / 'StereoDepthShaders.hpp').write_text(header + '}\n')
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
"$compiler" -std=c++20 -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter \
    "${includes[@]}" "$checkout/tests/linux_native_motion_recording.cpp" \
    "$native_source/motion/NativeMotion.cpp" "$native_source/motion/NativeOpticalFlow.cpp" \
    -Wl,-l:libvulkan.so.1 -pthread -o "$motion_output/recording_test"
"$compiler" -std=c++20 -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter \
    -DALVR_NATIVE_MOTION_TESTING "${includes[@]}" "$checkout/tests/linux_native_motion_rotation.cpp" \
    "$native_source/motion/NativeMotion.cpp" "$native_source/motion/NativeOpticalFlow.cpp" \
    -Wl,-l:libvulkan.so.1 -pthread -o "$motion_output/rotation_test"
"$compiler" -std=c++20 -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter \
    "${includes[@]}" "$checkout/tests/linux_native_motion_depth.cpp" -DALVR_NATIVE_MOTION_TESTING \
    "$native_source/motion/NativeMotion.cpp" "$native_source/motion/NativeOpticalFlow.cpp" \
    "$native_source/motion/StereoDepth.cpp" \
    -Wl,-l:libvulkan.so.1 -pthread -o "$motion_output/depth_test"
git -C "$checkout" rev-parse HEAD > "$motion_output/source-commit.txt"
git -C "$checkout" status --short --untracked-files=no > "$motion_output/source-dirty.txt"
(
    cd "$motion_output"
    sha256sum recording_test depth_test image_test timing_test rotation_test motion_luma.spv motion_warp.spv NativeMotionShaders.hpp \
        > checksums.sha256
)
printf 'Built checks in %s; no tests executed.\n' "$motion_output"
