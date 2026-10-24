#!/usr/bin/env bash
# Build only: no driver, SteamVR or headset launch.
set -euo pipefail
checkout="$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
output="${1:?Usage: tests/build_linux_depth_transport.sh NEW_OR_OWN_OUTPUT}"
mkdir -p "$output"
output="$(cd "$output" && pwd)"
compiler="${CXX:-g++}"
"$compiler" -std=c++20 -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
 -Wno-missing-field-initializers -Wno-misleading-indentation \
 -I"$checkout/alvr/server_openvr/cpp" -I"$checkout/alvr/server_openvr/cpp/shared" \
 -I"$checkout/deps/linux/vulkan-headers/include" -I"$checkout/deps/linux/ffmpeg/alvr_build/include" \
 -I"$checkout/openvr/headers" "$checkout/tests/linux_depth_transport.cpp" \
 "$checkout/alvr/server_openvr/cpp/platform/linux/depth/gpu_transport.cpp" \
 -ldl -pthread -o "$output/depth_transport_test"
git -C "$checkout" rev-parse HEAD > "$output/source-commit.txt"
git -C "$checkout" status --short --untracked-files=all > "$output/source-dirty.txt"
(cd "$output" && sha256sum depth_transport_test > checksums.sha256)
printf 'Built %s; no test executed.\n' "$output/depth_transport_test"
