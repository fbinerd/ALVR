# Linux direct mode: texture synchronization and GPU selection

Notes from a test campaign on the Linux direct-mode streamer (October 2026):
how ALVR reads SteamVR's textures, what goes wrong on Mesa drivers and slow
GPUs, how the render/encoder GPU is chosen, and what was measured. Each
section separates **facts** (measured or read from code), **inferences** and
**unknowns**.

## Scope and revision (updated 2026-10-27)

[PR #3425](https://github.com/alvr-org/ALVR/pull/3425) covers Linux direct-mode
frame synchronization. Its inspected head is `66b773c8`, including Vixea's
cleanup and synchronization changes. That revision already uses a shared
eight-interval timeline wait budget and skips unconfirmed frames. Earlier
measurements below explain how that policy was selected; descriptions of the
older two-interval/read-anyway behavior are historical.

GPU selection, Steam environment handling, cross-GPU encoding and the Intel
Wayland mirror workaround are separate local additions. They are not supplied
by PR #3425 alone. Two GPUs do not automatically cause failure; a mismatch
between SteamVR's render GPU and ALVR's render GPU can cause it. Physical
notebooks and AMD hardware have not been tested in this campaign.

Current checkout: `codex/mesa-v3-cross-gpu-nvenc-pr3425`, with the exact PR
revision as an ancestor and the GPU additions organized as separate commits.
The series also carries the existing upstream #3423 colour fix independently,
because the selected PR base predates it. The previous runtime checkpoint is
`b13fc576` on `codex/mesa-v3-cross-gpu-nvenc`. Reorganizing the commits preserved
that runtime source tree; the wiki was updated separately.

## Test setup

| Item | Value |
|---|---|
| Headset | Quest 2; earlier campaign: HEVC, 72 Hz, 1440x1504 per eye; later cross-GPU output: 2560x1216 total |
| Discrete GPU | RTX 3060, on NVIDIA's proprietary driver (615) and on nouveau + Mesa NVK |
| Integrated GPU | Intel UHD 770 (Raptor Lake-S), Mesa ANV, VAAPI (iHD, low-power VDEnc) |
| Mesa | 26.2.5 |
| SteamVR | 2.18.4, direct mode |
| Base code | PR #3425 head `66b773c8` plus separate local GPU additions; runtime checkpoint `b13fc576` |

Configurations tested:

- NVIDIA proprietary: render and NVENC on the RTX 3060.
- NVK: render on the RTX 3060 (Mesa), encode on the Intel GPU with VAAPI. The
  frame crosses GPUs through host memory, because NVK has no NVENC.
- Intel only: render and VAAPI encode on the UHD 770.
- Intel render and NVIDIA NVENC, with both Intel-primary and NVIDIA-primary
  firmware configurations (later remote tests, detailed below).
- NVIDIA render and NVIDIA NVENC with either firmware-primary GPU.

Intel-primary visual checks used the `75608676`/`ebc2c641` checkpoints; the
automatic NVIDIA-primary checks used `b13fc576`. They were successive
revisions, not a single controlled four-case run of the final build.

The later remote host uses EndeavourOS, KDE Wayland, Mesa 26.2.5 and NVIDIA
615.78.08. Firmware-primary refers to the user's BIOS choice; NVIDIA primary
was independently confirmed by its PCI `boot_vga=1` state.

## Background: how the streamer reads a SteamVR frame

1. SteamVR's compositor draws each eye into a shared texture and calls the
   driver's `Present`.
2. The driver hands the frame to an encode worker. Before reading the
   texture, the worker waits for SteamVR's per-texture timeline semaphore to
   reach the frame's value: the texture is then finished.
3. The worker renders (copy, foveation, layers) and the encoder compresses
   the result.

The frame-sync PR introduced step 2. Its current inspected head `66b773c8`
waits up to eight display intervals (about 111 ms at 72 Hz), shared across
all advertised timelines for one frame. A failed import or expired wait
rejects the frame; the client keeps its previous picture. The older PR
revision used a two-interval cap and read anyway after timeout. That older
behavior is not the policy of the current PR.

## Questions and answers

| Question | Answer | Status |
|---|---|---|
| Does the Vulkan layer's `vkWaitForPresentKHR` (released at queue time) cause frame overlap? | No longer relevant: the direct-mode switch (#3398) deleted `alvr/vulkan_layer`. No current branch contains that code. | Settled (git) |
| Which code did the direct-mode switch bring in? | The worker thread with a latest-wins mailbox and stale-job drop, rotational reprojection with re-stamping, PostPresent pacing on a vsync grid with a 2 ms lead, a free-running vsync announcer, write-fence polling with a guard sleep, newest-first pose matching. The frame-sync PR builds on it. | Fact (commit history) |
| Is the "SteamVR texture semaphore not reached or not importable" warning at app start caused by the robustness commit in the PR? | No: it also appears on builds without that commit. | Settled |
| Why does that warning appear? | Earlier matched-GPU load tests hit the wait cap. Later hybrid-GPU tests also reproduced import failure when SteamVR and ALVR selected different render GPUs. A publication/IPC-registration race remains an unobserved hypothesis. | Timeout and GPU-mismatch cases observed; race unproven |
| How often does the frame get read before it is finished? | Rare on NVIDIA. Several times per application start on NVK. Much more often on Intel (85 timeouts in 3 minutes of VRChat). Slower GPUs hit it more. | Fact |
| If unconfirmed frames are skipped instead of read, do heavy games stutter? | With a 2-interval cap, yes: runs of up to 66 skipped frames (about 0.9 s frozen picture). With an 8-interval cap, no skips at all. | Settled (blind A/B) |
| Is queue-family ownership transfer (`VK_QUEUE_FAMILY_EXTERNAL`) needed for a correct picture on Intel? | No: an A/B showed a correct picture with and without it. One blocky picture seen once on Intel had another, unknown cause. | Settled for Intel; AMD unknown |
| Do unconfirmed reads explain occasional flashes reported on AMD? | Plausible (same mechanism, more frequent on Mesa and slow GPUs), but no flash was ever seen together with a logged unconfirmed read. | Hypothesis (needs AMD) |
| Is the overlay layer alpha premultiplied? | Yes (measured on the dashboard layer); the PR blends it as premultiplied. | Settled |
| Does Vulkan validation report errors from ALVR? | An earlier run (NVIDIA and Intel, previous PR revision with ownership transfer) reported none from ALVR; the device-layer VUID seen elsewhere belongs to SteamVR Home. | Needs a rerun on the current code |

## Unconfirmed texture reads

**Fact (earlier matched-GPU campaign):** the warning was split into "could
not be imported" and "not reached within the wait cap". Every case in those
runs was the second one. Later hybrid-GPU trials reproduced the first when
SteamVR and ALVR used different GPUs; see the recent validation section.
The earlier absence of import failures must not be generalized to all setups.

**Fact (frequency):**

| Configuration | Workload | Unconfirmed frames |
|---|---|---|
| NVIDIA proprietary | normal use | 2 single warnings in about 10 application starts |
| NVK | SteamVR Home, menu, VRChat start (8 min) | 6 |
| NVK | Home x3 + VRChat (two runs, skip policy) | 11 timeouts, 7 frames skipped, 0 read |
| Intel | Home x3 | 4 timeouts, all skipped |
| Intel | VRChat, about 20 fps, 3 min | 85 timeouts (1.6% of frames) |

**Inference:** the texture really was still being drawn. The VRChat frames
that timed out on NVK took up to 31 ms of GPU time, more than the 28 ms cap.
Reading such a texture sends a mix of two frames, which looks like a flash.

### Policy options

| Policy | On timeout | Result |
|---|---|---|
| Older PR revision (historical) | read anyway after 2 intervals | can send an unfinished frame |
| Skip, 2-interval cap | drop the frame; the client keeps the previous one | no unfinished frames; **freezes under load** |
| Current PR `66b773c8`: skip, 8-interval shared cap | wait up to about 111 ms for all textures together, then drop | no unfinished frames; **no freeze measured in the earlier A/B** |

A GPU-side wait (submitting the render with a wait on the timeline value)
was considered. It brings nothing here, because the renderer already blocks
on its own fence before encoding: a CPU wait with a generous cap behaves the
same and cannot hang the queue if the value never arrives.

### Overload A/B: 2 vs 8 intervals

**Method:** Intel render and encode, SteamVR Home unthrottled (application
GPU time 60-80 ms per frame). 60-second windows in the order 8, 2, 2, 8.
Before each window the matching streamer and client builds were installed
and verified by SHA-256, and the mapped driver was checked. Collected:
SteamVR frame timings, ALVR statistics and logs, the client's VrApi line,
Intel engine activity. The tester did not know which build ran in which
window.

| Window | Cap | Skipped frames / 60 s | Longest run | Tester's rating |
|---|---|---:|---:|---|
| 1 | 8 | 0 | - | smooth for this GPU, no stutter |
| 2 | 2 | 231 | 66 (~0.9 s) | froze when leaving the Home room |
| 3 | 2 | 207 | 66 | same as window 2 |
| 4 | 8 | 0 | - | better than the others, no freeze |

No unconfirmed read and no import failure in any window. Server frame rate
(median 14-24 fps) and pipeline latency (median 70-83 ms) did not separate
the builds.

**Conclusion:** use the 8-interval shared cap with the never-read rule. This is already present in PR #3425 head `66b773c8`, as well as the local GPU integration.

### Low-load control

With GPU headroom (Home throttled to 6-18 fps), both caps produced 0 or 1
skips per 30-second window on NVK and Intel. The 2-interval rule does not
hurt when frames finish in time. Effective frame rates differed between
builds at the same settings for reasons not determined, so those windows
are not used to rank latency or performance.

## Queue-family ownership transfer

**Fact:** SteamVR's textures come from another process. The Vulkan spec
requires acquiring them from `VK_QUEUE_FAMILY_EXTERNAL` before use and
releasing them after. Without the transfer, NVIDIA and Intel ANV still
produced correct pictures in these tests.

**Inference:** drivers that keep images compressed (AMD with DCC) may not
tolerate its absence. The transfer is kept, with a code comment saying so.
Cost is negligible. It is not a proven fix for any visible artefact.

## GPU selection on Linux

### Why a GPU selector

The inspected upstream/PR baseline has no explicit render/encoder GPU
selector and encodes on the render device, with NVENC on NVIDIA and VAAPI
otherwise. The local selector's Automatic choice follows the Vulkan loader's
system order, skipping a CPU implementation when a hardware device exists.
It does not prefer discrete GPUs. Selection precedence is
`ALVR_VULKAN_DEVICE`, the saved Render GPU setting, then Automatic.
Machines with two GPUs may need explicit control:

- **The render GPU must be the one SteamVR uses.** SteamVR shares its
  textures only within one GPU. If ALVR's Vulkan device is a different GPU,
  the import fails or the picture is wrong. Before the selector there was
  no setting for this, and nothing told the user why it failed.
- **Encoding competes with rendering.** NVENC runs on the same GPU as the
  game. An integrated GPU's fixed-function encoder (Intel VDEnc) is
  otherwise idle and can take that work.
- **Some drivers have no usable encoder.** On nouveau + NVK the NVIDIA GPU
  renders but has no NVENC, so encoding must move to another GPU.
- **Driver testing.** Problems reported only on Mesa (AMD/Intel) can be
  reproduced on a machine without AMD hardware by rendering on Intel ANV or
  NVK. Without a selector, those paths cannot be exercised next to the main
  GPU.

### What the selector provides

| Setting / feature | Effect |
|---|---|
| Render GPU | ALVR's Vulkan device, by index or part of the name; the dashboard shows which GPU "automatic" takes |
| Encoder GPU | The GPU that encodes: a DRM render node, an index or part of the name; may differ from the render GPU |
| Encoder technology | Automatic (NVENC only on NVIDIA's proprietary driver, VAAPI otherwise), VAAPI or NVENC |
| Cross-GPU encoding | When the encoder GPU differs, the rendered frame is copied to host memory and uploaded to the encoder GPU (a linear dma-buf import from NVIDIA memory into Intel VAAPI failed) |
| Steam restart | After confirmation, restarts Steam with the render GPU's environment, so SteamVR and the games follow the choice |
| Warnings | Explain missing or conflicting Steam GPU selection; an empty environment does not prove that SteamVR will use the requested GPU |

### How it was used in these tests

| Goal | Render GPU | Encoder GPU |
|---|---|---|
| Normal use on the proprietary driver | RTX 3060 | RTX 3060, NVENC |
| Offload encoding from the discrete GPU | RTX 3060 | Intel UHD 770, VAAPI |
| Test Mesa NVK | RTX 3060 on NVK | Intel UHD 770, VAAPI (NVK has no NVENC) |
| Test Mesa on a slow GPU (closest to the AMD reports) | Intel UHD 770 | Intel UHD 770, VAAPI |
| Overload trial for the texture wait policy | Intel UHD 770 | Intel UHD 770, VAAPI |

### Recent validation with either firmware-primary GPU (2026-10-26/27)

The user performed application launches and visual checks; the agent inspected
GPU diagnostics and build artifacts. These are tests on a desktop containing
an Intel RPL-S iGPU and an RTX 3060, not physical notebook validation.

| Firmware primary | Render GPU | Encoder GPU | Observed result |
|---|---|---|---|
| Intel | NVIDIA | NVIDIA NVENC | User-confirmed picture after applying the render GPU environment to Steam/SteamVR |
| Intel | Intel | NVIDIA NVENC | User-confirmed picture with the independent CUDA uploader; sampled upload means 2.249–2.537 ms, zero sync skips, variable frame rate |
| NVIDIA | Intel | NVIDIA NVENC | User-confirmed picture through ordinary Dashboard Restart with the scoped headless mirror workaround; later windows 719–721 confirmed frames per 10 s, zero sync skips |
| NVIDIA | NVIDIA | NVIDIA NVENC | User-confirmed picture; 31 ten-second windows, 22,336 confirmed frames, zero sync skips; original mirror launcher restored |

Intel-primary cross-GPU samples ranged from 154–364 confirmed frames per ten
seconds; they do not establish sustained 72 fps. NVIDIA-primary cross-GPU
samples used a 2560x1216 output image and later upload means 1.669–1.710 ms.
Its startup window included three sync skips and an upload peak of 48.944 ms;
these observations are not a bounded-latency or game-performance guarantee.
The earlier 1440x1504-per-eye measurements below concern the opposite transfer
direction into Intel VAAPI and must not be applied as a universal NVENC limit.

**Cross-GPU NVENC implementation:** `NvencHostInput` resolves the encoder DRM
node to its PCI address and selects the corresponding CUDA device independently
of the renderer's Vulkan GPU. After renderer completion, the rendered image is
copied into coherent host staging memory, uploaded to a CUDA/NVENC surface,
and the upload stream is synchronized before that staging memory is reused.
This uses host RAM, not direct GPU-to-GPU P2P or a zero-copy DMA-BUF bridge.
The same-device NVIDIA path remains separate. Tests covered RGBA/BGRA,
UNORM/sRGB formats, padded rows, immediate source reuse, full-size transfers,
and H264 encode/decode. A transfer-only test is not an end-to-end VR pass.

**Two distinct black-screen causes were observed:**

1. SteamVR and ALVR selected different render GPUs. `vkImportSemaphoreFdKHR`
   returned `ErrorUnknown`; sampled windows contained zero confirmed frames.
   Restarting from the ALVR Dashboard applied a consistent render environment
   and registered the intended driver directory. Restarting SteamVR through
   its own mechanism had retained the old environment/registration.
2. With NVIDIA primary and both renderers correctly on Intel, SteamVR 2.18.4
   failed to create its Wayland desktop mirror swapchain (`VkResult=-3`,
   `CannotConnectToDisplayServer`), before NVENC initialization. Generic Intel
   Wayland presentation probes succeeded, so the exact failing parameter or
   driver interaction remains unknown.

For the second case, the Dashboard manages a reversible section in Valve's
existing `vrcompositor-launcher.sh`. Only Intel selection + Wayland + NVIDIA
`boot_vga=1` enables `MESA_VK_WSI_HEADLESS_SWAPCHAIN=1` in the compositor
child. Steam and games do not inherit it. The desktop mirror is unavailable
in that case, while headset rendering and encoding continue. Selecting
NVIDIA render removes the section; restoration of the original launcher
body and executable permissions was verified. The ELF compositor is not
wrapped or renamed. SteamVR's `allowFallbackMirrorWindowLinux` setting did
not fix this trial and was cleared afterward.

The matching streamer and Android client were built, and the installed Quest
APK hash was verified before the coordinated NVIDIA-primary trial. The later
same-device trial was started by the user; there was no additional independent
pre-trial ADB check. Steam CEF GPU-process crashes appeared during Intel-selected
startup, but the user reported a normal Steam interface and VR subsequently
worked. Their separate cause is unresolved; no CEF configuration fix was made.

### Other use cases

Separating the render GPU from the encoder GPU makes the encoder a choice
of its own, independent of the GPU that runs the game. The cross-GPU copy
adds work and some latency (see the limits below), so these setups trade
latency for a capability the render GPU lacks.

| Use case | Example | Status |
|---|---|---|
| A codec the render GPU cannot encode | Render on an RTX 30 series (NVENC has no AV1) and encode AV1 with VAAPI on a GPU that supports it (recent Intel Arc/Xe or AMD RDNA3) | Possible, not tested: the UHD 770 used here reports no AV1 encode entrypoint |
| Render GPU without a usable encoder | NVIDIA on nouveau/NVK; low-end cards without NVENC | Tested (NVK + Intel VAAPI) |
| Free the render GPU for the game | Heavy games where NVENC competes with rendering for GPU time and memory bandwidth | Tested (NVENC at 0%, 72 fps at 1440x1504) |
| Laptops with hybrid graphics | Discrete GPU renders, the integrated GPU (which often drives the display) encodes | Same setup as tested; not tested on a laptop |
| Choose the encoder by quality or features | Pick the encoder whose rate control, 10-bit or low-power mode suits the stream (10-bit is VAAPI-only on Linux) | Possible; quality not compared here |
| Avoid an encoder's session limit or load | Consumer NVENC session limits, or an encoder already used by recording | Possible, not tested |
| Mesa driver testing next to the main GPU | Render on Intel ANV or NVK to reproduce problems reported on AMD/Intel | Tested; most results on this page depend on it |

### Advantages and limits

**Advantages (measured):**

- The discrete GPU can be freed from encoding. Rendering on the RTX 3060 and
  encoding on the Intel GPU streamed at 72 fps at 1440x1504 per eye, 90 Mbps
  HEVC, with 10.2 ms encode time. At first light the cross-GPU path ran at
  59 fps with 4.6 ms encode and NVENC at 0%.
- NVK becomes usable for streaming (latency median 63 ms, the same range as
  the proprietary driver).
- Mesa-only problems can be reproduced and measured without AMD hardware;
  most results on this page depend on that.
- Misconfigurations are reported instead of failing silently.

**Limits (measured):**

- The cross-GPU copy through host memory is the bottleneck at high
  resolutions: 1600x1680 per eye reached about 69 fps (12.6 ms encode),
  1832x1920 about 51 fps (16.5 ms). In that earlier NVIDIA-to-Intel-VAAPI
  campaign, 1440x1504 was the highest setting that held 72 fps. These are
  workload/direction-specific results, not a limit for every cross-GPU path.
  A zero-copy handover between GPUs is not implemented.
- The choice takes effect only when SteamVR starts from a Steam client that
  has the right environment. Changing the Render GPU means restarting Steam.
- Encoder technologies "Vulkan Video" and "Software" are placeholders on
  Linux.

### Expected behaviour on AMD (not tested)

No AMD GPU was available for these tests. The expectations below come from
the code paths and from the Intel and NVK results:

| Setup | Expectation | Reason |
|---|---|---|
| A single AMD GPU | Very likely to work | The selector can stay on automatic, which keeps the previous behaviour; VAAPI on AMD is ALVR's existing path |
| AMD renders and encodes, another GPU present | Likely | Same Mesa path as the tested Intel render + Intel encode |
| AMD renders, another GPU (Intel) encodes | Likely | The Intel VAAPI host-memory path was tested with NVIDIA/NVK rendering; AMD remains untested |
| NVIDIA renders, AMD encodes | Uncertain | AMD VAAPI receiving frames from host memory was never tested |
| Laptop with AMD integrated + AMD discrete GPU | Likely | Both use RADV; selection by vendor:device should tell them apart |

Known risks on AMD: AMDVLK or amdgpu-pro Vulkan drivers interfere with
SteamVR (the dashboard warns about them); image compression (DCC) is where
the queue-family ownership transfer may matter; and whether the
never-read rule removes the flashes reported on AMD can only be confirmed
there.

### What ALVR can and cannot force

- The streamer picks its own Vulkan device ("Render GPU" setting).
- SteamVR's compositor picks its GPU itself, the primary one by default,
  from its own environment. ALVR cannot change that environment after
  SteamVR starts.
- SteamVR inherits the Steam client's environment only when Steam launches
  it. A program that calls `VR_Init` first (for example a pyopenvr script)
  starts SteamVR outside Steam, without that environment.

The selector therefore offers, after confirmation, to close Steam and start
it again with the selected GPU's environment. When Steam is not running, it
starts Steam that way directly. Use the ALVR Dashboard's Launch/Restart
button to apply these changes. Its confirmation shows the variables that will
be applied when a running Steam client needs a different environment. GPU
environment construction is shared by launch, validation and confirmation;
the normal launch path also re-registers the active ALVR driver.

### Environment per driver

| Render GPU driver | Variables |
|---|---|
| NVIDIA proprietary | `__GLX_VENDOR_LIBRARY_NAME=nvidia __NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only`, plus `VK_DRIVER_FILES=<nvidia_icd.json>` when located |
| Mesa (Intel, AMD, NVK) | `DRI_PRIME=<vendor:device> MESA_VK_DEVICE_SELECT=<vendor:device>! __GLX_VENDOR_LIBRARY_NAME=mesa`, plus the Mesa `__EGL_VENDOR_LIBRARY_FILENAMES` path when present |

Lessons:

- **Tell NVK from the proprietary driver.** Both report vendor `0x10de`. The
  dashboard now checks the Vulkan driver ("NVK"/Mesa vs "NVIDIA"); the
  streamer checks the encoder node's kernel driver (`nouveau` vs `nvidia`).
  NVENC is chosen automatically only on the proprietary driver.
- **Do not set `VK_DRIVER_FILES` for Mesa GPUs.** `MESA_VK_DEVICE_SELECT` with
  `!` already hides the other GPUs. Limiting the ICD list as well kept
  VRChat (Proton) from starting on the Intel GPU.
- **Watch for launcher wrappers.** A user-level `steam` wrapper that exports
  the NVIDIA variables makes the 32-bit Steam client crash in
  `libGLX_nvidia` on an NVK boot, and overrides the selector's choice.
  Launch `steam.sh` directly in that case.
- With the NVIDIA libraries installed but nouveau loaded, Steam needs
  `-cef-disable-gpu` to start reliably.

## Measurements on NVK and Intel

| Measurement | Value |
|---|---|
| NVK render + Intel VAAPI, normal streaming | pipeline latency median 63 ms (p90 77), 72 fps; same range as the proprietary driver (64 ms) |
| SteamVR Home on NVK | 15-19 ms GPU per frame (Home renders 2494x2601 per eye with supersampling): 51-64 fps on the compositor, so the stream varies between 53 and 72 fps |
| SteamVR menu open on NVK | compositor at 72 fps, 0 dropped or reprojected frames |
| VRChat loading | 24-42 fps with GPU at 3-7 ms (application-bound, not GPU-bound) |
| A 276-359 ms latency spike | application `game_time` of 223 ms during VRChat loading, not the streamer |
| Intel only, Home | server fps median 72, p10 41; latency median 71 ms, max 932 ms; a burst of client "Latency is too high" and "Desync" warnings |
| Intel only, VRChat | about 20 fps; latency median 77 ms, p90 149 |

Moving menus at low frame rates leaves a visible drag. The same effect
appears on NVIDIA when the application is capped at 36 fps. It comes from
the low frame rate, not from texture synchronization.

## Other bugs found

- **Dashboard crash on preset combo boxes.** A preset combo box that is not
  yet applied lists "Preset not applied" first. Picking entry *i* applied
  variant *i* instead of *i - 1*: every pick applied the next entry, and
  picking the last one panicked (`index out of bounds`, `choice.rs`),
  closing the dashboard and, with it, SteamVR. Present in ALVR since 2024.
  Fixed by offsetting the index.
- **Intel VAAPI black stream.** VAAPI encoded a surface nothing rendered into;
  fixed by mapping the Vulkan output with uncompressed tiling. A separate
  commit enables Intel's low-power VDEnc encoder.

## Open questions

| Question | What would settle it |
|---|---|
| Do unconfirmed reads cause the AMD flashes, and does the never-read rule with the 8-interval cap remove them? | A test on an AMD GPU with a build that logs every unconfirmed frame with a timestamp |
| Does the queue-family ownership transfer matter on AMD? | The same AMD test, with and without it; a Vulkan validation run |
| Vulkan validation on the current code | Rerun with `ALVR_VULKAN_VALIDATION=1` on NVIDIA, NVK and Intel |
| Latency cost of the 8-interval cap on fast GPUs | Heavy games on NVIDIA; no timeouts occurred there so far |
| Cause of the single blocky Intel picture | Not reproduced since |
| IPC publication/registration race | Still unobserved; distinguish it from the demonstrated GPU-mismatch import failure |
| Exact Intel Wayland mirror failure with NVIDIA primary | Identify the invalid swapchain request/driver interaction; headless is a workaround |
| Physical hybrid notebooks and AMD combinations | Validate real hardware; the desktop dual-GPU matrix does not prove these cases |
| Sustained cross-GPU latency and game performance | Matched workloads/resolutions and longer measurements; current samples establish functionality |

## Testing method notes

- Verify the exact binaries: an older build was once left loaded under a
  newer checkout. Hash the mapped driver and the installed client before
  each window.
- Install the client built from the same checkout as the streamer.
- Use A/B windows in crossed order (ABBA), keep the tester blind to the
  build, and record the rating per window.
- Make sure only one test script runs at a time. Two concurrent restarts
  start two dashboards that fight over SteamVR.
- Do not let a measurement tool start SteamVR. Start SteamVR first, then the
  recorders.
- Announce every step in the headset: route speech to the "ALVR Audio" sink,
  otherwise it plays on the PC.

## Ownership of the changes

Already in inspected PR #3425 head `66b773c8`: shared texture/layer handling,
cleanup, queue-family ownership transfer, and the shared eight-interval wait
with unconfirmed-frame rejection. Earlier campaign findings now incorporated
in the PR must not be described as new GPU-selector features.

Separate GPU additions (committed individually on top of that PR):

| Type | Change |
|---|---|
| feat | Opt-in Vulkan validation (`ALVR_VULKAN_VALIDATION=1`) and render selection (`ALVR_VULKAN_DEVICE`, saved Render GPU, then system order) |
| fix | Intel VAAPI: encode the rendered image instead of an empty surface |
| perf | Intel VAAPI: low-power fixed-function encoder (VDEnc) |
| feat | Render GPU, Encoder GPU and Encoder technology settings |
| feat | Dashboard preset menus for those settings |
| feat | Encoding on a different GPU through host memory |
| feat | Warnings when the environment overrides the GPU choice |
| feat | Start or restart Steam with the selected render GPU's environment |
| fix | Tell NVIDIA's proprietary driver from Mesa NVK; no `VK_DRIVER_FILES` restriction for Mesa render GPUs |
| fix | Dashboard: preset combo box off-by-one crash |
| feat | Independent CUDA/NVENC upload from a different render GPU |
| fix | Apply the render environment independently of firmware-primary GPU; show missing/conflicting variables and register the correct driver |
| fix | Compositor-only headless mirror compatibility for Intel render on NVIDIA-primary Wayland |
| test | NVENC host-transfer/encode checks and Dashboard environment/mirror regressions |
| docs | Code comment clarifications; these wiki pages |
