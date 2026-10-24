#!/usr/bin/env bash
# Compile the offline stereo depth check; execute nothing.
set -euo pipefail
checkout="$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
out="${1:?Usage: tests/build_linux_stereo_depth.sh OUTPUT_DIRECTORY}"
mkdir -p "$out"; out="$(cd "$out" && pwd)"
native="$checkout/alvr/server_openvr/cpp/platform/linux"
for shader in stereo_luma stereo_depth stereo_fill stereo_merge; do
    "${GLSLANG_VALIDATOR:-glslangValidator}" -V --target-env vulkan1.2 -o "$out/$shader.spv" "$native/shader/$shader.comp"
done
python3 - "$out" <<'PY'
import pathlib, struct, sys
out = pathlib.Path(sys.argv[1])
header = '#pragma once\n#include <cstdint>\nnamespace alvr::motion::shader {\n'
for name in ('stereo_luma', 'stereo_depth', 'stereo_fill', 'stereo_merge'):
    data = (out / f'{name}.spv').read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    header += f'inline constexpr uint32_t {name}[]={{' + ','.join(f'0x{w:08x}' for w in words) + '};\n'
(out / 'StereoDepthShaders.hpp').write_text(header + '}\n')
PY
includes=("-I$checkout/alvr/server_openvr/cpp" "-I$checkout/alvr/server_openvr/cpp/shared"
    "-I$checkout/deps/linux/vulkan-headers/include" "-I$checkout/deps/linux/ffmpeg/alvr_build/include"
    "-I$checkout/openvr/headers" "-I$out")
"${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter \
    "${includes[@]}" "$checkout/tests/linux_stereo_depth.cpp" \
    "$native/motion/StereoDepth.cpp" "$native/motion/NativeOpticalFlow.cpp" \
    -Wl,-l:libvulkan.so.1 -pthread -o "$out/stereo_depth_test"
printf 'Built %s/stereo_depth_test; no tests executed.\n' "$out"
