#!/usr/bin/env bash
# Offline-only build. The optical estimator stub is linked ONLY here.
set -euo pipefail
checkout="$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
output="${1:?Usage: build_linux_depth_capture.sh OUTPUT}"
mkdir -p "$output"
output="$(cd "$output" && pwd)"
linux="$checkout/alvr/server_openvr/cpp/platform/linux"
for shader in luma warp; do
 "${GLSLANG_VALIDATOR:-glslangValidator}" -V --target-env vulkan1.2 -o "$output/motion_$shader.spv" "$linux/shader/motion_$shader.comp"
done
python3 - "$output" <<'PY'
import pathlib,struct,sys
p=pathlib.Path(sys.argv[1]);header='#pragma once\n#include <cstdint>\nnamespace alvr::motion::shader {\n'
for name in ('luma','warp'):
 b=(p/f'motion_{name}.spv').read_bytes()
 header+=f'inline constexpr uint32_t {name}[]={{'+','.join(hex(v[0]) for v in struct.iter_unpack('<I',b))+'};\n'
(p/'NativeMotionShaders.hpp').write_text(header+'}\n')
PY
includes=("-I$checkout/alvr/server_openvr/cpp" "-I$checkout/alvr/server_openvr/cpp/shared"
 "-I$checkout/deps/linux/vulkan-headers/include" "-I$checkout/deps/linux/ffmpeg/alvr_build/include"
 "-I$checkout/openvr/headers" "-I$output")
"${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers -Wno-misleading-indentation \
 "${includes[@]}" "$checkout/tests/linux_depth_capture.cpp" "$checkout/tests/depth_capture_optical_stub.cpp" \
 "$linux/motion/NativeMotion.cpp" "$linux/depth/gpu_transport.cpp" \
 -Wl,-l:libvulkan.so.1 -ldl -pthread -o "$output/depth_capture_test"
printf 'Built capture-only executable; no live application launched.\n'
