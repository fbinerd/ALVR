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
