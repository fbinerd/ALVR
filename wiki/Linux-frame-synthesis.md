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
offscreen checker; it has not yet executed this revised path.

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
offscreen NVIDIA integration check, with opposite translations in each eye.
It writes previous/latest/expected/predicted PPM images and per-eye errors;
both predicted errors must improve over repeating the newest real frame.
It does not open SteamVR, ALVR or a headset app. Notify the user before running
tests. GPU and live testing are pending a user-confirmed Luna handoff.

```bash
ALVR_NATIVE_MOTION=1 ALVR_VULKAN_DEVICE=NVIDIA \
  timeout 60 /path/to/ignored/output/image_test /path/to/new/results
```

A passing fixture is required before a headset trial. It cannot validate
driver texture synchronization, real frame cadence, pose accuracy, image
quality or headset latency. A matching-source Android client must be prepared
before testing the ALVR client path; Air Link must never open that client app.

## Verification record

**Fact:** the two shaders compile for Vulkan 1.2. The backend, native-motion
implementation and encoder header passed C++20 syntax compilation with local
ALVR Vulkan/FFmpeg headers. The CPU timestamp eligibility check passed on
2026-10-21. Runtime optical-flow execution and headset output remain unknown.
The RTX 3060 is present again; persistent Render GPU and Encoder GPU selectors
were set to NVIDIA with a session backup. No ALVR/SteamVR launch or GPU test
was performed during this implementation.

**Primary API reference:** [Khronos Vulkan `VK_NV_optical_flow`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_optical_flow.html)
and [optical-flow queue and image requirements](https://docs.vulkan.org/spec/latest/chapters/VK_NV_optical_flow/optical_flow.html).
