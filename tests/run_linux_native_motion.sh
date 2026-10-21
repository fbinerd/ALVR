#!/usr/bin/env bash
# Offscreen GPU execution only. Announce the test and confirm the test model first.
set -euo pipefail
motion_build="${1:?Usage: tests/run_linux_native_motion.sh BUILD_DIRECTORY NEW_RESULTS_DIRECTORY [image_test options]}"
motion_results="${2:?Results directory is required}"
shift 2
motion_build="$(cd "$motion_build" && pwd)"
for motion_file in source-commit.txt source-dirty.txt checksums.sha256; do
    if [[ ! -f "$motion_build/$motion_file" ]]; then
        printf 'Missing build metadata: %s\n' "$motion_build/$motion_file" >&2
        exit 2
    fi
done
if [[ -e "$motion_results" ]]; then
    printf 'Use a new results directory: %s\n' "$motion_results" >&2
    exit 2
fi
if [[ -s "$motion_build/source-dirty.txt" ]]; then
    printf 'Refusing a build from tracked source changes; commit and rebuild first.\n' >&2
    exit 2
fi
(
    cd "$motion_build"
    sha256sum --check checksums.sha256
)
mkdir -p "$motion_results"
motion_results="$(cd "$motion_results" && pwd)"
cp "$motion_build/source-commit.txt" "$motion_build/checksums.sha256" "$motion_results/"
printf 'Offscreen GPU test; results: %s\n' "$motion_results"
printf '%q ' "$motion_build/image_test" "$motion_results" "$@" > "$motion_results/command.txt"
printf '\n' >> "$motion_results/command.txt"
# Keep the result even when quality/budget checks fail or the process times out.
motion_status=0
ALVR_NATIVE_MOTION=1 ALVR_VULKAN_DEVICE=NVIDIA timeout --kill-after=5 300 \
    "$motion_build/image_test" "$motion_results" "$@" > "$motion_results/run.log" 2>&1 \
    || motion_status=$?
printf '%s\n' "$motion_status" > "$motion_results/exit-status.txt"
cat "$motion_results/run.log"
exit "$motion_status"
