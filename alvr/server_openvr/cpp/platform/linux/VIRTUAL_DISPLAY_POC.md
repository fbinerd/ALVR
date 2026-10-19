# Linux virtual display proof

This is a log-only experiment. It does not send compositor images to the
headset. Direct mode remains the default.

Enable the proof with `ALVR_LINUX_VIRTUAL_DISPLAY_POC=1` in the driver process
environment, or with `1` in `~/.config/alvr/virtual_display_poc` before starting
SteamVR. Both the server and compositor processes must see the option.

Before starting a live trial, back up `steamvr.vrsettings`, the ALVR session,
and OpenVR driver registration. Enable these keys in the `steamvr` section of
`~/.local/share/Steam/config/steamvr.vrsettings`:

```json
{
  "enableLinuxVulkanAsync": true,
  "allowFallbackMirrorWindowLinux": true
}
```

On the inspected Linux SteamVR build, `CHmdWindowSDL` is constructed before
the renderer selects its virtual display. If no physical HMD display can be
leased, `allowFallbackMirrorWindowLinux` permits startup to continue through
a desktop window. Without it, startup ends with `CannotDRMLeaseDisplay`
before the virtual-renderer decision. A direct-surface error may still appear
with fallback enabled; the subsequent startup result and `Headset display is
virtual` message determine whether the trial advanced.

The plugin reads and logs these settings; it does not change them. Use 72 Hz
for the local proof clock. Start and verify the ALVR Dashboard before starting
SteamVR. The plugin records its PID, library mappings, component pointer,
callbacks, and IPC-reference results in `~/.config/alvr/vd_plugin.log`.

At the end, archive the logs, restore the saved settings and ALVR session,
remove the proof option if it was previously absent, clear any temporary Quest
refresh-rate override, and return to the normal driver. Reuse the Quest app;
the proof changes no client protocol. Provider loading and the fallback window
alone do not establish Motion Smoothing support.

## Current investigation result (2026-10-18)

Do not repeat the live proof on the inspected SteamVR 2.17.10 Linux build
without a compositor-side change. Its compositor SHA-256 is
`a642dc897b934c9e73e500c1029871e99e5eb13de5c7052b980ef4277e84ab30`.
The fallback trial selected the virtual display and enabled async, but the
renderer failed to acquire an HMD swapchain image, returned a null texture,
and crashed while clearing it. The historical minidump confirms the null
access in the compositor, outside the ALVR plugin.

There is a separate frame-delivery blocker: this Linux compositor's
`DoVirtualDisplayPresent` branch only records profiling markers and does not
call the virtual display provider. Vsync callbacks and successful interface
discovery therefore do not prove a usable frame path. Changing ALVR HMD
properties cannot supply this missing compositor operation. Motion Smoothing
UI availability remains unverified.

The detailed REA addresses, crash context, and recommendation are recorded in
the parent project's `analysis/steamvr-virtual-display-linux.md`. Keep the
experiment opt-in and stop before image/encoder integration. A future trial
requires a SteamVR Linux implementation/fix for virtual-display presentation
or a separately scoped compositor experiment; no binary modification is
included in this proof.
