use std::fs;
use std::path::Path;
use std::process::Command;
use std::thread;
use std::time::Duration;

use alvr_common::{
    anyhow::{Result, bail},
    debug, error, info, warn,
};
use sysinfo::Process;

pub fn launch_steamvr_with_steam() {
    if running_steam_pids().is_empty()
        && let Some(adapter) = selected_render_gpu_adapter()
    {
        let Some(steam_script) = steam_script_path() else {
            error!("Render GPU is selected, but Steam's steam.sh script could not be located");
            return;
        };
        info!(
            "Starting Steam and SteamVR on ALVR Render GPU {}",
            adapter.get_info().name
        );
        let mut command = Command::new(steam_script);
        command.arg("steam://rungameid/250820");
        apply_render_gpu_environment(&mut command, &adapter);
        if let Err(e) = super::spawn_and_reap(&mut command) {
            error!("Failed to launch SteamVR with the selected Render GPU: {e}");
        }
        return;
    }
    if let Err(e) = super::spawn_and_reap(Command::new("steam").args(["steam://rungameid/250820"]))
    {
        error!("Failed to launch SteamVR through Steam: {e}");
    }
}

// An already-running Steam process cannot have its environment changed. When
// the user explicitly asks to apply ALVR's Render GPU, shut Steam down cleanly
// and start a fresh client with the selected GPU environment.
pub fn restart_steam_with_render_gpu() {
    let Some(adapter) = selected_render_gpu_adapter() else {
        error!("Cannot apply the Render GPU: no explicit Vulkan GPU selection matches");
        return;
    };
    let gpu_name = adapter.get_info().name;
    thread::spawn(move || {
        info!("Closing Steam so it can restart on ALVR's selected Render GPU ({gpu_name})");
        if let Err(e) = Command::new("steam").arg("-shutdown").status() {
            error!("Could not request a graceful Steam shutdown: {e}");
            return;
        }

        for _ in 0..60 {
            if running_steam_pids().is_empty() {
                break;
            }
            thread::sleep(Duration::from_secs(1));
        }
        if !running_steam_pids().is_empty() {
            error!("Steam did not close within 60 seconds; leaving it running");
            return;
        }

        // Re-enter the normal launcher so the current ALVR driver is
        // registered even when the confirmation interrupted the Launch action.
        let mut launcher = super::LAUNCHER.lock();
        launcher.restart_steamvr();
    });
}

pub fn render_gpu_conflict_for_confirmation() -> Option<(String, String)> {
    if running_steam_pids().is_empty() {
        return None;
    }
    let adapter = selected_render_gpu_adapter()?;
    let conflicts = conflicting_steam_overrides(&adapter);
    if conflicts.is_empty() {
        return None;
    }
    Some((
        adapter.get_info().name,
        format!(
            "{}\n\nSteam will restart with:\n{}",
            conflicts.join("\n"),
            render_gpu_launch_options(&adapter)
        ),
    ))
}

fn selected_render_gpu_adapter() -> Option<wgpu::Adapter> {
    let selector = crate::data_sources::get_read_only_local_session()
        .settings()
        .video
        .encoder_config
        .linux_gpu
        .render_gpu
        .clone()
        .filter(|selector| !selector.trim().is_empty())?;
    let wanted = selector.trim().to_lowercase();
    wgpu::Instance::new(&wgpu::InstanceDescriptor {
        backends: wgpu::Backends::VULKAN,
        ..Default::default()
    })
    .enumerate_adapters(wgpu::Backends::VULKAN)
    .into_iter()
    .filter(|adapter| {
        let device_type = adapter.get_info().device_type;
        device_type == wgpu::DeviceType::DiscreteGpu
            || device_type == wgpu::DeviceType::IntegratedGpu
    })
    .enumerate()
    .find(|(index, adapter)| {
        let info = adapter.get_info();
        wanted == index.to_string() || info.name.to_lowercase().contains(&wanted)
    })
    .map(|(_, adapter)| adapter)
}

fn running_steam_pids() -> Vec<u32> {
    fs::read_dir("/proc")
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|entry| {
            (fs::read_to_string(entry.path().join("comm")).ok()?.trim() == "steam")
                .then(|| entry.file_name().to_string_lossy().parse().ok())
                .flatten()
        })
        .collect()
}

fn steam_script_path() -> Option<std::path::PathBuf> {
    let home = std::env::var_os("HOME")?;
    [".steam/root/steam.sh", ".local/share/Steam/steam.sh"]
        .iter()
        .map(|relative| std::path::PathBuf::from(&home).join(relative))
        .find(|path| path.is_file())
}

fn apply_render_gpu_environment(command: &mut Command, adapter: &wgpu::Adapter) {
    for name in GPU_OVERRIDE_VARS {
        command.env_remove(name);
    }
    command.envs(render_gpu_environment(adapter));
}

// Use the same environment for launch, validation, and the confirmation UI.
// Explicit selection must not depend on firmware's primary GPU or on the
// desktop GPU. In particular, an empty Steam environment does not select the
// GPU requested by ALVR on a hybrid system.
fn render_gpu_environment(adapter: &wgpu::Adapter) -> Vec<(String, String)> {
    let mut variables = vec![];
    let mut set = |name: &str, value: &str| {
        variables.push((name.to_owned(), value.to_owned()));
    };
    let info = adapter.get_info();
    if is_nvidia_proprietary(&info) {
        set("__GLX_VENDOR_LIBRARY_NAME", "nvidia");
        set("__NV_PRIME_RENDER_OFFLOAD", "1");
        // Keep this even when pinning the host ICD. Steam's runtime can
        // rewrite ICD paths; this layer still hides the integrated device.
        set("__VK_LAYER_NV_optimus", "NVIDIA_only");
        let base = "/usr/share/vulkan/icd.d/nvidia_icd";
        let path = [format!("{base}.json"), format!("{base}.x86_64.json")]
            .into_iter()
            .find(|path| Path::new(path).is_file());
        if let Some(path) = path {
            set("VK_DRIVER_FILES", &path);
        }
    } else {
        let mesa_egl = "/usr/share/glvnd/egl_vendor.d/50_mesa.json";
        variables.extend(mesa_render_gpu_environment(
            info.vendor,
            info.device,
            is_x11_session(),
            Path::new(mesa_egl).is_file().then_some(mesa_egl),
        ));
        // MESA_VK_DEVICE_SELECT (with "!") already hides the other GPUs.
        // Limiting VK_DRIVER_FILES to one host ICD as well breaks Proton
        // games (VRChat: no graphics device on Intel), so leave it alone.
    }
    variables
}

// SteamVR and ALVR render with Vulkan. Forcing Intel's GLX/EGL vendor in
// NVIDIA-primary X11 also changes Steam's UI and hardware queries, and has
// crashed the X server inside nvidia_drv.so. Select Intel for Vulkan without
// changing the desktop's OpenGL/EGL vendor. Keep the validated Wayland path.
fn mesa_render_gpu_environment(
    vendor: u32,
    device: u32,
    x11: bool,
    mesa_egl: Option<&str>,
) -> Vec<(String, String)> {
    let ids = format!("{vendor:04x}:{device:04x}");
    let mut variables = vec![("MESA_VK_DEVICE_SELECT".into(), format!("{ids}!"))];
    if vendor != 0x8086 || !x11 {
        variables.push(("DRI_PRIME".into(), ids));
        variables.push(("__GLX_VENDOR_LIBRARY_NAME".into(), "mesa".into()));
        if let Some(path) = mesa_egl {
            variables.push(("__EGL_VENDOR_LIBRARY_FILENAMES".into(), path.into()));
        }
    }
    variables
}

fn is_x11_session() -> bool {
    match std::env::var("XDG_SESSION_TYPE").as_deref() {
        Ok("x11") => true,
        Ok("wayland") => false,
        _ => std::env::var_os("WAYLAND_DISPLAY").is_none() && std::env::var_os("DISPLAY").is_some(),
    }
}

pub fn apply_selected_render_gpu_environment(command: &mut Command) -> bool {
    let Some(adapter) = selected_render_gpu_adapter() else {
        return false;
    };
    apply_render_gpu_environment(command, &adapter);
    true
}

pub fn terminate_process(process: &Process) {
    process.kill_with(sysinfo::Signal::Term);
}

// Release ALVR wraps SteamVR's vrcompositor to inject its capture layer, and
// nothing removes that wrap when switching to this build. Direct mode drives
// the display itself and must not run behind the wrapper, with it in place
// SteamVR finds no HMD and fails with errors like 108. Undo it on every
// launch so a stale wrap from a previous install cannot break this one.
pub fn maybe_unwrap_vrcompositor_launcher() -> Result<()> {
    let steamvr_bin_dir = alvr_server_io::steamvr_root_dir()?
        .join("bin")
        .join("linux64");
    let launcher_path = steamvr_bin_dir.join("vrcompositor");

    // Not a symlink means nothing wrapped it, so there is nothing to undo.
    if fs::read_link(&launcher_path).is_err() {
        debug!("vrcompositor is not wrapped, nothing to undo");
        return Ok(());
    }

    // Newer builds move the real binary into an alvr subdirectory, older ones
    // rename it in place. Handle both so any install can be recovered.
    let candidates = [
        alvr_filesystem::original_vrcompositor_path(&steamvr_bin_dir),
        steamvr_bin_dir.join("vrcompositor.real"),
    ];
    let Some(real_path) = candidates.into_iter().find(|path| path.is_file()) else {
        bail!(
            "vrcompositor is a symlink but Valve's binary could not be found next to it. \
            Verify integrity of SteamVR files to restore it."
        );
    };

    fs::remove_file(&launcher_path)?;
    fs::rename(&real_path, &launcher_path)?;

    info!(
        "Removed a leftover vrcompositor wrapper and restored {}",
        launcher_path.display()
    );

    Ok(())
}

const MIRROR_BEGIN: &str = "# ALVR Intel mirror compatibility begin\n";
const MIRROR_END: &str = "# ALVR Intel mirror compatibility end\n";
const MIRROR_BODY: &str = r#"# A virtual HMD does not need a desktop mirror. Some Intel/Mesa Wayland
# mirror swapchains fail when NVIDIA owns the primary display. Scope Mesa's
# headless workaround to this compositor process; never to Steam or games.
case "${MESA_VK_DEVICE_SELECT-}" in
    8086:*)
        if [ -n "${WAYLAND_DISPLAY-}" ] && [ "${MESA_VK_WSI_HEADLESS_SWAPCHAIN-}" != 1 ]; then
            for alvr_gpu_device in /sys/class/drm/card[0-9]*/device; do
                if [ "$(cat "$alvr_gpu_device/vendor" 2>/dev/null)" = 0x10de ] &&
                   [ "$(cat "$alvr_gpu_device/boot_vga" 2>/dev/null)" = 1 ]; then
                    export MESA_VK_WSI_HEADLESS_SWAPCHAIN=1
                    echo '[ALVR] Using Mesa headless compositor mirror: Intel render, NVIDIA primary' >&2
                    break
                fi
            done
            unset alvr_gpu_device
        fi
        ;;
esac
"#;

// A small managed section in Valve's existing shell launcher avoids wrapping
// or renaming its ELF binary. Preserve the rest of the script byte for byte.
fn mirror_launcher_script(script: &str, enabled: bool) -> Result<String> {
    if !script.starts_with("#!") {
        bail!("SteamVR compositor launcher is not a shell script");
    }
    let Some(newline) = script.find('\n') else {
        bail!("SteamVR compositor launcher has no body");
    };
    let (shebang, mut body) = script.split_at(newline + 1);
    if let Some(previous) = body.strip_prefix(MIRROR_BEGIN) {
        let Some(end) = previous.find(MIRROR_END) else {
            bail!("Incomplete ALVR mirror compatibility section; leaving launcher untouched");
        };
        body = &previous[end + MIRROR_END.len()..];
    }
    if body.contains(MIRROR_BEGIN) || body.contains(MIRROR_END) {
        bail!("Unexpected ALVR mirror compatibility marker; leaving launcher untouched");
    }
    Ok(if enabled {
        format!("{shebang}{MIRROR_BEGIN}{MIRROR_BODY}{MIRROR_END}{body}")
    } else {
        format!("{shebang}{body}")
    })
}

pub fn configure_compositor_mirror() -> Result<()> {
    let path = alvr_server_io::steamvr_root_dir()?.join("bin/linux64/vrcompositor-launcher.sh");
    if fs::symlink_metadata(&path)?.file_type().is_symlink() {
        bail!("SteamVR compositor shell launcher is a custom symlink; leaving it untouched");
    }
    let enabled =
        selected_render_gpu_adapter().is_some_and(|adapter| adapter.get_info().vendor == 0x8086);
    let original = fs::read_to_string(&path)?;
    let updated = mirror_launcher_script(&original, enabled)?;
    if original == updated {
        return Ok(());
    }
    let temporary = path.with_extension(format!("alvr-{}.tmp", std::process::id()));
    let result = (|| -> Result<()> {
        fs::write(&temporary, updated)?;
        fs::set_permissions(&temporary, fs::metadata(&path)?.permissions())?;
        if fs::read_to_string(&path)? != original {
            bail!("SteamVR launcher changed during preparation; leaving it untouched");
        }
        fs::rename(&temporary, &path)?;
        Ok(())
    })();
    if result.is_err() {
        fs::remove_file(&temporary).ok();
    }
    result?;
    info!(
        "{} Intel compositor mirror compatibility (Wayland with NVIDIA primary only)",
        if enabled { "Prepared" } else { "Removed" }
    );
    Ok(())
}

#[derive(PartialEq)]
enum DeviceInfo {
    // NVIDIA GPU on NVIDIA's proprietary driver.
    Nvidia,
    // NVIDIA GPU on Mesa's NVK (nouveau kernel driver): selected like the
    // other Mesa GPUs, and no NVENC.
    NvidiaMesa { device_type: wgpu::DeviceType },
    Amd { device_type: wgpu::DeviceType },
    Intel { device_type: wgpu::DeviceType },
    Unknown,
}

pub fn linux_hardware_checks() {
    let wgpu_adapters = wgpu::Instance::new(&wgpu::InstanceDescriptor {
        backends: wgpu::Backends::VULKAN,
        ..Default::default()
    })
    .enumerate_adapters(wgpu::Backends::VULKAN);
    let device_infos = wgpu_adapters
        .iter()
        .filter(|adapter| {
            adapter.get_info().device_type == wgpu::DeviceType::DiscreteGpu
                || adapter.get_info().device_type == wgpu::DeviceType::IntegratedGpu
        })
        .map(|adapter| {
            let vendor = match adapter.get_info().vendor {
                0x10de if is_nvidia_proprietary(&adapter.get_info()) => DeviceInfo::Nvidia,
                0x10de => DeviceInfo::NvidiaMesa {
                    device_type: adapter.get_info().device_type,
                },
                0x1002 => DeviceInfo::Amd {
                    device_type: adapter.get_info().device_type,
                },
                0x8086 => DeviceInfo::Intel {
                    device_type: adapter.get_info().device_type,
                },
                _ => DeviceInfo::Unknown,
            };

            (adapter, vendor)
        })
        .collect::<Vec<_>>();

    // "Render GPU" setting: the GPU SteamVR and the games should render on.
    // Matched like the streamer does (index or part of the name); the
    // streamer's own device list may be ordered differently, so a name is
    // the reliable selector.
    let render_gpu = crate::data_sources::get_read_only_local_session()
        .settings()
        .video
        .encoder_config
        .linux_gpu
        .render_gpu
        .clone()
        .filter(|selector| !selector.trim().is_empty());
    let selected_adapter = render_gpu.as_ref().and_then(|selector| {
        let wanted = selector.trim().to_lowercase();
        let found = wgpu_adapters.iter().enumerate().find(|(index, adapter)| {
            wanted == index.to_string() || adapter.get_info().name.to_lowercase().contains(&wanted)
        });
        if found.is_none() {
            warn!(
                "Render GPU \"{selector}\" matches no Vulkan adapter; using the automatic choice"
            );
        }
        found.map(|(_, adapter)| adapter)
    });

    linux_gpu_checks(&device_infos, selected_adapter);
    linux_encoder_checks(&device_infos);
}

// NVIDIA GPU driven by NVIDIA's proprietary driver, as opposed to Mesa's NVK
// on the nouveau kernel driver. The two need different launch options.
fn is_nvidia_proprietary(info: &wgpu::AdapterInfo) -> bool {
    info.vendor == 0x10de
        && info.driver != "NVK"
        && !info.driver_info.contains("Mesa")
        && !info.name.contains("NVK")
}

// Variables that restrict which GPU Vulkan / OpenGL programs use. Steam
// passes them on to SteamVR and the games it starts.
const GPU_OVERRIDE_VARS: &[&str] = &[
    "VK_DRIVER_FILES",
    "VK_ICD_FILENAMES",
    "VK_ADD_DRIVER_FILES",
    "VK_LOADER_DRIVERS_SELECT",
    "VK_LOADER_DRIVERS_DISABLE",
    "VK_LOADER_LAYERS_DISABLE",
    "DISABLE_LAYER_NV_OPTIMUS_1",
    "NODEVICE_SELECT",
    "__VK_LAYER_NV_optimus",
    "MESA_VK_DEVICE_SELECT",
    "DRI_PRIME",
    "__NV_PRIME_RENDER_OFFLOAD",
    "__NV_PRIME_RENDER_OFFLOAD_PROVIDER",
    "__GLX_VENDOR_LIBRARY_NAME",
    "__EGL_VENDOR_LIBRARY_FILENAMES",
];

// GPU-restricting variables in the environment of the running Steam client
// (the first process named "steam"), as (name, value).
fn steam_gpu_override_env() -> Vec<(String, String)> {
    let Ok(entries) = fs::read_dir("/proc") else {
        return vec![];
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let is_steam = fs::read_to_string(path.join("comm"))
            .map(|comm| comm.trim() == "steam")
            .unwrap_or(false);
        if !is_steam {
            continue;
        }
        let Ok(environ) = fs::read(path.join("environ")) else {
            continue;
        };
        return environ
            .split(|byte| *byte == 0)
            .filter_map(|pair| {
                let pair = String::from_utf8_lossy(pair);
                let (name, value) = pair.split_once('=')?;
                GPU_OVERRIDE_VARS
                    .contains(&name)
                    .then(|| (name.to_string(), value.to_string()))
            })
            .collect();
    }
    vec![]
}

// A running Steam client must have the explicit selection applied. Absence
// of overrides is not proof of compatibility: its default changes with the
// primary GPU chosen by firmware and the desktop session.
fn conflicting_steam_overrides(adapter: &wgpu::Adapter) -> Vec<String> {
    gpu_environment_changes(&render_gpu_environment(adapter), &steam_gpu_override_env())
}

fn gpu_environment_changes(
    expected: &[(String, String)],
    current: &[(String, String)],
) -> Vec<String> {
    let mut changes = vec![];
    for (name, wanted) in expected {
        match current.iter().find(|(key, _)| key == name) {
            Some((_, value)) if value == wanted => {}
            Some((_, value)) => changes.push(format!("{name}={value} must become {wanted}")),
            None => changes.push(format!("{name} is not set (required: {wanted})")),
        }
    }
    for (name, value) in current {
        if !value.is_empty() && !expected.iter().any(|(key, _)| key == name) {
            changes.push(format!("Remove {name}={value}"));
        }
    }
    changes
}

// Launch options that make SteamVR / a game render on `adapter`.
fn render_gpu_launch_options(adapter: &wgpu::Adapter) -> String {
    render_gpu_environment(adapter)
        .into_iter()
        .map(|(name, value)| format!("{name}={value}"))
        .collect::<Vec<_>>()
        .join(" ")
}

fn linux_gpu_checks(
    device_infos: &[(&wgpu::Adapter, DeviceInfo)],
    selected_adapter: Option<&wgpu::Adapter>,
) {
    let have_intel_igpu = device_infos.iter().any(|gpu| {
        gpu.1
            == DeviceInfo::Intel {
                device_type: wgpu::DeviceType::IntegratedGpu,
            }
    });
    debug!("have_intel_igpu: {}", have_intel_igpu);
    let have_amd_igpu = device_infos.iter().any(|gpu| {
        gpu.1
            == DeviceInfo::Amd {
                device_type: wgpu::DeviceType::IntegratedGpu,
            }
    });
    debug!("have_amd_igpu: {}", have_amd_igpu);

    let have_igpu = have_intel_igpu || have_amd_igpu;
    debug!("have_igpu: {}", have_igpu);

    let have_nvidia_dgpu = device_infos.iter().any(|gpu| gpu.1 == DeviceInfo::Nvidia);
    debug!("have_nvidia_dgpu: {}", have_nvidia_dgpu);

    let have_amd_dgpu = device_infos.iter().any(|gpu| {
        gpu.1
            == DeviceInfo::Amd {
                device_type: wgpu::DeviceType::DiscreteGpu,
            }
    });
    debug!("have_amd_dgpu: {}", have_amd_dgpu);

    if have_amd_igpu || have_amd_dgpu {
        let is_any_amd_driver_invalid = device_infos.iter().any(|gpu| {
            info!("Driver name: {}", gpu.0.get_info().driver);
            match gpu.0.get_info().driver.as_str() {
                "AMD proprietary driver" | "AMD open-source driver" => true, // AMDGPU-Pro | AMDVLK
                _ => false,
            }
        });
        if is_any_amd_driver_invalid {
            error!(
                "Amdvlk or amdgpu-pro vulkan drivers detected, SteamVR may not function properly. \
            Please remove them or make them unavailable for SteamVR and games you're trying to launch.\n\
            For more detailed info visit the wiki: \
            https://github.com/alvr-org/ALVR/wiki/Linux-Troubleshooting#artifacting-no-steamvr-overlay-or-graphical-glitches-in-streaming-view"
            )
        }
    }

    let have_intel_dgpu = device_infos.iter().any(|gpu| {
        gpu.1
            == DeviceInfo::Intel {
                device_type: wgpu::DeviceType::DiscreteGpu,
            }
    });
    let have_nvk_dgpu = device_infos.iter().any(|gpu| {
        gpu.1
            == DeviceInfo::NvidiaMesa {
                device_type: wgpu::DeviceType::DiscreteGpu,
            }
    });
    debug!("have_nvk_dgpu: {}", have_nvk_dgpu);
    debug!("have_intel_dgpu: {}", have_intel_dgpu);

    let steamvr_root_dir = match alvr_server_io::steamvr_root_dir() {
        Ok(dir) => dir,
        Err(e) => {
            error!(
                "Couldn't find OpenVR or SteamVR files. \
                Please make sure you have installed and ran SteamVR at least once. \
                Or if you're using Flatpak Steam, make sure to use ALVR Dashboard from Flatpak ALVR. {e}"
            );
            return;
        }
    };

    let vrmonitor_path_string = steamvr_root_dir
        .join("bin")
        .join("vrmonitor.sh")
        .into_os_string()
        .into_string()
        .unwrap();
    debug!("vrmonitor_path: {}", vrmonitor_path_string);

    let steamvr_opts = "For functioning VR you need to put the following line into SteamVR's launch options and restart it:";
    let game_opts = "And this similar line to the launch options of ALL games that you're trying to launch from steam:";

    let mut vrmonitor_path_written = false;
    let steam_overrides = steam_gpu_override_env();
    if !steam_overrides.is_empty() {
        info!(
            "Steam's GPU-restricting environment (inherited by SteamVR and games): {}",
            steam_overrides
                .iter()
                .map(|(name, value)| format!("{name}={value}"))
                .collect::<Vec<_>>()
                .join(" ")
        );
    }
    if let Some(adapter) = selected_adapter {
        let conflicts = conflicting_steam_overrides(adapter);
        if !conflicts.is_empty() && !running_steam_pids().is_empty() {
            warn!(
                "Render GPU is {}, but the running Steam client has not applied its GPU \
                environment: {}. Use Launch/Restart to review and apply the selected Render GPU.",
                adapter.get_info().name,
                conflicts.join(" ")
            );
        }
        let options = render_gpu_launch_options(adapter);
        info!(
            "Render GPU setting: {} ({:04x}:{:04x}), driver {} {}",
            adapter.get_info().name,
            adapter.get_info().vendor,
            adapter.get_info().device,
            adapter.get_info().driver,
            adapter.get_info().driver_info
        );
        if adapter.get_info().vendor == 0x8086 && is_x11_session() {
            info!(
                "Intel render selection on X11 applies to Vulkan; OpenGL/EGL keep the desktop \
                default to avoid changing Steam's UI graphics vendor."
            );
        }
        warn!("{steamvr_opts}\n{options} {vrmonitor_path_string} %command%");
        warn!("{game_opts}\n{options} %command%");
        return;
    }
    if have_igpu {
        if have_nvidia_dgpu {
            let base_path = "/usr/share/vulkan/icd.d/nvidia_icd";
            let nvidia_vk_override_path = if Path::new(&format!("{base_path}.json")).exists() {
                format!("VK_DRIVER_FILES={base_path}.json")
            } else if Path::new(&format!("{base_path}.x86_64.json")).exists() {
                format!("VK_DRIVER_FILES={base_path}.x86_64.json")
            } else {
                "__VK_LAYER_NV_optimus=NVIDIA_only".to_string()
            };
            let nv_options = format!(
                "__GLX_VENDOR_LIBRARY_NAME=nvidia __NV_PRIME_RENDER_OFFLOAD=1 {nvidia_vk_override_path}"
            );

            warn!("{steamvr_opts}\n{nv_options} {vrmonitor_path_string} %command%");
            warn!("{game_opts}\n{nv_options} %command%");

            vrmonitor_path_written = true;
        } else if have_intel_dgpu || have_amd_dgpu || have_nvk_dgpu {
            warn!("{steamvr_opts}\nDRI_PRIME=1 {vrmonitor_path_string} %command%");
            warn!("{game_opts}\nDRI_PRIME=1 %command%");
            vrmonitor_path_written = true;
        } else {
            warn!(
                "Beware, using just integrated graphics might lead to very poor performance in SteamVR and VR games."
            );
            warn!(
                "For more information, please refer to the wiki: https://github.com/alvr-org/ALVR/wiki/Linux-Troubleshooting"
            )
        }
    }
    if !vrmonitor_path_written {
        warn!(
            "Make sure you have put the following line in your SteamVR launch options and restart it:\n\
            {vrmonitor_path_string} %command%"
        )
    }
}

fn linux_encoder_checks(device_infos: &[(&wgpu::Adapter, DeviceInfo)]) {
    for device_info in device_infos {
        match device_info.1 {
            DeviceInfo::Nvidia => {
                match nvml_wrapper::Nvml::init() {
                    Ok(nvml) => {
                        let device_count = nvml.device_count().unwrap();
                        debug!("nvml device count: {}", device_count);
                        // fixme: on multi-gpu nvidia system will do it twice,
                        for index in 0..device_count {
                            match nvml.device_by_index(index) {
                                Ok(device) => {
                                    debug!("nvml device name: {}", device.name().unwrap());
                                    probe_nvenc_encoder_profile(
                                        &device,
                                        nvml_wrapper::enum_wrappers::device::EncoderType::H264,
                                        "H264",
                                    );
                                    probe_nvenc_encoder_profile(
                                        &device,
                                        nvml_wrapper::enum_wrappers::device::EncoderType::HEVC,
                                        "HEVC",
                                    );
                                    // todo: probe for AV1 when will be available in nvml-wrapper
                                }
                                Err(e) => {
                                    error!("Failed to acquire NVML device with error: {}", e)
                                }
                            }
                        }
                    }
                    Err(e) => {
                        alvr_common::show_e(format!("Can't initialize NVML engine, error: {e}."))
                    }
                }
            }
            DeviceInfo::Amd { device_type: _ } | DeviceInfo::Intel { device_type: _ } => {
                let libva_display_open = libva::Display::open();
                if let Some(libva_display) = libva_display_open {
                    if let Ok(vendor_string) = libva_display.query_vendor_string() {
                        info!("GPU Encoder vendor: {}", vendor_string);
                    }
                    probe_libva_encoder_profile(
                        &libva_display,
                        libva::VAProfile::VAProfileH264Main,
                        "H264",
                        true,
                    );
                    probe_libva_encoder_profile(
                        &libva_display,
                        libva::VAProfile::VAProfileHEVCMain,
                        "HEVC",
                        true,
                    );
                    probe_libva_encoder_profile(
                        &libva_display,
                        libva::VAProfile::VAProfileAV1Profile0,
                        "AV1",
                        false,
                    );
                } else {
                    alvr_common::show_e(
                        "Couldn't find VA-API runtime on system, \
                        you unlikely to have hardware encoding. \
                        Please install VA-API runtime for your distribution \
                        and make sure it works (Manjaro, Fedora affected). \
                        For detailed advice, check wiki: \
                        https://github.com/alvr-org/ALVR/wiki/Linux-Troubleshooting#failed-to-create-vaapi-encoder",
                    );
                }
            }
            DeviceInfo::NvidiaMesa { device_type: _ } => {
                // NVENC needs NVIDIA's proprietary driver (CUDA); nouveau/NVK
                // has no encoder ALVR can use.
                warn!(
                    "{} runs on Mesa's NVK (nouveau), which has no NVENC: set \"Encoder GPU\" to \
                    another GPU with VAAPI, or use NVIDIA's proprietary driver",
                    device_info.0.get_info().name
                );
            }
            _ => alvr_common::show_e(
                "Couldn't determine gpu for hardware encoding. \
            You will likely fallback to software encoding.",
            ),
        }
    }
}

fn probe_nvenc_encoder_profile(
    device: &nvml_wrapper::Device,
    encoder_type: nvml_wrapper::enum_wrappers::device::EncoderType,
    profile_name: &str,
) {
    match device.encoder_capacity(encoder_type) {
        Ok(_) => {
            info!("GPU supports {} profile.", profile_name);
        }
        Err(e) => {
            if matches!(e, nvml_wrapper::error::NvmlError::NotSupported) {
                alvr_common::show_e(format!(
                    "Your NVIDIA gpu doesn't support {profile_name}. Please make sure CUDA is installed properly. Error: {e}"
                ))
            } else {
                error!("{}", e)
            }
        }
    }
}

fn probe_libva_encoder_profile(
    libva_display: &std::rc::Rc<libva::Display>,
    profile_type: libva::VAProfile::Type,
    profile_name: &str,
    is_critical: bool,
) {
    let profile_probe = libva_display.query_config_entrypoints(profile_type);
    let mut message = String::new();
    if profile_probe.is_err() {
        message = format!("Couldn't find {profile_name} encoder.");
    } else if let Ok(profile) = profile_probe {
        if profile.is_empty() {
            message = format!("{profile_name} profile entrypoint is empty.");
        } else if !profile.contains(&libva::VAEntrypoint::VAEntrypointEncSlice) {
            message = format!("{profile_name} profile does not contain encoding entrypoint.");
        }
    }
    if !message.is_empty() {
        if is_critical {
            error!("{} Your gpu may not suport encoding with this.", message);
        } else {
            info!(
                "{}
                Your gpu may not suport encoding with this. \
            If you're not using this encoder, ignore this message.",
                message
            );
        }
    }
}

#[cfg(test)]
mod gpu_environment_tests {
    use super::{gpu_environment_changes, mesa_render_gpu_environment};

    #[test]
    fn intel_x11_selects_vulkan_without_forcing_steam_gl_vendors() {
        let selected = mesa_render_gpu_environment(0x8086, 0xa782, true, Some("/egl/mesa.json"));
        assert_eq!(
            selected,
            vec![("MESA_VK_DEVICE_SELECT".into(), "8086:a782!".into())]
        );
        // Restart confirmation must also remove overrides inherited from the
        // previously selected NVIDIA GPU or the old Intel launch environment.
        let mut current = selected.clone();
        current.extend(nvidia());
        current.push((
            "__EGL_VENDOR_LIBRARY_FILENAMES".into(),
            "/egl/mesa.json".into(),
        ));
        current.push(("DRI_PRIME".into(), "8086:a782".into()));
        let changes = gpu_environment_changes(&selected, &current);
        assert_eq!(changes.len(), current.len() - selected.len());
        assert!(changes.iter().all(|change| change.starts_with("Remove ")));
        assert!(gpu_environment_changes(&selected, &selected).is_empty());
    }

    #[test]
    fn wayland_intel_and_x11_amd_keep_full_render_selection() {
        for (vendor, x11, ids) in [(0x8086, false, "8086:a782"), (0x1002, true, "1002:a782")] {
            let selected = mesa_render_gpu_environment(vendor, 0xa782, x11, Some("/egl/mesa.json"));
            assert!(selected.contains(&("DRI_PRIME".into(), ids.into())));
            assert!(selected.contains(&("MESA_VK_DEVICE_SELECT".into(), format!("{ids}!"))));
            assert!(selected.contains(&("__GLX_VENDOR_LIBRARY_NAME".into(), "mesa".into())));
            assert!(selected.contains(&(
                "__EGL_VENDOR_LIBRARY_FILENAMES".into(),
                "/egl/mesa.json".into()
            )));
        }
    }

    fn pairs(values: &[(&str, &str)]) -> Vec<(String, String)> {
        values
            .iter()
            .map(|(name, value)| (name.to_string(), value.to_string()))
            .collect()
    }

    fn nvidia() -> Vec<(String, String)> {
        pairs(&[
            ("__NV_PRIME_RENDER_OFFLOAD", "1"),
            ("__GLX_VENDOR_LIBRARY_NAME", "nvidia"),
            ("__VK_LAYER_NV_optimus", "NVIDIA_only"),
        ])
    }

    fn intel() -> Vec<(String, String)> {
        pairs(&[
            ("DRI_PRIME", "8086:a782"),
            ("MESA_VK_DEVICE_SELECT", "8086:a782!"),
            ("__GLX_VENDOR_LIBRARY_NAME", "mesa"),
        ])
    }

    #[test]
    fn unconfigured_steam_needs_selection_for_either_primary_gpu() {
        for selected in [nvidia(), intel()] {
            assert!(!gpu_environment_changes(&selected, &[]).is_empty());
        }
    }

    #[test]
    fn applied_selection_does_not_request_another_restart() {
        for selected in [nvidia(), intel()] {
            assert!(gpu_environment_changes(&selected, &selected).is_empty());
        }
    }

    #[test]
    fn switching_render_gpu_requires_both_removal_and_selection() {
        for (selected, current) in [(nvidia(), intel()), (intel(), nvidia())] {
            let changes = gpu_environment_changes(&selected, &current);
            assert!(changes.iter().any(|change| change.starts_with("Remove ")));
            assert!(changes.iter().any(|change| change.contains("is not set")));
        }
    }

    #[test]
    fn mixed_icd_list_does_not_prove_nvidia_is_selected() {
        let mut selected = nvidia();
        selected.push(("VK_DRIVER_FILES".into(), "/icd/nvidia.json".into()));
        let mut current = selected.clone();
        current.last_mut().unwrap().1 = "/icd/nvidia.json:/icd/intel.json".into();
        assert!(!gpu_environment_changes(&selected, &current).is_empty());
    }

    #[test]
    fn mesa_selection_must_hide_other_devices() {
        let selected = intel();
        let mut current = selected.clone();
        current[1].1 = "8086:a782".into();
        assert!(!gpu_environment_changes(&selected, &current).is_empty());
    }

    #[test]
    fn stale_loader_restrictions_require_restart() {
        let selected = intel();
        let mut current = selected.clone();
        current.push(("VK_LOADER_LAYERS_DISABLE".into(), "~implicit~".into()));
        assert!(!gpu_environment_changes(&selected, &current).is_empty());
    }
}

#[cfg(test)]
mod mirror_tests {
    use super::{MIRROR_BEGIN, MIRROR_END, mirror_launcher_script};
    use std::{fs, process::Command, time::SystemTime};

    const ORIGINAL: &str =
        "#!/bin/bash\n# Vendor launcher\nprintf '%s' \"${MESA_VK_WSI_HEADLESS_SWAPCHAIN-unset}\"\n";

    #[test]
    fn installation_is_idempotent_and_removal_restores_original() {
        let installed = mirror_launcher_script(ORIGINAL, true).unwrap();
        assert!(installed.starts_with("#!/bin/bash\n"));
        assert!(installed.ends_with(ORIGINAL.split_once('\n').unwrap().1));
        assert_eq!(mirror_launcher_script(&installed, true).unwrap(), installed);
        assert_eq!(mirror_launcher_script(&installed, false).unwrap(), ORIGINAL);
        assert_eq!(mirror_launcher_script(ORIGINAL, false).unwrap(), ORIGINAL);
    }

    #[test]
    fn malformed_or_misplaced_sections_are_rejected() {
        assert!(mirror_launcher_script("ELF", true).is_err());
        assert!(mirror_launcher_script(&format!("#!/bin/sh\n{MIRROR_BEGIN}broken"), true).is_err());
        assert!(mirror_launcher_script(&format!("{ORIGINAL}{MIRROR_END}"), false).is_err());
    }

    #[test]
    fn compatibility_updates_preserve_the_vendor_body() {
        let previous =
            format!("#!/bin/bash\n{MIRROR_BEGIN}# Older managed code\n{MIRROR_END}vendor_body\n");
        let updated = mirror_launcher_script(&previous, true).unwrap();
        assert!(!updated.contains("Older managed code"));
        assert!(updated.ends_with("vendor_body\n"));
    }

    #[test]
    fn headless_environment_is_scoped_to_intel_wayland_and_nvidia_primary() {
        let unique = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let root =
            std::env::temp_dir().join(format!("alvr-mirror-{}-{unique}", std::process::id()));
        struct Cleanup(std::path::PathBuf);
        impl Drop for Cleanup {
            fn drop(&mut self) {
                fs::remove_dir_all(&self.0).ok();
            }
        }
        let _cleanup = Cleanup(root.clone());
        let card = root.join("card0/device");
        fs::create_dir_all(&card).unwrap();
        fs::write(card.join("vendor"), "0x10de\n").unwrap();
        let script = mirror_launcher_script(ORIGINAL, true)
            .unwrap()
            .replace("/sys/class/drm", root.to_str().unwrap());
        for (gpu, wayland, primary, expected) in [
            ("8086:a782!", true, "1", "1"),
            ("8086:a782!", true, "0", "unset"),
            ("8086:a782!", false, "1", "unset"),
            ("10de:2504!", true, "1", "unset"),
            ("", true, "1", "unset"),
        ] {
            fs::write(card.join("boot_vga"), primary).unwrap();
            let mut command = Command::new("bash");
            command
                .arg("-c")
                .arg(&script)
                .env("MESA_VK_DEVICE_SELECT", gpu)
                .env_remove("MESA_VK_WSI_HEADLESS_SWAPCHAIN")
                .env_remove("WAYLAND_DISPLAY");
            if wayland {
                command.env("WAYLAND_DISPLAY", "wayland-test");
            }
            let result = command.output().unwrap();
            assert!(result.status.success(), "{result:?}");
            assert_eq!(String::from_utf8(result.stdout).unwrap(), expected);
        }
    }
}
