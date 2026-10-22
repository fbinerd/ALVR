# Native motion offline checks

From the companion MetaHorizonLinux repository, run:

```sh
scripts/evaluate_native_motion_quality.sh out/alvr/mock/RECORDING out/alvr/NEW_RESULT --start 400 --frames 65
```

This builds in the existing `mhl-alvr-native-motion-build` image (no image
rebuild), decodes HEVC to RGBA, keeps alternate real frames, calls the production
`NativeMotion::capture`, `prepare`, and gap-fill `predict` path, and compares
predictions to decoded omitted frames. No ALVR/SteamVR process, ADB operation,
shared configuration or headset is used. Requires NVIDIA optical flow.

`metrics.csv` contains per-target RGB MAE, PSNR, gradient-domain error,
horizontal gradient error at eye-local 8-pixel boundaries, and temporal error
delta (mean absolute difference of signed error on successive predictions).
Temporal error is not motion-compensated; it includes moving image edges.
The repeat baseline uses the newest retained real frame on identical targets.
`summary.json` averages per-target scores, including per-frame PSNR (not a
pooled PSNR). Error units are 8-bit RGB values. Smaller errors are better.
Block8 is a boundary gradient error, not a detector proving tile causation.
Timing separates wall time for prepared flow and warp; optical GPU time may
be unavailable (-1). Wall time includes waits; upload/readback are excluded.

The first eight decoded packet sizes are checked against the frame ledger;
B-frame reordering is rejected. This is a prefix consistency check, not a
proof of every frame's provenance. The selected window must decode completely.
Pose yaw is interpolated from `poses.csv` and retained for later rotation
experiments; current evaluator does not compensate rotation. Present clocks
were not recorded: it models ideal 72 Hz slots, retaining pose timestamps
separately, giving the same gap-fill horizon 1.5. This does not reproduce live
scheduling jitter or the Quest's final rotational reprojection.

Outputs include input/checker hashes, source revision/dirty metadata, metrics,
worst-frame synthesized/truth/4x-difference PPMs and aligned left-eye crops.
Raw RGBA is deleted after success; failed runs retain it for diagnosis.
Recordings with dummy views have identical eyes: they cannot validate stereo
parallax. Ten synthetic scenes and recovery cases remain a separate check:
`tests/build_linux_native_motion.sh` and `tests/run_linux_native_motion.sh`.

## Rotation-separated comparison

Pass `--rotation` to the companion evaluator to align previous luma into the
newest image's pose and rotate the residual prediction into the exact target
pose. It interpolates recorded yaw at the frame's stamped pose time and uses
the mock's `ViewParams::DUMMY` FOV (angles +/-1 radian). This switch is valid
for those mock recordings, not arbitrary recordings with unknown calibration.
The production driver computes full quaternion deltas and actual per-eye FOV.
The old statement above that the evaluator never compensates rotation applies
to its default baseline only.

Mode 4 remains the previous gap filler. Mode 5 selects rotation separation;
mode 6 adds continuous reciprocal-flow / photometric confidence blending.
Use `--confidence` for mode 6 offline; it implies `--rotation`. Mode 6 is an
experiment: it improves the Oct 21 window, but worsens MAE and boundary error
slightly against mode 5 in the Oct 22 window. Do not silently replace mode 5.
The blend uses a photometric cost proxy; native NVIDIA cost values have not
been calibrated into a confidence probability.

`optical_gpu_ms` now survives prepared-flow reuse. If the optical queue lacks
timestamp support, timestamps bracket pack through unpack on the render queue;
that fallback includes queue dependencies. GPU time excludes CPU descheduling
and is not a proof that the display deadline will be met end to end. No
adaptive cap is used: the current recorded samples remain below 13.9 ms in
GPU time; wall-time tails and heavier applications still require evaluation.

Depth diagnostics: create `~/.config/alvr/diag_submit_depth` before SteamVR
starts to log up to 120 layers per submitting process. Metadata is reported
only when the handle belongs to the driver's registered texture map; absent
or unknown depth must not be labeled linear/reversed-Z from projection alone.
The file is default-off; it never changes submitted layers or uses depth.
