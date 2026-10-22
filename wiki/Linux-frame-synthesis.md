# Linux frame synthesis (native motion)

Experimental frame synthesis for the ALVR Linux streamer: when the VR
application misses a display slot, the driver synthesizes the missing picture
from the two newest real pictures and streams it, so the headset keeps
receiving the full refresh rate. It plays the role SteamVR Motion Smoothing
plays on Windows; SteamVR's own Motion Smoothing is not available on Linux
(see [Why not SteamVR's Motion Smoothing](#why-not-steamvrs-motion-smoothing)).

Branch: `linux-frame-synthesis` (formerly `airlink-gpu-encoder-selection`).
Status (2026-10-22): works end to end and validated on a Quest 2 in four
headset sessions; mode 7 is the recommended mode. Off by default.

## Contents

- [Quick start](#quick-start)
- [Modes](#modes)
- [How it works](#how-it-works)
- [Comparison with ASW and SSW](#comparison-with-asw-and-ssw)
- [Results](#results)
- [Depth (hDepthTexture) evaluation](#depth-hdepthtexture-evaluation)
- [Limitations and known issues](#limitations-and-known-issues)
- [Bugs found and fixed along the way](#bugs-found-and-fixed-along-the-way)
- [Why not SteamVR's Motion Smoothing](#why-not-steamvrs-motion-smoothing)
- [Test kit](#test-kit)
- [Offline checker](#offline-checker)
- [Next steps](#next-steps)

## Quick start

Requirements:

- Linux, SteamVR, ALVR built from this branch.
- An NVIDIA RTX GPU (`VK_NV_optical_flow`, RTX 20 series or newer) selected
  as **both** Render GPU and Encoder GPU (same-GPU NVENC).
- Foveated encoding **disabled**.

Enable (mode 7 recommended), then restart SteamVR:

```bash
echo 7 > ~/.config/alvr/diag_native_motion
```

Disable: delete the file (or write `0`) and restart SteamVR. The file is read
when the Vulkan context and the encoder are created, so a change applies after
a SteamVR restart.

The older environment variables `ALVR_NATIVE_MOTION=1` (and
`ALVR_NATIVE_MOTION_BIDIRECTIONAL=1`) still work when `vrserver` inherits
them, but when the dashboard starts SteamVR through a running Steam client
they never reach `vrserver`; this is why the file switch exists.

If any requirement is missing (other GPU vendor, cross-GPU encode, foveated
encoding, unsupported format) the driver logs the reason and streams normal
frames.

## Modes

| Mode | Behaviour | Use |
|---|---|---|
| 0 / no file | Normal ALVR, no synthesis | default |
| 1, 2 | Controlled experiment: drops every other real picture and predicts it (2 = bidirectional flow session) | lab only |
| 3 | Repeat reference: drops every other real picture and repeats the newest | measurement baseline |
| 4 | Gap filling: every real picture is sent; a missed slot gets an extrapolated picture | superseded |
| 5 | Mode 4 + head-rotation separation | superseded |
| 6 | Mode 5 + confidence blending | experimental, worse |
| **7** | Mode 5 + still-scene dead zone, colour-guided flow, border fallback | **recommended** |
| 9 | Interpolation between the two newest real pictures (holds the real picture one slot) | experimental |

Modes 4–9 only act when the application misses a slot. With an application at
the full refresh rate they synthesize almost nothing (178 pictures in 30 s on
the Quest, versus about 1,000 with the application at half rate).

Measurement switch: `~/.config/alvr/diag_native_motion_eager` computes the
flow right after each real picture instead of on the synthesized one
(A/B showed no clear difference; off by default).

## How it works

Code: `alvr/server_openvr/cpp/platform/linux/` — `motion/NativeMotion.cpp`,
`motion/NativeOpticalFlow.cpp`, `shader/motion_luma.comp`,
`shader/motion_warp.comp`, `Encoder.hpp`, `OvrDirectModeComponent.cpp`
(`EncodeWorkerLoop`), `motion/NativeMotionSwitch.hpp`.

1. **Capture.** Each real picture the encoder renders is copied into owned
   GPU history (two slots) and reduced to per-eye luma (largest dimension
   capped at 768). The copy reads the output image in the General layout and
   does not wait on the encode path (0.1 ms mean); the encoder waits for it
   before the next render writes the output, and the flow waits before
   reading its lumas.
2. **Motion.** NVIDIA's hardware optical flow (`VK_NV_optical_flow`) estimates
   forward and backward motion between the two retained real pictures, per
   eye, on the same GPU.
3. **Rotation separation (modes 5–9).** Before the flow, the older picture is
   rotationally aligned into the newest picture's camera (same quaternion
   convention and per-eye FOV tangents as the real-frame rewarp,
   `BuildWarpParams`); the flow then holds only object motion and parallax.
   The synthesized picture is rotated into the exact pose of its stamp in the
   same pass, so image and stamp agree.
4. **Warp.** A compute shader inverse-warps the newest picture to the target
   time (extrapolation t ≈ 1.5 of the real interval; interpolation t ≈ 0.5 in
   mode 9, sampling both pictures). Mode 7 adds a 0.5–1.5-pixel motion dead
   zone (still scenes stay byte-identical), colour-guided 2x2 flow
   upsampling, continuous consistency confidence, and fills newly exposed
   borders from the rotated newest picture.
5. **Scheduling (gap filling).** The encode worker waits for the next Present
   until one frame interval after the previous Present plus 1 ms. If none
   arrives, it synthesizes the slot: the target time is the slot time
   measured on the Present clock (not pose stamps, whose rotation-based match
   can jump by a tracking sample), and the stamp is the tracking pose nearest
   to the slot (retried every millisecond for up to 5 ms until that pose has
   arrived; the server only sends pictures whose stamp is in its pose queue).
   It stops after four intervals without a real picture.
6. **Guards.** No fill until four real pictures after a reset or 100 ms pause
   (warm-up); no fill while the newest real picture has layers over the
   scene (SteamVR dashboard); history resets on non-increasing or stale
   stamps.
7. **Mode 9.** When the application skipped a slot, the newest real picture
   is captured but held; the picture halfway between the previous and the
   newest real picture is sent first, then the held real picture one slot
   later. Costs about one slot of latency for object motion; head rotation
   is still corrected by the headset from the pose stamps.

## Comparison with ASW and SSW

ASW = Meta Asynchronous SpaceWarp (PC runtime); SSW = Virtual Desktop
Synchronous SpaceWarp. Public information only; internals may differ.

| | This work (mode 7) | ASW | SSW |
|---|---|---|---|
| Runs on | PC (ALVR driver) | PC (Meta runtime) | headset |
| Platform | Linux | Windows | Windows PC + Quest |
| GPU | NVIDIA RTX | NVIDIA / AMD | any (work on headset) |
| Activation | only missed slots | locks the app to half rate | streams half rate |
| Network | full rate | n/a (Link) | half the frames |
| Motion source | hardware optical flow | GPU encoder motion estimation | video motion vectors on headset |
| Rotation | separated (mode 5+) | ATW | headset reprojection |
| Prediction | extrapolation (mode 9: interpolation) | extrapolation | extrapolation |
| Depth / positional | not available (see below) | ASW 2.0 uses app depth | no |
| Known artifact | shadow behind moving near objects | halo around hands | similar |

Position: comparable to ASW 1.0 in the core; ahead in filling only what is
missing (real pictures are never dropped); behind ASW 2.0 on depth and behind
SSW on bandwidth; the only one of the three on Linux.

## Results

All results on an RTX 3060 (driver 615.71.09), SteamVR 2.18.4, Quest 2,
1440x1504 per eye, 72 Hz. "Repeated vsyncs" = share of 72-Hz display slots
that would show the same image as the previous slot (frames shown by their
timestamps, best vsync phase). "Judder" = median deviation of each displayed
step from its neighbours.

### Offline, recorded Home frames (real-frame evaluator)

Ground truth: full-rate recordings; every other frame is treated as real and
the dropped one is synthesized by the production code path. Means over 28–31
targets; lower is better.

| Window | Repeat | Mode 4 | Mode 5 | Mode 7 | Mode 9 |
|---|---:|---:|---:|---:|---:|
| Yaw sweep, MAE | 2.39–2.58 | 1.029 | 0.878 | 0.761 | **0.685** |
| Yaw sweep, Block8 error | 1.66 | 1.011 | 0.843 | 0.835 | **0.759** |
| Yaw sweep, temporal error | 3.74 | 1.625 | 1.346 | 1.184 | **1.073** |
| Still scene, error vs newest | 0 | — | 0.0702 | **0.0006** | — |
| Fast yaw (45°, 2 s), MAE | 5.85 | — | 2.118 | **1.925** | — |
| Sideways sway, MAE | 0.67 | 0.627 | 0.550 | **0.536** | — |

Fast-yaw worst frames: mode 5 leaves a black hole at the newly exposed
border; mode 7 fills it (its higher Block8 there comes from softened fine
branches). GPU flow+warp per synthesized picture: 3–8 ms median offline;
4–6 ms (mode 7) and 6–8.5 ms (mode 9) logged live on the Quest.

Synthetic checker (512x384, ten scenes) for the warp edge fix: occlusion
revealed-area MAE 32.6 → 24.8, occlusion changed 6.55 → 5.42, fine texture
1.34 → 1.24, others unchanged; all ten quality scenes and recoveries pass.

### Live, mock client (no headset), SteamVR Home

| Case | Received fps | Repeated vsyncs | Judder |
|---|---:|---:|---:|
| Home at 72, no synthesis | 71.9 | 0% (quiet day) / 6–7% (loaded day) | 0.013 / 0.27 px |
| Home throttled to 36, no synthesis | 36.0 | 50% | 1.95 px |
| Home at 36, mode 4 (first working) | 65–69 | 11–12% | 0.34–0.52 px |
| Home at 36, mode 4 slot-aimed (3 runs) | 65–68 | 4.3 / 7.4 / 13.2% | 0.22–0.42 px |
| Home at 36, mode 5 | 72 | 11.6% | 0.324 px |
| Home at 36, mode 7 | 72 | **7.5–9.2%** | **0.24–0.30 px** |
| Home at 36, mode 9 | 64.7 | 13.2% | 0.308 px (latency 80 ms vs 52 ms) |
| Home at 72, mode 4–7 | 71.5–72 | 2–3% (normal mode same day 6–7%) | 0.09–0.16 px |
| Sideways sway, Home at 36: none / mode 4 | | 49.9% / 12.4% | 0.228 / 0.061 px |

Run-to-run variation follows late real Presents from SteamVR (49 to 273 per
40 s run), not the filling itself (>99% of missed slots filled).

### Quest 2 sessions (Home throttled to 36 fps unless noted)

| Session | Mode | Quest fps | Latency | User's observation |
|---|---|---:|---:|---|
| 1 | 0 | 35.9 | 64 ms | hands tremble and lag |
| 1 | 4 | 71.8 | 66 ms | hands smoother; blocks, distortion, image trembles |
| 1 | 4, Home at 72 | 71.8 | 71 ms | smoothest (application at full rate) |
| 2 | 0 | 36.0 | 62 ms | stable; shadow around hands |
| 2 | 4 | 72.0 | 65 ms | blocks while moving; trembles |
| 2 | 5 | 72.0 | 66 ms | much better than 4; blocks when turning; ripples when still |
| 3 | 5 | 72 | — | distorted flashes when turning; menu tremulous |
| 3 | **7** | 72 | — | **surroundings good while moving; blocks and ripples gone**; shadow behind moving hand and menu; a few bad frames at start (fixed afterwards by the warm-up) |
| 4 | 0 | — | — | distant fine; near objects and menus dragged |
| 4 | 7 | — | — | distant fine; near objects and menu drag/shadow |
| 4 | 9 | — | — | less shadow, but the shadow flickers |

Session 4 showed that the drag on near objects is present **without**
synthesis too: it comes from the half-rate application plus the headset's
rotation-only reprojection, not from the synthesis. Extrapolation cannot know
background revealed behind a moving object (it is hidden in both real
pictures); interpolation (mode 9) can, at the cost of latency.

## Depth (hDepthTexture) evaluation

OpenVR's direct-mode `SubmitLayerPerEye_t` carries `hDepthTexture` and
`mProjection` ("depth not always provided"). Depth would allow ASW 2.0-style
positional correction, the main remaining artifact.

Diagnostic: `touch ~/.config/alvr/diag_submit_depth` (then restart SteamVR)
logs to `vrserver.txt` every swap texture set created (format, size), the
first 120 layers per owner plus one in 720, and per-owner totals every 10 s:
`Submit depth summary pid=… comm=… layers=… with_depth=… known_depth=…`.

| Application | Engine / path | Layers reach ALVR as | Layers | With depth |
|---|---|---|---:|---:|
| SteamVR Home | Source 2, native Vulkan | composited by vrcompositor, or Home's own textures | 9,291 / 3,148 | 0 |
| VRChat | Unity (OpenVR), Proton | VRChat.exe textures (pass-through) | 4,998 | 0 (no depth set created) |
| Open Brush | Unity (OpenXR), Proton | OpenBrush.exe textures (pass-through) | 3,242 | **0**, although Unity logs `Depth Submission Mode: Depth24Bit` |

Findings:

- Proton's `vrclient_x64.so` forwards depth (`VRTextureWithDepth_t`) for every
  game except No Man's Sky (`disable_depth_data` compares `SteamGameId` with
  275850) or invalid depth data.
- Open Brush submits depth to SteamVR's OpenXR runtime, and the runtime
  shares depth images with the compositor (`vrclient.so` has shared/injected
  depth image code), but the compositor does **not** pass depth to the
  direct-mode driver: no depth swap set is created for the driver and every
  layer arrives with `hDepthTexture = 0`. No OpenVR driver property requests
  depth. The compositor keeps depth for its own features (`depthReprojection*`
  settings, Motion Smoothing).
- The Linux Open Brush depot does not start VR (`NOXR_GRID`; Steam launches
  `OpenBrush.exe`). Force Proton Experimental in the game's Compatibility
  settings to run the Windows build in VR.
- SteamVR Home renders at 3523x3682 when the client's recommended size is
  2492x2604; SteamVR then composites it into its own 2160x2256 system-layer
  texture ("Scene into System Layer"), so any depth would stay in the
  compositor. VRChat and Open Brush render at the recommended size and pass
  through.

Conclusion: with the current SteamVR, a direct-mode driver does not receive
application depth even when the application submits it. Positional
correction with depth would need another route (e.g. capturing the depth
images inside vrcompositor with a Vulkan layer).

## Limitations and known issues

- NVIDIA RTX only; same-GPU NVENC; foveated encoding must be off.
- Shadow/drag behind moving near objects and the SteamVR menu (also present
  without synthesis); mode 9 reduces it but flickers and adds ~28 ms
  (pipeline median 80 ms vs 52 ms in the mock).
- No depth (see above).
- Fine detail is slightly softened on fast motion.
- Configuration through a file, no dashboard setting yet.
- GPU cost spikes up to 16–20 ms were seen offline under concurrent load
  (13.9-ms slot); live Quest logs stayed at 4–8.5 ms.
- VRChat once crashed SteamVR in a mock session ("Watchdog timeout in thread
  Connection"), leaving the driver blocked by SteamVR safe mode; not
  reproduced in later VRChat sessions.

## Bugs found and fixed along the way

| Symptom | Cause | Fix |
|---|---|---|
| Native motion never active in real sessions | env variables never reached vrserver (Steam starts it) | file switch `diag_native_motion` |
| Gap fills almost never fired | slots counted from the end of the real picture's encode | count from the Present (`job.enqueueTime`) |
| ~150 fill rejections per run | extrapolation timed by pose stamps that jump by a tracking sample | time by Present clocks |
| Jitter with filling on at full rate | synchronous capture on the encode path | deferred capture (General layout copy) |
| Image tremble (Quest) | flow carried head rotation, reprojected again by the headset | rotation separation (mode 5) |
| Blocks and still ripples (Quest) | tile interpolation, flow noise, empty borders | mode 7 |
| Distorted pictures at connect (Quest) | prediction before the cadence settled | four-picture warm-up |
| Viewer showed wrong per-eye colours | ffplay `-fflags nobuffer` dropped the only IDR | removed (viewer only; stream was fine) |
| Driver failed to load on a cold start | `libopenvr_api.so` not found | bundled with `$ORIGIN` lookup |
| Dashboard hung at start | wedged adb server in `adb kill-server` | test scripts stop adb first |

## Why not SteamVR's Motion Smoothing

SteamVR 2.17.10 and 2.18.4 for Linux report Motion Smoothing as unsupported
by design: the capability probe and the NVIDIA optical-flow, NvEncode and CVP
backend wrappers in `vrcompositor` are stubs returning false/null
(`xor eax,eax; ret`). Forcing the capability flag makes the compositor try
the estimator, fail (`Failed to initialize motion estimation system!`) and
disable it again. The orchestration code exists, the platform backends do
not. The virtual-display present path is empty as well. Reimplementing the
missing backends behind Valve's private contract was judged riskier than
synthesizing in ALVR's own driver, where the frames and poses already are.

## Test kit

Scripts live in the MetaHorizonLinux research repository (`scripts/`); the mock client is in this branch:

- **Mock client with a viewer** (`alvr/client_mock`): connects like a
  headset; `MOCK_OUT=<dir>` records the stream (`stream.hevc`, `frames.csv`
  with per-frame stamps, `poses.csv`), shows it in an ffplay window,
  `MOCK_SWEEP=<deg>,<s>` scripts a yaw sweep, `MOCK_SWAY=<m>,<s>` a sideways
  motion.
- **`mock_stream_test.sh`**: restarts ALVR (this branch) and SteamVR Home,
  connects the mock (stream over TCP, since a same-host client cannot share
  the UDP port), records and reports. `MOTION=0..9`, `THROTTLE=1` (Home at
  half rate via SteamVR per-app `framesToThrottle`), `SWEEP`, `SWAY`,
  `VRCHAT=1`.
- **`mock_motion_report.py`**: frame cadence, shifts, judder, 72-Hz display
  simulation.
- **`evaluate_native_motion_quality.sh`** (real-frame evaluator, `--rotation`,
  `--stable`, `--interpolate`): MAE, PSNR, Block8 gradient error, temporal
  error, still-scene error, GPU timings, worst-frame crops.
- **Quest sessions**: ADB starts the ALVR app; voice guidance through
  `spd-say` routed to the "ALVR Audio" sink so it plays in the headset.

## Offline checker

`tests/build_linux_native_motion.sh <out>` builds `timing_test` (CPU
timestamp rules), `image_test` (synthetic offscreen GPU scenes) and
`recording_test` (real-frame evaluator). `tests/run_linux_native_motion.sh
<build> <new-results> --suite quality|stream|smoke [options]` runs
`image_test` from a clean committed build only.

- `quality`: 512x384, ten scenes (stationary, translation, slow
  horizontal/vertical/diagonal, fine texture, rotation, foreground occlusion,
  timestamp jitter, colour flash) plus four timestamp-recovery cases;
  stationary images must stay byte-identical.
- `stream`: same scenes at 2048x2144 per eye; `--max-predict-ms 13.888889`
  gates the maximum prediction time.
- `--compare-flow-iterations` / `--compare-bidirectional-flow`: paired
  in-process comparisons with byte-equality checks; `--candidate-first`
  reverses order; `--warmup-samples N` excludes a timing prefix.

These checks exercise the extrapolation path of mode 1; gap filling and the
rotation modes are measured with `recording_test` on real recordings.

## Next steps

1. Mode 9: remove the flicker of the remaining shadow and reduce latency, or
   drop it.
2. Dashboard setting instead of the file switch.
3. A genuinely heavy application (not only throttled Home).
4. Depth through another route (capture in vrcompositor), if worth it.
5. Foveated encoding and non-NVIDIA GPUs (compute-shader optical flow).
6. Split the synthesis from the Air Link work into a branch based on
   upstream ALVR for review.

References: [Khronos `VK_NV_optical_flow`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_optical_flow.html),
[optical-flow queue and image requirements](https://docs.vulkan.org/spec/latest/chapters/VK_NV_optical_flow/optical_flow.html).
