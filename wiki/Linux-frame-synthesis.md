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
It extracts half-resolution luma separately for each eye and estimates both
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

Before resampling, the shader compares previous/latest color at the current
texel and at the actual proposed source. Identity at both sites preserves the
exact current texel. For valid source motion up to one pixel per real-frame
interval and a color change of at most 16/255 per channel, it instead predicts
`latest + horizon * (latest - previous)`, clamped to the color range. This
first-order temporal approximation avoids spatial filtering of subpixel
detail; larger motion/changes use the spatial warp. It can amplify noise or
overshoot changing edges. Both changes await GPU validation, including slow
horizontal, vertical and diagonal fixtures at both resolutions.

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
| `quality` | 512x384 | 6 | Stationary, translation, subpixel horizontal/vertical/diagonal translation, fine texture, rotation, foreground occlusion, timestamp jitter |
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
and temporal subpixel approximation described above are pending GPU tests.
Headset output, pose association and full-resolution budget remain unknown.
The RTX 3060 is present again; persistent Render GPU and Encoder GPU selectors
were set to NVIDIA with a session backup. No ALVR/SteamVR launch occurred
during implementation of the expanded checker.

**Primary API reference:** [Khronos Vulkan `VK_NV_optical_flow`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_optical_flow.html)
and [optical-flow queue and image requirements](https://docs.vulkan.org/spec/latest/chapters/VK_NV_optical_flow/optical_flow.html).
