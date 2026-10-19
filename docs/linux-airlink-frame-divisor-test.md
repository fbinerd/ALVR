# Linux Air Link frame divisor diagnostic

This opt-in diagnostic supplies a repeatable frame shortage to Meta's Air Link
client, for comparing headset-side Mobile ASW on and off. It applies only to
the Linux OpenVR direct-mode encoder while Air Link is streaming. A connected
native ALVR video client disables the diagnostic.

Write a positive integer to ALVR's configuration directory (normally
`~/.config/alvr/airlink_test_frame_divisor`):

```sh
printf '2\n' > ~/.config/alvr/airlink_test_frame_divisor
```

The driver reads the file about once per second while presenting valid source
frames. `N > 1` admits one of every N source pictures before rendering and
encoding. At a steady 72 Hz source, N=2 should deliver about 36 encoded
pictures per second; the existing measured refresh correction makes the exact
target about 35.91. This deliberately reduces video delivery without changing
the display refresh, tracking, audio, session negotiation, or ASW state.

Deleting the file, writing 1, or supplying zero, invalid/unreadable contents or
an out-of-range integer restores normal admission. Effective divisor changes
are logged as `Air Link test frame divisor: 1 -> 2`. The first picture after
a change is admitted immediately. Let the setting settle for two seconds
before collecting a measurement window.

Skipping before encoding preserves the HEVC reference chain. Encoded pictures
keep their original pose timestamps, packet frame IDs remain consecutive
(including the existing startup numbering), and their presentation times use
N display slots between pictures. On a live divisor change the first picture
uses its observed slot before the new cadence takes effect. The one-picture
worker mailbox can still drop additional source pictures during a real stall;
N specifies admission, not a guarantee of delivery under overload.

## Pending live trial

Use Meta's Air Link app, with preset A: HEVC, 2048x2144, foveated encoding off,
preferred/display refresh 72 Hz. Do not launch or reinstall `alvr.client.dev`.
Start Dashboard before SteamVR and verify an ordinary 72 Hz Link stream first.
Back up settings and any pre-existing divisor file. Keep the headset worn;
do not use the proximity-sensor override.

Set N=2, verify approximately 36 real video pictures per second, and compare
30-second windows with `debug.ovr.xrs.swc=1,0,1,0`. Announce each step through
the host's speech output before changing a setting or measuring. Capture the
actual `xrstreaming` PID's VrApi `FPS`, `Stale`, `ASW`, `Type`, `E`, and `D`,
plus ALVR frame rate/latency and the ASW state-change logs. `ASW` includes
positional timewarp activity and alone does not prove synthesized motion;
`E` and `D` are extrapolation-factor mean/RMS pairs, not frame counters.
Optional short screen recordings belong in separate, unmeasured windows.

Finally remove/restore the divisor file, clear the temporary Quest properties,
restore settings and confirm normal visible 72 Hz streaming. This build has
only been compiled; no live validation has been performed.
