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
