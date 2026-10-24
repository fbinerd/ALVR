# Linux direct mode: summary

Short version of
[Linux direct mode: texture synchronization and GPU selection](Linux-direct-mode-texture-sync-and-GPU-selection),
which has the full context, data and method.

## The problem

- ALVR reads each eye texture SteamVR draws, then encodes it.
- It must wait until SteamVR has finished drawing. It does this with SteamVR's
  per-texture semaphore, waiting at most 2 frames (about 28 ms at 72 Hz).
- If the wait runs out, the frame is read anyway. A half-drawn frame can show
  up as a flash or a mix of two frames.

## What we found

| Topic | Result | Status |
|---|---|---|
| When the wait runs out | Application start, loading, heavy frames | Confirmed |
| Cause | The frame is still being drawn; the semaphore import never failed | Confirmed |
| Where | Rare on NVIDIA's driver, more frequent on Mesa (NVK, Intel), most frequent on slow GPUs | Confirmed |
| Skip unfinished frames, 2-frame wait | Heavy scenes froze for up to about 1 s (231 and 207 skips per minute) | Rejected (blind A/B) |
| Skip unfinished frames, 8-frame wait (about 111 ms) | No skips, smooth picture | Adopted (blind A/B) |
| Queue-family ownership transfer | Not needed for a correct picture on NVIDIA or Intel; kept because the Vulkan spec requires it and AMD may need it | Kept |
| Vulkan layer `vkWaitForPresentKHR` | No longer exists; direct mode removed the layer | Ruled out |

**Policy in this branch:** never read an unfinished texture; wait up to 8
frames for all textures of a frame together; skip the frame only after that.

## Still open

| Question | What settles it |
|---|---|
| Does this cause the occasional flashes seen on AMD? | A test on an AMD GPU |
| Vulkan validation on the current code | A run with `ALVR_VULKAN_VALIDATION=1` |
| Latency cost of the longer wait on fast GPUs | Heavy games on NVIDIA |

## GPU selector

Upstream ALVR on Linux renders and encodes on one automatically chosen GPU.
With two GPUs that breaks or wastes hardware.

| Feature | What it does |
|---|---|
| Render GPU | Chooses ALVR's GPU; it must match SteamVR's |
| Encoder GPU and technology | Chooses which GPU encodes, with VAAPI or NVENC |
| Cross-GPU encoding | Encodes on a different GPU than the one rendering |
| Steam restart | Restarts Steam so SteamVR and games follow the chosen GPU |
| Warnings | Reports when something overrides the choice |

| Use case | Status |
|---|---|
| Free the gaming GPU from encoding | Tested: 72 fps at 1440x1504 per eye |
| GPU without an encoder (NVIDIA on nouveau/NVK) | Tested |
| A codec the gaming GPU lacks (for example AV1 from another GPU) | Possible, not tested |
| Laptops with an integrated and a discrete GPU | Possible, not tested on a laptop |
| Test Mesa drivers next to the main GPU | Tested |

| Limit | Detail |
|---|---|
| Cross-GPU copy | Goes through system memory: adds some latency; 72 fps only up to 1440x1504 per eye here |
| Changing the render GPU | Needs a Steam restart |

### On AMD (not tested)

| Setup | Expectation |
|---|---|
| A single AMD GPU | Very likely to work (VAAPI on AMD is the existing path) |
| AMD mixed with another GPU | Likely, unverified |
| NVIDIA renders, AMD encodes | Uncertain |

AMDVLK or amdgpu-pro drivers interfere with SteamVR.

## Practical tips

| Situation | Do |
|---|---|
| Choosing a Mesa GPU (Intel, AMD, NVK) | Set only `MESA_VK_DEVICE_SELECT` / `DRI_PRIME`; do not restrict `VK_DRIVER_FILES`, it stops Proton games such as VRChat from starting |
| NVIDIA GPU | Check the driver, not the vendor: NVK and NVIDIA's driver both report as NVIDIA |
| A `steam` wrapper forces NVIDIA variables | Launch `steam.sh` directly when testing other GPUs |
| Starting SteamVR | Start it through Steam, not from a tool that calls `VR_Init` first; otherwise SteamVR misses Steam's GPU environment |

## Other fix

| Bug | Fix |
|---|---|
| Picking the last entry of an unapplied preset list crashed the dashboard (and SteamVR); every other pick applied the next entry | Index offset corrected |
