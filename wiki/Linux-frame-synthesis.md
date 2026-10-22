# Linux frame synthesis status

## Current path

Linux ALVR receives SteamVR's composed eye images in the direct-mode driver,
composites layers in `platform/linux/Renderer.cpp`, and encodes the resulting
Vulkan image from `Encoder.hpp`. A frame is submitted to the encoder once per
driver presentation. Reprojection metadata and frame timestamps are carried
by the driver. The default path submits only real composed frames.

## Optional optical-flow device setup

`VK_NV_optical_flow` setup is available behind the environment variable
`ALVR_NATIVE_MOTION=1`. It is off by default. When requested, `VkContext`
checks the selected render device for `VK_NV_optical_flow`, its required
format-feature and synchronization2 extensions, the optical-flow feature,
and a queue family supporting optical flow and transfers. It enables the
features and creates/gets a queue only if every check succeeds. Otherwise it
logs a warning and starts the existing path without optical flow. The device
must also support extended storage-image formats and storage writes without
format, used for R8 luma and the existing RGBA/BGRA encoder input.

This uses the Vulkan GPU selected as the **render GPU**. SteamVR's imported
textures must be on that same GPU. Choosing another encoder GPU does not move
the optical-flow work; the existing encoder handoff is downstream. On this
host, the tested implementation is NVIDIA-specific. The Intel GPU does not
provide this NV extension.

## Controlled synthesis prototype

`ALVR_NATIVE_MOTION=1` now also requests a controlled synthesis prototype.
Requirements: NVIDIA render GPU, same-GPU NVENC, foveated encoding disabled,
and an RGBA8/BGRA8 output with formatless storage-write support. Select
**NVIDIA GeForce RTX 3060** for both Render GPU and Encoder GPU on this host.
Encoder technology Automatic resolves to NVENC. SteamVR must render on the
same GPU; an Intel-only Steam environment can defeat the selection.

The encoder worker retains every other composed frame in owned GPU memory.
It extracts luma separately for each eye, at most half the output dimensions,
with its largest dimension capped at 512. The ratio is preserved with dimensions
rounded down to multiples of four. For 2048x2144 output per eye the estimator
uses 488x512 luma instead of 1024x1072. Color/output resolution is unchanged.
Larger downsampling footprints use four color samples to reduce aliasing.
The estimator still computes both
motion directions using the borrowed-device backend from the research repo.
On an intervening presentation, a shader inverse-warps the latest color
image forward using backward motion and forward/backward consistency checks.
Pixels with invalid motion repeat the newest pixel. The eye boundaries are
clamped independently. The output goes through the existing single NVENC
submission and timestamp channel at the existing presentation cadence.

Fallback pixels are fetched exactly. The warp solves the four corners of an
8x8 output tile once. When all corners pass the existing consistency checks
and their displacements vary by at most 0.5 output pixels, it interpolates
the displacement across that tile. Motion boundaries, invalid corners and
partial/seam tiles retain per-pixel inversion. This reduces repeated flow
sampling for smooth motion; performance and boundary quality need GPU tests.

Each inverse solve uses at most five updates. It now stops when the next
source coordinate equals the current coordinate exactly, reusing the flow
already sampled there for the final consistency check. Repeating deterministic
samples at that coordinate would give the same update. There is no convergence
tolerance and no change to the residual or bidirectional thresholds. GPU
validation is pending; divergent lanes and pipeline compilation may affect
the cost, so this is not a measured speedup.

The offscreen checker's `--fixed-flow-iterations` flag disables that exit via
a shader specialization constant. It keeps five updates and the final flow
sample, providing a control in the same executable. Recovery checks use the
selected variant too. Compare both variants at the same `--luma-max 512`,
require unchanged image-quality gates and exact stationary output, and retain
device/wall timings before attributing any speedup to this change.

Before resampling, the shader compares previous/latest color at the current
texel and at the actual proposed source. Identity at both sites preserves the
exact current texel. Subpixel prediction no longer depends on a one-pixel
threshold applied to the coarse luma vectors. For each full 8x8 tile, 16 RGB
history samples form a regularized local translation fit using symmetric
spatial gradients. The fit must explain at least 75% of temporal squared
change, have sufficient texture/change, and estimate at most one output pixel
per real-frame interval. For accepted tiles, pixels changing by at most 16/255
per channel use `latest + horizon * (latest - previous)`, clamped to the color
range. Other pixels use the spatial warp. Accepted tiles skip the four corner
flow solves; all workgroup barriers precede any pixel return. This is an
untested candidate for the full-size slow-motion regression under capped luma.
It can still amplify noise, misclassify mixed/ambiguous motion, or overshoot
changing edges; GPU quality/cost checks are required before live use.
If the fit rejects motion and all 16 samples have the same nonzero RGB change,
pixels matching that change keep the exact latest color. This protects a
stationary texture during a uniform color flash from false coarse-flow warp;
pixels with a different change still use spatial inversion. The uniformity
threshold is 1.25/255 per channel and is an unvalidated heuristic for mixed
color and geometric motion.

Two retained real frames are required before predicting. A predicted frame
may extend at most 0.55 of the measured real-frame interval beyond the latest
frame; duplicate/reversed timestamps and intervals longer than 100 ms reset
history and use a real frame. Logs include `Native motion predicted=...` and
`Native motion timestamp rejected=...`. The compositor's submitted textures
are not read on prediction presentations and are released before GPU work.
New commands have bounded waits. Runtime GPU failures go through the existing
encoder worker error/idle path; pending resources are not freed without
confirmed completion. Constructor capability/format failures use normal
frames instead.

After a rejected prediction, that fallback real frame re-anchors the capture
cadence. One warm-up presentation is left before retaining the next real
frame. This avoids a recurring rejection when two retained timestamps are
only one presentation apart. GPU recovery coverage is prepared in the
offscreen checker; all four recovery cases passed on the RTX 3060 at `798f39a7`.

This is controlled half-input-rate prediction, not automatic SteamVR app
throttling. At a stable 72 presentations/s it selects 36 real inputs/s and
attempts 36 predictions/s. It does not halve the game's rendering cost,
enable Valve's Motion Smoothing UI, or implement Valve's missing Linux
estimator. There is no depth or game-provided motion input. The prototype
stamps predictions with the current job's tracking timestamp; its association
with the estimated image pose and interaction with rotational rewarp remain
unverified. It must not be described as validated headset Motion Smoothing.

No extra setting is enabled by default. Unsupported modes, cross-GPU encode,
and foveated encoding retain normal frames. Unset `ALVR_NATIVE_MOTION` and
restart SteamVR to disable the experimental path. Remove any
`airlink_test_frame_divisor` file before testing this mode: the old divisor
would remove presentations needed by its controlled cadence.

## Build and checks

Linux streamer builds compile the two GLSL sources with `glslangValidator`
and generate aligned C++ shader arrays in Cargo's `OUT_DIR`; generated SPIR-V
is not committed. The Linux Docker build image installs `glslang-tools`.
Windows and macOS do not compile these shaders or the native backend.

Build isolated checks without executing either one:

```bash
bash tests/build_linux_native_motion.sh /path/to/ignored/output
```

`timing_test` is CPU-only and checks timestamp eligibility, including
nonmonotonic values, long pauses and overflow boundaries. `image_test` is an
offscreen NVIDIA integration checker. Its analytic reference renders the
target time directly, independently of the predictor. Suites:

| Suite | Per-eye size | Predictions per case | Cases |
| --- | --- | --- | --- |
| `smoke` (default) | 512x384 | 1 | Opposite translations |
| `quality` | 512x384 | 6 | Stationary, translation, subpixel horizontal/vertical/diagonal translation, fine texture, rotation, foreground occlusion, timestamp jitter, stationary color flash |
| `stream` | 2048x2144 | 12 | Same cases at representative stream dimensions |

`quality` and `stream` also verify four timestamp rejection/recovery cases
against the actual predictor: duplicate, reversed, excessive horizon and long
pause. A rejected call must preserve output and retain the real input; a
successful prediction must release that input exactly once. Every output
pixel is checked for opaque alpha and for the eye's distinct color tint,
including the seam. Image-quality gates require improvement in both the whole
interior and the changed-pixel region of each eye. Stationary images require
exact RGBA preservation at every pixel including edges; the CSV reports
`static_pixel_mismatches`. Subpixel translation advances 0.25 output pixels
per presentation along each selected axis at both resolutions, covering
possible slow-motion freezes or directional bias.
The color-flash control changes chroma every two presentations without moving
geometry; the target retains the latest color. Prediction must stay within
the existing 0.25 RGB MAE gate when repeating the latest is exact. This guards
against treating every small color change as predictable motion.
Newly revealed foreground-background pixels are reported
separately; that region is diagnostic, not independently gated, because its
visibility is not available from latest color alone.

Results include `metrics.csv` (eye/region errors), `timings.csv` (each sample),
and `summary.txt` (quality result and p50/p95/max). Timings are CPU wall time
including GPU completion, excluding fixture upload/readback and encoding.
The first sample is marked cold and excluded from steady percentiles when
more samples exist. Warm capture timing and capture+prediction pair timing
are also recorded. These are not end-to-end frame latency or paced 72 Hz
measurements. The fixture runs unpaced; desktop GPU load can affect results.
The predictor also reports separate flow-estimation and warp-submission CPU
wall durations (both include their completion waits). These are not Vulkan
timestamp queries and can include CPU scheduling or other queued GPU work;
their percentile maxima need not occur in the same sample.
Optional queue timestamp queries additionally report device durations. The
optical duration sums four optical-queue runs including their packed-output
copies; it excludes input and final copies on the render queue. Warp duration
covers render-queue barriers and compute, excluding CPU recording/submission.
An unsupported queue or unavailable result reports -1 with zero summary
samples, rather than inventing a zero-cost operation. Both intervals can
include device scheduling/preemption; neither accounts for encoding/display.

The checker precomputes reference images, executes an unpaced GPU
sequence, then scores/writes images and CSVs. Host batch memory is bounded
at 1.5 GiB of cached pixel payload per scene; other allocations are additional.
Larger iteration counts require smaller dimensions.
Uploads/readbacks remain between samples but outside prediction timing.
This avoids CPU reference/scoring gaps lowering device clocks between
predictions. It is a throughput check, not a paced or game-loaded deadline
test. Prior results with per-sample CPU work are not directly comparable.

Commit before the final check build. The builder records source revision,
tracked dirtiness and binary/shader hashes. The runner refuses dirty build
metadata or reused output directories and preserves failure/timeout logs.
When available, `nvidia-smi` records read-only utilization, memory, graphics
clock and power telemetry in `gpu-telemetry.csv` during execution, alongside
wall-clock start/end files. Sampler errors are kept separately; telemetry
does not isolate the GPU or measure per-dispatch device time.
It does not open SteamVR, ALVR or a headset app. Announce every test first;
GPU execution requires the user-confirmed Luna handoff.

```bash
bash tests/run_linux_native_motion.sh /path/to/ignored/output \
  /path/to/new/quality-results --suite quality --save-images
# Only after quality passes, measure representative resolution. The explicit
# warm-prediction limit is one 72 Hz slot; encoding/capture still need budget.
bash tests/run_linux_native_motion.sh /path/to/ignored/output \
  /path/to/new/stream-results --suite stream --max-predict-ms 13.888889
```

Use `--luma-max 0` to retain the original half-resolution estimator as a
same-binary timing/quality baseline. The default is 512; other caps must be
multiples of four in [64,2048]. Small fixtures below the cap are unchanged.
Compare 0 versus 512 under this same burst protocol; prior interleaved-CPU
timings cannot establish the speedup. A lower cap can lose fine motion or
disocclusion detail, so all nine quality gates still apply.

Use `--iterations N` to choose 1-120 samples per case. `--save-images` writes
the first sample's previous/latest/expected/predicted PPMs in case folders;
it is on by default only for `smoke`. At stream dimensions, these files can
consume about 950 MB across nine cases. The runner has a bounded 300-second
timeout. A partial run or failure lacks a successful final `passed=1`;
inspect `exit-status.txt` and the log before interpreting metrics.

A passing fixture is required before a headset trial. It cannot validate
driver texture synchronization, real frame cadence, pose accuracy, image
quality or headset latency. A matching-source Android client must be prepared
before testing the ALVR client path; Air Link must never open that client app.

### Paired exact-exit comparison

Separate processes showed large GPU-clock variation even in the unchanged
optical-flow stage. Use `--compare-flow-iterations` to run the fixed-iteration
control and exact-exit candidate in one process, on the same references and
timestamps. Each owns independent images, history and optical-flow sessions.
The order alternates fixed/exact, exact/fixed per sample. `--candidate-first`
reverses every pair for a second run. GPU commands remain synchronous; this
mode does not measure concurrent execution or isolate other desktop GPU work.

`--warmup-samples N` excludes a prefix from timing summaries (default 1),
while retaining its image, input-release and exact-output checks. At least
one measured sample must remain; a one-sample smoke run reports its cold
sample. The paired stream default is ten samples instead of twelve so that
two predictions plus the shared references fit the existing 1.5 GiB pixel
cache cap. Other CPU/GPU allocations are additional. All ten scenes and the
four timestamp-recovery cases run for each variant. A difference in any RGBA
byte in any sample fails the comparison, even if both quality gates pass.

Metrics and timings append a `variant` column (`fixed` or `exact`). Timings
also append `pair_position`, `start_unix_ns` (prediction start for telemetry
correlation) and `warmup`. CSV rows are grouped by variant, so use sample and
position to reconstruct execution order. `comparison_case` summary lines
report exact-byte agreement and paired candidate-minus-control time deltas:
negative is faster for exact exit. Order-specific counts/medians expose order
bias; an empty group has count zero and its reported median is not usable.
Saved PPMs are placed in `fixed/<scene>/` and `exact/<scene>/`.

The optional budget still gates the **maximum** measured prediction time,
not just its percentile. With the small sample counts used so far, p95 and
maximum coincide. Do not relax the deadline when switching protocols or
claim game/stream readiness from a paired throughput pass. Two resident
histories and alternating variants may affect cache residency and clocks;
paired timing reduces long gaps but does not prove stable device frequency.

After the user-confirmed Luna handoff, run these in fresh result directories:

```bash
bash tests/run_linux_native_motion.sh /path/to/checker /path/to/paired-quality \
  --suite quality --compare-flow-iterations --warmup-samples 2 --luma-max 512 --save-images
# Require 10/10 quality, exact agreement for every sample and 4/4 recoveries
# for each variant before full size. Then run two opposite starting orders:
bash tests/run_linux_native_motion.sh /path/to/checker /path/to/paired-stream-ab \
  --suite stream --compare-flow-iterations --warmup-samples 2 --luma-max 512 \
  --max-predict-ms 13.888889 --save-images
bash tests/run_linux_native_motion.sh /path/to/checker /path/to/paired-stream-ba \
  --suite stream --compare-flow-iterations --warmup-samples 2 --luma-max 512 \
  --max-predict-ms 13.888889 --save-images --candidate-first
```

A timing-only failure permits the opposite-order comparison. Stop on any
quality, release, recovery or equality failure. Keep logs and telemetry for
all runs. No ALVR, SteamVR or Quest process is involved in this protocol.

### Experimental bidirectional estimator

`--compare-bidirectional-flow` compares the existing two separate directions
against a session with `VK_OPTICAL_FLOW_SESSION_CREATE_BOTH_DIRECTIONS_BIT_NV`.
Both variants keep exact flow exit enabled, the same luma cap and all image
thresholds. They are labeled `separate` and `bidirectional` in metrics/images.
This mode is mutually exclusive with `--compare-flow-iterations` and
`--fixed-flow-iterations`. The same alternating order, warm-up accounting,
memory cap and byte-equality gate apply. Comparison summary keys now use
`candidate_wins`, `candidate_minus_control_p50_ms`, `control_first_*` and
`candidate_first_*` for either kind of comparison.

The experimental session requires `bidirectionalFlowSupported`, allocates
backward vector/cost images and buffers, and binds the backward output slots.
It copies both directions back through the same semaphore/fence chain.
There are two execute chains per stereo pair instead of four; device work
does not necessarily halve. All outputs are checked for aliases and legal
usage/layout, and their metadata changes only after successful completion.
Existing single-direction API calls retain their default behavior. The
`NativeMotion` constructor's bidirectional argument defaults to false; the
encoder selects it only when `ALVR_NATIVE_MOTION_BIDIRECTIONAL=1`, alongside
the existing `ALVR_NATIVE_MOTION=1`. Both must be inherited by vrserver at
startup. Unsupported bidirectional hardware logs the capability rejection
and retains separate-direction synthesis. Initialization/GPU failures use
the existing normal-frame fallback. The flag alone does not enable native
motion, change the selected GPU, or select virtual display. No FFI or settings
layout changes are involved.

The API's [session flags](https://docs.vulkan.org/refpages/latest/refpages/source/VkOpticalFlowSessionCreateFlagBitsNV.html)
require backward vector output and, when cost generation is enabled, backward
cost output. Unsupported devices reject this test explicitly. Paired quality
must pass before attempting stream-size timings with both initial orders.

```bash
bash tests/run_linux_native_motion.sh /path/to/checker /path/to/bidir-quality \
  --suite quality --compare-bidirectional-flow --warmup-samples 2 --luma-max 512 --save-images
```

Do not infer stream readiness or output equivalence from compilation alone.

## Verification record

**Fact:** the two shaders compile for Vulkan 1.2. The backend, native-motion
implementation and encoder header passed C++20 syntax compilation with local
ALVR Vulkan/FFmpeg headers. The CPU timestamp eligibility check passed on
2026-10-21. The first smoke fixture passed on RTX 3060 at source `bf103800`:
left/right repeated MAE 18.5085/18.4292, predicted MAE 0.696263/0.541704,
8.45 ms flow+warp at 512x384 per eye. That result predates the cadence recovery
fix and expanded checker. The expanded suite at `6e2ce564` passed the five
moving scenes and all four timestamp recovery contracts, but failed stationary
images: predicted MAE 0.563584/0.633863 versus exact repetition. The identity
patch guard at `798f39a7` passed all seven small-image cases with exact static
pixels. At 2048x2144 per eye it preserved stationary pixels but failed slow
translation: predicted MAE 1.52871/1.59969 versus repeated 1.3463/1.3575.
Warm prediction p95 ranged 17.398–37.283 ms under uncontrolled desktop load,
exceeding the 13.888889 ms test gate. The tile solver, cheaper identity check
and temporal subpixel approximation at `f56f9221` passed all nine image scenes
at both sizes. Its stream prediction p95 still ranged 17.289–33.296 ms under
variable clocks. The new luma cap/filter, burst measurement and device-query
instrumentation are pending GPU tests. Headset output, pose association and
full-resolution budget remain unknown.
The RTX 3060 is present again; persistent Render GPU and Encoder GPU selectors
were set to NVIDIA with a session backup. No ALVR/SteamVR launch occurred
during implementation of the expanded checker.

**Primary API reference:** [Khronos Vulkan `VK_NV_optical_flow`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_optical_flow.html)
and [optical-flow queue and image requirements](https://docs.vulkan.org/spec/latest/chapters/VK_NV_optical_flow/optical_flow.html).
Timestamp queries follow the queue capability and stage requirements of
[`vkCmdWriteTimestamp2`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdWriteTimestamp2.html)
and [`vkCmdResetQueryPool`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdResetQueryPool.html).
