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
