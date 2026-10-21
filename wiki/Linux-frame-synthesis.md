# Linux frame synthesis status

## Current path

Linux ALVR receives SteamVR's composed eye images in the direct-mode driver,
composites layers in `platform/linux/Renderer.cpp`, and encodes the resulting
Vulkan image from `Encoder.hpp`. A frame is submitted to the encoder once per
driver presentation. Reprojection metadata and frame timestamps are carried
by the driver, but ALVR does not currently synthesize an additional image.

## Optional optical-flow device setup

`VK_NV_optical_flow` setup is available behind the environment variable
`ALVR_NATIVE_MOTION=1`. It is off by default. When requested, `VkContext`
checks the selected render device for `VK_NV_optical_flow`, its required
format-feature and synchronization2 extensions, the optical-flow feature,
and a queue family supporting optical flow and transfers. It enables the
features and creates/gets a queue only if every check succeeds. Otherwise it
logs a warning and starts the existing path without optical flow.

This uses the Vulkan GPU selected as the **render GPU**. SteamVR's imported
textures must be on that same GPU. Choosing another encoder GPU does not move
the optical-flow work; the existing encoder handoff is downstream. On this
host, the tested implementation is NVIDIA-specific. The Intel GPU does not
provide this NV extension.

## Limits

This change only negotiates the extension, features and queue. No frame
history, image conversion, flow execution, interpolation pass, frame pacing,
timestamp assignment or second encoder submission is connected. Setting
`ALVR_NATIVE_MOTION=1` therefore does **not** enable Motion Smoothing or
change the output video today. Do not use it as a user-facing setting.

The existing standalone backend and synthesis reference are in the separate
MetaHorizonLinux research repository, not linked into this ALVR branch yet.
The next implementation step is to retain two composed frames on the render
device, convert them to the optical-flow input format, run flow in both
directions, and feed a synthesized midpoint through the renderer/encoder with
correct timestamps. Any mode must remain disabled by default and preserve
normal frame delivery when unsupported.

## Verification record

**Fact:** `VkContext.hpp` passed a C++20 syntax-only compile with the local
ALVR Vulkan and FFmpeg headers on 2026-10-21. No GPU, ALVR process, SteamVR or
headset was used. The NVIDIA card is disconnected; runtime feature/queue
creation remains unknown. No full build or live test was run.

**Primary API reference:** [Khronos Vulkan `VK_NV_optical_flow`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_optical_flow.html)
and [optical-flow queue and image requirements](https://docs.vulkan.org/spec/latest/chapters/VK_NV_optical_flow/optical_flow.html).
