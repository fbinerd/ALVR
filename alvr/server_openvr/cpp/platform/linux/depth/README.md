# Game depth source (new-files-only integration)

Source-neutral core is an exact copy of MetaHorizonLinux
`src/depth_pipeline/gpu_transport.hpp/.cpp`. Its sync script checks the packet
header hash and refuses peer branches. `depth_packet.h` delegates to the exact
608-byte `DepthPacket.h` already used by the recording evaluator. No synthesis
or existing driver file is edited by this receiver.

`GameDepthSource` is a present-thread RAII registration adapter. Construct it
with Encoder&, the encoder's Vulkan instance/physical/device/queue/family,
Unix socket path, enable-file path and a QueryProvider. It calls the EXISTING
`setDepthSource(std::function<std::optional<NativeMotion::Depth>(uint64_t)>)`.

The consumer owner supplies and retains the binding at an appropriate call
site. Never use an unrelated private VkDevice: returned VkImages must belong
to the encoder's device and queue family. Its source callback gets the exact
ALVR target stamp and must return a query containing a verified mapping to
CLOCK_MONOTONIC, active producer ID, actual per-eye composed pose/FOV, full SBS
layout and aligned tracking space. Missing mapping/geometry rejects the packet.
No arrival-time estimate or STAGE=tracking assumption is implemented.

Default paths should be `~/.config/alvr/depth_receiver.sock` and
`~/.config/alvr/diag_depth_receiver`. Absent/0 disables ingress; 1 enables it
within one second. Socket is 0600, same-UID peers only; an existing path is not
unlinked. Keep its directory private. Existing stereo fallback is unchanged:
an accepted game depth takes precedence; empty callback leaves consumer policy
to Encoder. No automatic production call site is installed in this change.

Receiver's owned R32F image is stereo SBS. It preserves source per-eye depth
resolution/FOV exactly, with no resampling. NativeMotion already accepts any
depth extent. FOV/poses must match the composed colour picture even when its
resolution differs. Flips/crops/overlays/rewarp need a verified mapping before
the provider may assert fullStereoLayout; otherwise reject.

Never destroy the binding from another thread or after the encoder's device.
Destroy it on the present thread after consumer GPU work quiesces and BEFORE
Encoder/VkContext teardown. The callback holds a weak reference; destructor
unregisters. Delivered images remain unwritten through the next present; a
newer copy fence on the same queue protects older consumer reads. Disable
stops ingress while retaining delivered images; blocking drain is teardown
only. Diagnostic readback helpers are not used by the present callback.

Offline test:

```sh
bash tests/build_linux_depth_transport.sh out/depth_transport
out/depth_transport/depth_transport_test \
  /path/to/canonical/recording /path/to/NEW/test-output NVIDIA
```

Audit canonical recording hashes first. This launches only a separate producer
test executable and private Vulkan devices, never SteamVR/ALVR/Quest. It checks
60 complete allocations byte-exact, SBS depth packing and previous-image
lifetime, rejects wrong clocks/time/FOV/pose and exercises exact hook binding.
Positive callback uses an explicitly synthetic aligned packet; it is not a
live-space calibration. Real replay packets retain their original unaligned
recording-space semantics in the raw receiver test.

## Production binding (2026-10-24)

Supersedes the preceding no-call-site checkpoint. ProductionDepthSource.hpp
owns registration and calibrated queries; diag_depth_receiver remains off by
default and refreshes in <=1 second. It reads a private same-UID calibration
record at ~/.config/alvr/depth_calibration. Missing/expired clock/space
measurement rejects depth. No live calibrator or live delivery is claimed.

Merge hooks: Encoder.hpp adds only the depth header/member, prepareGameDepth
method borrowing its VkContext/queue, and destructor reset after nativeMotion
quiesces and before device teardown. OvrDirectModeComponent.h adds a local-eye
snapshot to FrameJob; .cpp snapshots it on enqueue and supplies one immutable
ComposedDepthPicture before enc.present. No synthesis algorithm/policy changes.
Only a single unwarped, non-foveated layer is eligible; exact timestamp, FOV,
pose and source UV mapping must match. A rewarped picture needs depth
reprojection first, which is not implemented. Production query/registration
and unmodified NativeMotion::capture are proved by build_linux_depth_capture.sh
using a test-only optical estimator stub on Intel. Never link that stub into
the driver. The research README has bridge assumptions and reproduction steps.

## Exact source-frame association (2026-10-29)

The opt-in production calibrator now writes clock/origin record **version4**.
It retains measured public OpenXR CLOCK_MONOTONIC timestamps and the measured
tracking/reference transform. Its two clock anchors must be equal: it is NOT
an affine mapping from client/poll/history tags. Packet-v1/full and explicit
packet-v2/depth-only remain608 bytes with unchanged image semantics.

Eight coherent tight public-runtime pairs bootstrap the origin, as before.
Afterward each callback resolves a unique authenticated metadata frame by both
eye poses/FOV, producer/reference, <=100ms coarse runtime-prediction gate and
<=250ms metadata age. A nonzero unique full-pose history tag remains required,
but its numeric value is never extrapolated. Still/duplicate poses reject.
The128-frame metadata cache is bounded; the present thread uses try-lock only.
association_matched/missing/ambiguous/contended count queries, not unique images.
Use receiver accepted/rejected counters for image coverage.

The callback rechecks metadata on deferred retries for the SAME retained
picture. Missing association polls bounded authenticated GPU ingress without
matching/importing/returning a depth image. Valid queries still apply all strict
clock, timestamp, both-eye geometry, source-UV, pose and device checks. Defaults,
mode7 synthesis and image lifetime are unchanged. No Encoder/warp/motion edits.

Offline proof uses tests/build_linux_depth_capture.sh and the common online
producer with MHL_DEPTH_TEST_FRAME_V4=1 MHL_DEPTH_TEST_DEPTH_ONLY=1. Sixty images
are checked exact in the actual callback and NativeMotion::capture;30 attach
late. A forced missing-query interval verifies packets survive without being
accepted early. The optical-flow constructor is test-only stubbed; predict is
never called. This is not live acceptance/quality evidence. A matching capture
layer with record-v4 support is required; do not mix older layer binaries.
