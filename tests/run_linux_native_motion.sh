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
# Read-only telemetry helps distinguish variable desktop load/clocks from
# predictor cost. It does not isolate the device or alter GPU settings.
motion_sampler=""
motion_stop_sampler() {
    if [[ -n "$motion_sampler" ]]; then
        kill "$motion_sampler" 2>/dev/null || true
        wait "$motion_sampler" 2>/dev/null || true
        motion_sampler=""
    fi
}
trap motion_stop_sampler EXIT
if command -v nvidia-smi >/dev/null 2>&1; then
    timeout --kill-after=2 305 nvidia-smi \
        --query-gpu=timestamp,index,name,utilization.gpu,memory.used,clocks.current.graphics,power.draw \
        --format=csv --loop-ms=100 > "$motion_results/gpu-telemetry.csv" 2> "$motion_results/gpu-telemetry-error.txt" &
    motion_sampler=$!
fi
date --iso-8601=ns > "$motion_results/start-time.txt"
# Keep the result even when quality/budget checks fail or the process times out.
motion_status=0
ALVR_NATIVE_MOTION=1 ALVR_VULKAN_DEVICE=NVIDIA timeout --kill-after=5 300 \
    "$motion_build/image_test" "$motion_results" "$@" > "$motion_results/run.log" 2>&1 \
    || motion_status=$?
date --iso-8601=ns > "$motion_results/end-time.txt"
motion_stop_sampler
printf '%s\n' "$motion_status" > "$motion_results/exit-status.txt"
cat "$motion_results/run.log"
exit "$motion_status"
