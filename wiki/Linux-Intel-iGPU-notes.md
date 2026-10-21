# Linux: ALVR on an Intel integrated GPU (notes and diagnostics)

Archive notes for the `linux-intel-vaapi` branch (on top of PRs #3423 and
#3425). Tested on one machine: Intel UHD 770 (Raptor Lake-S, the only GPU
after the NVIDIA card was removed), Ubuntu 24.04, kernel 7.0, SteamVR 2.17.10
(Linux), Quest 2 over Wi-Fi, October 2026. Measured facts are marked as such;
the rest is inference.

## What changed in this branch

| Commit | What | Why |
| --- | --- | --- |
| external queue family | SteamVR textures acquired from / released to `VK_QUEUE_FAMILY_EXTERNAL` every frame | ownership rules for memory shared with another process; AMD DCC / Intel CCS |
| validation + GPU selection | `ALVR_VULKAN_VALIDATION=1`, `ALVR_VULKAN_DEVICE` | Vixea's request on #3425; `physDevs[0]` was often the iGPU or llvmpipe |
| Intel VAAPI output | map the Vulkan output to VAAPI (DRM-PRIME), uncompressed Intel tiling | the black stream of issue #3420 |
| Intel low-power encode | FFmpeg `low_power` (VDEnc) when the driver offers it | encode 22 ms -> 4-5 ms, 35 -> 72 fps |
| unfinished frames + fence | skip frames whose SteamVR semaphore was not reached; fence wait outside `assert()` | no half-drawn frames; no silent hangs |
| diagnostics | `ALVR_DEBUG_PIXELS`, `ALVR_DEBUG_FRAMES`, `~/.config/alvr/diag_swaplog` | used to locate the black stream and to clear ALVR of the dashboard flicker |

## Setup that worked

- **Mesa 26.2 or newer.** Mesa 25.2.8 (Ubuntu 24.04) crashes SteamVR's
  vrcompositor at "Creating samplers": `gfx12_CreateSampler` dereferences a
  NULL YCbCr conversion for a custom-border-colour sampler with a
  multi-planar format. Fixed upstream by Mesa commit `7b95d822`
  (first in 26.2.0). Mesa 26.2.5 from the kisak PPA works.
- **VAAPI driver:** `intel-media-va-driver` (iHD). The Ubuntu `+dfsg` build
  exposes only the low-power entrypoints (`VAEntrypointEncSliceLP`); ALVR's
  dashboard then warns "profile does not contain encoding entrypoint", but
  encoding works with this branch. `intel-media-va-driver-non-free` exposes
  both.
- **Resolution / bitrate:** 1152x1200 per eye, HEVC, 62 Mbps, 72 Hz was a
  usable compromise (the GPU is the limit, not the encoder).
- **Leftover NVIDIA user-space libraries** (card removed, driver packages
  still installed) make Steam's web helper crash (it loads
  `libnvidia-glvkspirv.so`), and Steam then shuts SteamVR down. Starting
  Steam with `-cef-disable-gpu` and
  `__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
  __GLX_VENDOR_LIBRARY_NAME=mesa VK_LOADER_DRIVERS_DISABLE='*nvidia*'`
  avoided it. Forcing `VK_DRIVER_FILES` to the host Intel ICD instead broke
  games under Proton ("InitializeEngineGraphics failed" in VRChat).

## The black stream (issue #3420) - fixed

Measured: the client decoded tens of Mbps, yet a server recording
(`POST /api/recording/start`, `/stop`; file in `$HOME/recording.*.h265`) had
every frame at luma 16. `ALVR_DEBUG_PIXELS=1` showed SteamVR's texture, the
composed image and the encoder input with content. The cause was the Intel
"import VA surface" path in `EncodePipelineVAAPI`: it encoded a VA surface
that was never imported as the renderer output (`ImportOutput` commented
out). Intel now maps the Vulkan output like AMD.

## Performance

| Configuration (UHD 770) | Encode | Stream |
| --- | --- | --- |
| default VAAPI entrypoint, 1440x1504 | ~22 ms | ~35 fps |
| `low_power` (VDEnc), 1440x1504, no Home | ~4-5 ms | 72 fps |
| SteamVR Home, 1440x1504 | - | ~32 fps (GPU bound) |

SteamVR measured the GPU at 42 MP/s against 311 MP/s for 1440x1504 at
72 Hz. Scene applications take ~17 ms of GPU per frame on this iGPU, more
than a 72 Hz frame (13.9 ms).

## Known issues (not ALVR)

### Dashboard over a scene application flickers (Home, VRChat)

Measured: with the SteamVR dashboard open over SteamVR Home or VRChat, the
compositor's own output alternates every ~5-12 frames between the scene,
a blurred scene and black (also in SteamVR's desktop VR View). Closing the
dashboard stops it. `IVRCompositor::GetCumulativeStats` with the dashboard
open: ~7-10 application timeouts per second, ~50 of ~57 presents per second
in the timed-out state; with it closed, ~0.

ALVR is faithful: `diag_swaplog` shows one layer per frame, from
vrcompositor only, each eye's 3-texture set consumed exactly at the index
ALVR hands out; `ALVR_DEBUG_FRAMES` shows no dropped layers or unfinished
frames; the pixel readback shows the content reaching the encoder.

Ruled out: GPU synchronisation (`INTEL_DEBUG=sync`), Intel CCS
(`INTEL_DEBUG=noccs`), extra full memory barriers before/after every render
pass, blit, copy and dispatch in vrcompositor (a test Vulkan layer), the
Mesa Source 2 workaround (`anv_assume_full_subgroups=32`), Home resolution
(60 % supersampling), `doNotFadeToGrid`.

Inference: in vrcompositor, when the scene is dimmed (dashboard), the frame
loop stops waiting for the application's frame (`[Compositor]
frameTimeout( 0 ms )`, a 0.99 threshold on a "1 - fade" value); a slow
GPU never has the frame ready, so the application "times out" and the
compositor shows its fallback. Patching that branch to always wait did not
remove the timeouts, so the full mechanism is not understood. AMD (RX 9070
XT) reports the dashboard over a game working; NVIDIA too.

### vrcompositor crashes when SteamVR restarts

ALVR asks SteamVR to restart when a client connects with settings different
from those SteamVR started with. On the way out vrcompositor crashes in
SharedResourceManager's leak check (`vrcompositor+0x231f5c`, a virtual call
on a freed resource) or jumps to an invalid address (`error 15`, also
reported by the AMD tester on #3425). Harmless when ALVR's dashboard
relaunches SteamVR; fatal when SteamVR was started by Steam.

### VRChat hangs at "install script" when SteamVR was started by ALVR

Steam does not know about a SteamVR started by ALVR's dashboard and launches
its own as a dependency of the game, then waits for it forever. Start
SteamVR through Steam (`steam://rungameid/250820`), connect the headset,
then start the game.

### SteamVR Home on Mesa

SteamVR Home (steamtours) reports Vulkan errors under the Khronos layer:
`VUID-VkDeviceCreateInfo-ppEnabledLayerNames-12385`,
`VUID-vkCreateDevice-ppEnabledExtensionNames-01387`,
`VUID-VkShaderModuleCreateInfo-pCode-08737` (invalid SPIR-V control flow),
`-08740/-08742`, `VUID-VkGraphicsPipelineCreateInfo-Input-08733` and
synchronisation hazards on its desktop window. Other SteamVR processes:
`VUID-VkSemaphoreTypeCreateInfo-timelineSemaphore-03252`.

## GPU and encoder selection (branch `linux-gpu-encoder-selection`)

Dashboard: Settings > Presets (last three rows): drop-down menus "Render
GPU", "Encoder GPU" and "Encoder technology". The GPU menus list the GPUs
Vulkan reports on this machine (CPU implementations such as llvmpipe are
left out); "Automatic" forces nothing and leaves ALVR's default behaviour,
any GPU entry forces that GPU. The same settings, with free text, are in
Settings > Video > Encoder config > "GPU and encoder selection (Linux)".

| Setting | Default (automatic) | Values |
| --- | --- | --- |
| Render GPU | first discrete GPU, then first integrated GPU | device index or part of the name ("Vulkan device N: name" in the log) |
| Encoder GPU | the render GPU | DRM render node (`/dev/dri/renderD129`), device index or part of the name |
| Encoder technology | NVENC on NVIDIA, VAAPI otherwise (from the encoder GPU) | Automatic, VAAPI, NVENC, Vulkan Video, Software |

How each choice acts:

- **Render GPU** sets ALVR's Vulkan device, which must be the GPU SteamVR
  renders on (SteamVR's textures are shared as opaque fds, usable only on
  the GPU that created them). SteamVR and games pick their GPU from their
  launch options, not from ALVR: with a Render GPU set, the dashboard prints
  the launch options for that GPU when it starts SteamVR
  (`__GLX_VENDOR_LIBRARY_NAME=nvidia __NV_PRIME_RENDER_OFFLOAD=1 VK_DRIVER_FILES=...`
  for NVIDIA's driver, `DRI_PRIME=vvvv:dddd MESA_VK_DEVICE_SELECT=vvvv:dddd`
  for Mesa drivers). `ALVR_VULKAN_DEVICE` still overrides the setting.
- **Encoder GPU** different from the render GPU: VAAPI only (NVENC needs
  the render GPU). Handing the output over as a linear dma-buf failed
  (the Intel GPU could not read the NVIDIA-exported buffer), so the
  renderer copies each finished frame into host memory with a GPU
  transfer and VAAPI uploads it into a surface of the encoder GPU.
  Measured on RTX 3060 (render) + UHD 770 (encode), 2304x1184: 59 fps,
  encode ~4.6 ms, NVENC idle, correct image. If SteamVR's environment
  limits Vulkan to one driver (e.g. `VK_DRIVER_FILES=nvidia_icd.json`),
  the streamer still finds the encoder GPU's `/dev/dri/renderD*` node by
  PCI vendor (intel / nvidia, geforce / amd, radeon in the selector).
- **Encoder technology**: VAAPI and NVENC are implemented. Vulkan Video and
  Software are placeholders: they log "not implemented yet" and no encoder
  is created. The old "Force software encoding" switch maps to Software.

**When a choice cannot be honoured** because of variables Steam was
started with (a PRIME-offload wrapper such as one setting
`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json`,
`__VK_LAYER_NV_optimus=NVIDIA_only`, `__NV_PRIME_RENDER_OFFLOAD=1`), which
SteamVR and games inherit:

- the dashboard, when it launches SteamVR, logs Steam's GPU-restricting
  environment and warns if it forces a GPU other than the chosen Render GPU;
- the streamer warns "Render GPU setting ... matches no Vulkan device, so it
  cannot be honoured; GPU-restricting environment inherited from
  Steam/SteamVR: ..." at every encoder build;
- an Encoder GPU hidden from Vulkan is still found by its DRM node, and the
  log names the restricting variables.

The streamer logs its decision once per encoder build, e.g.
`Encoder: VAAPI on /dev/dri/renderD128 (render GPU node /dev/dri/renderD128)`.

Next steps this opens: a Vulkan Video pipeline (enable FFmpeg's
`h264_vulkan` / `hevc_vulkan` / `av1_vulkan` encoders, NV12 encode-source
images, a video encode queue), the Linux software pipeline, and a zero-copy
cross-GPU hand-over (host-memory dma-buf or VA user-pointer surfaces)
instead of the CPU upload.

## Diagnostics reference

| Switch | Where | What it logs |
| --- | --- | --- |
| `ALVR_VULKAN_VALIDATION=1` (+ `VK_LAYER_PATH`) | env | Khronos validation on ALVR's instance: "Vulkan validation: ..." |
| `ALVR_VULKAN_DEVICE=<index or name>` | env | forces the Vulkan device; all devices are logged |
| `ALVR_DEBUG_PIXELS=1` | env | once a second, byte averages of the centre of SteamVR's texture, the composed image and the encoder input, plus the encoder input's DRM modifier (adds TRANSFER_SRC to that image) |
| `ALVR_DEBUG_FRAMES=1` | env | every 2 s: frames queued, replaced, aged, unfinished, encoded; frames with 1 / 2+ layers; layers dropped |
| `~/.config/alvr/diag_swaplog` | file, live | while it exists: every GetNextSwapTextureSetIndex, SubmitLayer and Present |
| `ALVR_VAAPI_LOW_POWER=0` | env | keeps the default VAAPI entrypoint on Intel |
| `ALVR_VAAPI_IMPORT_SURFACE=1` | env | the old (black) import path, for comparison |

ALVR's log lines reach the dashboard's event stream
(`ws://127.0.0.1:8082/api/events`), not SteamVR's `vrserver.txt`.
`env` switches must be in the environment Steam is started with, since
SteamVR inherits it.
