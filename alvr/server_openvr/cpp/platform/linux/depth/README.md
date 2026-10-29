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

### Exact payload scanning acceleration (2026-10-29)

Record4 live testing removed clock rejects/resets but missed first-prediction
readiness and did not pass object-judder/repeat gates. Do not treat that run as
a quality success. The next candidate preserves every validation check while
using runtime AVX2/SSE2 unsigned IEEE-word scans (scalar elsewhere) for R32F.
Depth-only payload rows use all four existing validator workers; full-v1 colour
half checks retain their prior semantics. No validation is skipped, no FP math
or match tolerance changed. Nonnegative finite values/+0 pass; negative values,
-0, Inf and NaN reject. Vector loads stay within each row, excluding padding.

Offline:1,134,184 reference-equivalent row cases in each native/SSE2/scalar and
sanitized build;156 actual invalid producer payloads across both eyes/odd/one-row
partitions refused before export, followed by one byte-exact valid image. Native
RTX3060 ABBA64-image transport checks remain exact; validation mean1.50/1.36ms
before vs.55/.61ms after. This timing isolates the CPU export-boundary scan and
is not a live packet-coverage or quality claim. Capture/transport lifetime and
the first-generated-frame attachment cutoff remain unchanged.

### Retired private destination cache (2026-10-29)

One fully retired PRIVATE R32F destination allocation may be cached by extent.
This is not a received FD/export allocation or raw gather slot: those are never
rewritten/recycled by this change. It removes recurring private image creation
without changing the callback signature or source geometry/clock policy.

Keep the original two-delivery lifetime boundary and require the THIRD copy's
same-queue fence before retirement. The second copy's fence may precede consumer
reads submitted during that following present; the third copy is after them.
No present-thread wait is added. Cache capacity is one allocation, resize drops
incompatible cache, disable retains promised delivered images. A reused image
is completely overwritten behind a discard transition; default receiver stays
off. Existing bounded loan/backpressure/ACK and source allocation rules remain.

Offscreen Intel and native RTX3060 proof covers61 exact images, previous-image
contents through resize/disable/reconnect, and an explicitly held GPU interval.
10 new allocations/51 reused (guard includes an extra in-flight image). No reuse
occurs before that held interval completes. A watchdog bounds the test's hold;
it is not present in production. Native before/after median acquisition CPU time
has only a modest reduction; live cadence benefit remains to be established.
No NativeMotion/Encoder/shader/peer branch edits.

### Experimental picture-pose timeline (mode 7 only)

`~/.config/alvr/diag_motion_picture_pose` containing `1` opts in at SteamVR
startup. Absent/0 and every other native-motion mode preserve the legacy path.
This experiment requires a single unwarped layer; it does not change the normal
rotational wire-tag matcher, output stamps, present clock, canonical depth,
matching guards, shaders or source image lifetime.

An output tag selected by rotation is not an actual picture pose during lateral
motion. The job therefore independently retains the submitted head pose,
converted back to raw tracking space, and its unique full-pose history tag.
History alignment uses actual picture geometry. For a generated slot, only the
measured tracking-tag duration `outputTarget - outputSource` is applied to the
actual picture tag. Position is interpolated within a measured tracking bracket
of at most 25 ms; no extrapolation, endpoint clamping or runtime-clock relabeling.
The target packet tag and target orientation retain the existing output path.
Missing/ambiguous picture tags or missing brackets give zero positional warp
for that slot, while rotation continues. Horizons beyond 100 ms fail closed.
This deliberately preserves the picture timeline's latency; it does not claim
to late-warp every real picture to the newest physical head pose.

Five-second `Motion picture pose` logs count actual-geometry frames, mapped
translation queries, missing queries and untagged pictures; they also report
legacy-label position error and picture/output tags. The research controller
`depth_abc_trial.py --picture-pose` snapshots/restores this flag and applies it
to all A/B/C arms. Live quality is still a separate gate.

Offline tests: `tests/linux_motion_picture_pose.cpp` uses the production history
and duration mapping for 4095 variable-delay/interpolated cases, strict negative
queries and the unchanged rotational wire matcher. The recording checker option
`DEPTH_PICTURE_STAMP_TEST=reference|legacy|rebased` injects 9–46 ms labels into
identical retained colour/depth, with a common prefix leaving recorded future
padding. Scoring is unchanged. The rebased case must recover reference pixels;
it is not proof of live cadence or of correct timing in every application.

### Bounded consumer pixel diagnostic

Create an ignored output directory, then put its absolute path on the first
line of `~/.config/alvr/diag_motion_depth_snapshot` before starting SteamVR.
Absent file: no readback buffers, submissions, transfer-source usage changes
or snapshots. The diagnostic saves at most three pairs per motion instance,
only when both retained pictures have depth. It waits for the owned copy,
reads the exact private older/newest RGBA8 and R32F images, restores layouts,
and records their tags, clocks and transform. BGRA8 is exported as RGBA8.
No imported/raw image or resource filename is treated as a live GPU handle.
A GPU timeout retains submitted buffer/memory handles until device teardown.

The `alvr.motion-depth-pair.v1` JSON and four raw files per pair are diagnostic
artifacts, not the 608-byte transport. R32F is view-Z metres, +0 invalid,
side-by-side eyes; colour is the encoded RGBA8 actually retained by synthesis.
Readback can stall the worker: NEVER use this run for cadence, latency or
performance comparisons. The research controller's `--depth-snapshots` snapshots
and restores the flag. `check_motion_depth_pair.py` checks foreground coverage
and spatial agreement. The performance reporter rejects these diagnostic runs.
The offline checker can use `MHL_MOTION_DEPTH_SNAPSHOT=/absolute/output` instead
of editing the user's config. Its twelve exported images must match the source
fixture byte-exact and diagnostic-on/off summary scores must be identical.
