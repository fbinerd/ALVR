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

        let Some(steam_script) = steam_script_path() else {
            error!(
                "Could not locate Steam's steam.sh script to restart it with ALVR's GPU environment"
            );
            return;
        };
        let mut command = Command::new(steam_script);
        command.arg("steam://rungameid/250820");
        apply_render_gpu_environment(&mut command, &adapter);
        if let Err(e) = super::spawn_and_reap(&mut command) {
            error!("Failed to restart SteamVR with Render GPU {gpu_name}: {e}");
        } else {
            info!("Started Steam and SteamVR with ALVR Render GPU {gpu_name}");
        }
    });
}

pub fn render_gpu_conflict_for_confirmation() -> Option<(String, String)> {
    let adapter = selected_render_gpu_adapter()?;
    let conflicts = conflicting_steam_overrides(&adapter);
    if conflicts.is_empty() || running_steam_pids().is_empty() {
        return None;
    }
    Some((adapter.get_info().name, conflicts.join(" ")))
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
    let info = adapter.get_info();
    if is_nvidia_proprietary(&info) {
        command.env("__GLX_VENDOR_LIBRARY_NAME", "nvidia");
        command.env("__NV_PRIME_RENDER_OFFLOAD", "1");
        let base = "/usr/share/vulkan/icd.d/nvidia_icd";
        let path = [format!("{base}.json"), format!("{base}.x86_64.json")]
            .into_iter()
            .find(|path| Path::new(path).is_file());
        if let Some(path) = path {
            command.env("VK_DRIVER_FILES", path);
        } else {
            command.env("__VK_LAYER_NV_optimus", "NVIDIA_only");
        }
    } else {
        let ids = format!("{:04x}:{:04x}", info.vendor, info.device);
        command.env("DRI_PRIME", &ids);
        command.env("MESA_VK_DEVICE_SELECT", format!("{ids}!"));
        command.env("__GLX_VENDOR_LIBRARY_NAME", "mesa");
        let mesa_egl = "/usr/share/glvnd/egl_vendor.d/50_mesa.json";
        if Path::new(mesa_egl).is_file() {
            command.env("__EGL_VENDOR_LIBRARY_FILENAMES", mesa_egl);
        }
        // MESA_VK_DEVICE_SELECT (with "!") already hides the other GPUs.
        // Limiting VK_DRIVER_FILES to one host ICD as well breaks Proton
        // games (VRChat: no graphics device on Intel), so leave it alone.
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
    "__VK_LAYER_NV_optimus",
    "MESA_VK_DEVICE_SELECT",
    "DRI_PRIME",
    "__NV_PRIME_RENDER_OFFLOAD",
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

// Steam variables that force a GPU other than `adapter` on SteamVR.
fn conflicting_steam_overrides(adapter: &wgpu::Adapter) -> Vec<String> {
    let nvidia = is_nvidia_proprietary(&adapter.get_info());
    steam_gpu_override_env()
        .into_iter()
        .filter(|(name, value)| {
            let value_lower = value.to_lowercase();
            match name.as_str() {
                "VK_DRIVER_FILES" | "VK_ICD_FILENAMES" => value_lower.contains("nvidia") != nvidia,
                "__VK_LAYER_NV_optimus" => value == "NVIDIA_only" && !nvidia,
                "__NV_PRIME_RENDER_OFFLOAD" => value == "1" && !nvidia,
                "__GLX_VENDOR_LIBRARY_NAME" => (value_lower == "nvidia") != nvidia,
                "MESA_VK_DEVICE_SELECT" | "DRI_PRIME" => nvidia,
                _ => false,
            }
        })
        .map(|(name, value)| format!("{name}={value}"))
        .collect()
}

// Launch options that make SteamVR / a game render on `adapter`.
fn render_gpu_launch_options(adapter: &wgpu::Adapter) -> String {
    let info = adapter.get_info();
    if is_nvidia_proprietary(&info) {
        // NVIDIA's proprietary driver: PRIME render offload.
        let base_path = "/usr/share/vulkan/icd.d/nvidia_icd";
        let vk_override = if Path::new(&format!("{base_path}.json")).exists() {
            format!("VK_DRIVER_FILES={base_path}.json")
        } else if Path::new(&format!("{base_path}.x86_64.json")).exists() {
            format!("VK_DRIVER_FILES={base_path}.x86_64.json")
        } else {
            "__VK_LAYER_NV_optimus=NVIDIA_only".to_string()
        };
        format!("__GLX_VENDOR_LIBRARY_NAME=nvidia __NV_PRIME_RENDER_OFFLOAD=1 {vk_override}")
    } else {
        // Mesa drivers (Intel, AMD, NVK): select the device by PCI ids for
        // OpenGL (DRI_PRIME) and Vulkan (MESA_VK_DEVICE_SELECT). With NVK the
        // NVIDIA user-space libraries may still be installed: pin GLX to Mesa.
        let ids = format!("{:04x}:{:04x}", info.vendor, info.device);
        let glx = if info.vendor == 0x10de {
            "__GLX_VENDOR_LIBRARY_NAME=mesa "
        } else {
            ""
        };
        format!("{glx}DRI_PRIME={ids} MESA_VK_DEVICE_SELECT={ids}")
    }
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
        if !conflicts.is_empty() {
            warn!(
                "Render GPU is {}, but Steam was started with {}: SteamVR and games inherit \
                this and will not render on the chosen GPU. Start Steam without these variables \
                (for example not through a PRIME-offload wrapper), or choose the GPU they select.",
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
