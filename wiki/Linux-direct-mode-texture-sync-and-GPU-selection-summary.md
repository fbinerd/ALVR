# Linux direct mode: summary

Updated 2026-10-27. Short version of
[Linux direct mode: texture synchronization and GPU selection](Linux-direct-mode-texture-sync-and-GPU-selection),
which preserves the historical measurements and their limits.

## Scope

[PR #3425](https://github.com/alvr-org/ALVR/pull/3425), inspected at `66b773c8`,
handles direct-mode texture synchronization and cleanup. It already waits up
to eight display intervals (about 111 ms at 72 Hz), shared across a frame's
advertised timelines, and skips unconfirmed frames. The older two-interval
policy that read on timeout is historical.

The GPU selector, Steam GPU environment, cross-GPU encoding and the Intel
Wayland mirror workaround are separate additions, committed individually above
the exact PR head in `codex/mesa-v3-cross-gpu-nvenc-pr3425`. The previous
runtime checkpoint is `b13fc576`; reorganizing the commits preserved its code.
The series also carries the existing #3423 colour fix as a separate commit,
because the inspected PR base predates that upstream fix. Two GPUs do not
guarantee a failure, but SteamVR and ALVR must render on the same GPU.

## Synchronization findings

| Topic | Result |
|---|---|
| Earlier matched-GPU load tests | Waits expired during application start, loading and heavy frames; no import failures in those windows |
| Skip with a two-interval cap | Heavy scenes froze for up to about 1 s in the earlier blind A/B |
| Shared eight-interval cap | No skips in those A/B windows; already incorporated in inspected PR head |
| Later hybrid-GPU mismatch | Semaphore import returned `ErrorUnknown`, yielding zero confirmed frames; consistent render selection recovered streaming |
| Queue-family ownership transfer | Retained in the PR; earlier NVIDIA/Intel tests showed a picture without it, while AMD remains untested |

## GPU selection and encoding

| Feature | Behavior |
|---|---|
| Render GPU | `ALVR_VULKAN_DEVICE`, saved Render GPU, then system order; Automatic has no discrete-first preference |
| Encoder GPU/backend | Independently chooses encoder device and VAAPI/NVENC backend |
| Steam environment | Dashboard Launch/Restart applies the selected render GPU; confirmation reports missing/conflicting variables for a running Steam client |
| Cross-GPU NVENC | Render completion, host-RAM staging, independent CUDA upload and synchronization before staging reuse |
| Same-device NVIDIA | Keeps the existing render/encode path; no cross-GPU host upload |

Cross-GPU encoding currently uses host RAM, not P2P or zero-copy DMA-BUF
handover. Its latency and resolution limits depend on direction and workload.
The earlier 1440x1504-per-eye / 72 fps result into Intel VAAPI is not a
universal limit for the newer Intel-to-NVIDIA NVENC path.

## Recent desktop validation

Host: Intel RPL-S + RTX 3060, Quest 2, SteamVR 2.18.4; later remote tests used
EndeavourOS/KDE Wayland, Mesa 26.2.5 and NVIDIA 615.78.08.

| Firmware primary | Render | Encode | Result |
|---|---|---|---|
| Intel | NVIDIA | NVIDIA | User-confirmed picture after applying the selected render environment |
| Intel | Intel | NVIDIA | User-confirmed picture; zero sync skips in sampled windows, variable FPS |
| NVIDIA | Intel | NVIDIA | User-confirmed picture through normal Dashboard Restart with the mirror workaround; later ~72 fps, zero sampled sync skips |
| NVIDIA | NVIDIA | NVIDIA | User-confirmed picture; 22,336 frames in 31 ten-second windows, zero sync skips |

Intel-primary checks used `75608676`/`ebc2c641`; the automatic NVIDIA-primary
checks used `b13fc576`. This was not a controlled four-case run of one build.

The NVIDIA-primary Intel-render trial used 2560x1216 total output. Later
upload means were 1.669–1.710 ms; startup included three sync skips and a
48.944 ms upload peak. These are functional checks, not a guarantee of game
performance or uniformly bounded latency. Matching streamer/client builds
were produced and the installed APK hash was checked before the coordinated
trial. The subsequent user-started same-device trial had no new independent
pre-trial ADB check.

Earlier NVIDIA/NVK render into Intel VAAPI and Intel-only VAAPI trials remain
recorded in the full page. Physical notebooks, AMD and AV1 cross-GPU encoding
have not been validated here.

## NVIDIA-primary Intel Wayland mirror workaround

SteamVR failed before encoder initialization when creating its desktop mirror
swapchain on Intel while NVIDIA was firmware-primary. A reversible section in
its existing compositor shell launcher enables
`MESA_VK_WSI_HEADLESS_SWAPCHAIN=1` only for Intel selection + Wayland + NVIDIA
`boot_vga=1`.

Headset rendering/encoding works, but the desktop mirror is unavailable in
this case. Steam and games do not inherit the variable. NVIDIA render removes
the section and restores the original launcher body/permissions. No ELF
binary is wrapped. The exact failing swapchain parameter remains unknown;
SteamVR's fallback-mirror setting did not solve it and was cleared.

## Practical guidance and remaining work

- Start/restart through the ALVR Dashboard to apply GPU selection and register
  the intended driver; SteamVR's own restart can retain the old environment.
- Keep Mesa Vulkan selection through `MESA_VK_DEVICE_SELECT`/`DRI_PRIME`; do
  not pin a Mesa `VK_DRIVER_FILES` path, which broke Proton startup here.
- NVIDIA proprietary selection also uses `__VK_LAYER_NV_optimus=NVIDIA_only`;
  NVK requires the Mesa path despite reporting the same vendor ID.
- Revalidate physical notebooks, AMD, Vulkan validation and latency under
  matched heavy workloads. An IPC publication race has not been reproduced;
  distinguish it from the observed GPU-mismatch import failure.
- Steam CEF GPU processes crashed during one Intel-selected startup, but the
  user reported a normal interface and VR worked afterward; no separate CEF
  fix was implemented.

## Later Zorin X11 recovery

Applying the full Mesa GL environment to Steam crashed Xorg inside NVIDIA's
driver. Vulkan-only Intel selection preserved the desktop but left NVIDIA GL
in Steam's VR menu, producing framebuffer/interop failures. A scoped Intel GL
environment in the two 64-bit WebHelpers restored the in-headset menu (user
confirmed); Steam startup keeps its default GL vendor. The Dashboard prepares
the scripts after Steam's updater runs and restarts only a stale browser before
launching SteamVR. Wayland remains on its earlier path. Ordinary OpenGL games
keep their own/desktop GL selection. Separate compositor crashes remain under
investigation; desktop/menu recovery is not full-session stability proof.
