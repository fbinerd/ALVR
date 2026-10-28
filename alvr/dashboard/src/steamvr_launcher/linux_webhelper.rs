use alvr_common::{
    anyhow::{Context, Result, bail},
    error, info,
};
use std::{
    fs,
    path::{Path, PathBuf},
    process::Command,
    sync::atomic::{AtomicBool, Ordering},
    thread,
    time::{Duration, Instant},
};
use sysinfo::{Pid, Signal, System};

const BEGIN: &str = "# ALVR Intel X11 WebHelper compatibility begin\n";
const END: &str = "# ALVR Intel X11 WebHelper compatibility end\n";
const BODY: &str = r#"# Keep Intel GL local to the 64-bit browser. The 32-bit Steam client must
# retain the desktop GL vendor: forcing Mesa there can crash NVIDIA's Xorg.
case "${MESA_VK_DEVICE_SELECT-}" in
    8086:*)
        alvr_webhelper_x11=false
        case "${XDG_SESSION_TYPE-}" in
            x11) alvr_webhelper_x11=true ;;
            wayland) ;;
            *) [ -n "${WAYLAND_DISPLAY-}" ] || [ -z "${DISPLAY-}" ] || alvr_webhelper_x11=true ;;
        esac
        if "$alvr_webhelper_x11"; then
            export DRI_PRIME="${MESA_VK_DEVICE_SELECT%!}"
            export __GLX_VENDOR_LIBRARY_NAME=mesa
            echo '[ALVR] Using Intel OpenGL for this 64-bit WebHelper' >&2
        fi
        unset alvr_webhelper_x11
        ;;
esac
"#;

static PREPARING: AtomicBool = AtomicBool::new(false);

struct Preparation;

impl Drop for Preparation {
    fn drop(&mut self) {
        PREPARING.store(false, Ordering::Release);
    }
}

fn launcher_script(original: &str, enabled: bool) -> Result<String> {
    if !original.starts_with("#!") {
        bail!("WebHelper launcher is not a shell script");
    }
    let newline = original
        .find('\n')
        .context("WebHelper launcher has no body")?;
    let (shebang, mut body) = original.split_at(newline + 1);
    if let Some(previous) = body.strip_prefix(BEGIN) {
        let end = previous
            .find(END)
            .context("Incomplete ALVR WebHelper section")?;
        body = &previous[end + END.len()..];
    }
    if body.contains(BEGIN) || body.contains(END) {
        bail!("Unexpected ALVR WebHelper marker; leaving launcher untouched");
    }
    Ok(if enabled {
        format!("{shebang}{BEGIN}{BODY}{END}{body}")
    } else {
        format!("{shebang}{body}")
    })
}

fn update_launcher(path: &Path, enabled: bool) -> Result<()> {
    let metadata = fs::symlink_metadata(path)?;
    if !metadata.file_type().is_file() {
        if !enabled {
            return Ok(());
        }
        bail!(
            "WebHelper launcher is not a regular file: {}",
            path.display()
        );
    }
    let original = fs::read_to_string(path)?;
    if !enabled && !original.contains(BEGIN) && !original.contains(END) {
        return Ok(());
    }
    let updated = launcher_script(&original, enabled)?;
    if original == updated {
        return Ok(());
    }
    let temporary = path.with_extension(format!("alvr-{}.tmp", std::process::id()));
    let result = (|| -> Result<()> {
        fs::write(&temporary, updated)?;
        fs::set_permissions(&temporary, metadata.permissions())?;
        if fs::read_to_string(path)? != original
            || fs::symlink_metadata(path)?.file_type().is_symlink()
        {
            bail!("WebHelper launcher changed during preparation");
        }
        fs::rename(&temporary, path)?;
        Ok(())
    })();
    if result.is_err() {
        fs::remove_file(temporary).ok();
    }
    result
}

fn launchers() -> Result<[PathBuf; 2]> {
    let steam =
        super::linux_steamvr::steam_script_path().context("Steam's steam.sh was not found")?;
    Ok([
        steam
            .parent()
            .unwrap()
            .join("ubuntu12_64/steamwebhelper.sh"),
        alvr_server_io::steamvr_root_dir()?.join("bin/vrwebhelper/linux64/vrwebhelper.sh"),
    ])
}

pub(super) fn configure(enabled: bool) -> Result<()> {
    let paths = match launchers() {
        Ok(paths) => paths,
        Err(_) if !enabled => return Ok(()),
        Err(error) => return Err(error),
    };
    for path in paths {
        if enabled || path.exists() {
            update_launcher(&path, enabled)?;
        }
    }
    Ok(())
}

fn selector() -> Option<String> {
    crate::data_sources::get_read_only_local_session()
        .settings()
        .video
        .encoder_config
        .linux_gpu
        .render_gpu
        .clone()
}

fn process_env(pid: u32, name: &str) -> Option<String> {
    fs::read(format!("/proc/{pid}/environ"))
        .ok()?
        .split(|byte| *byte == 0)
        .filter_map(|pair| std::str::from_utf8(pair).ok()?.split_once('='))
        .find_map(|(key, value)| (key == name).then(|| value.to_owned()))
}

fn steam_pids() -> Vec<u32> {
    let Ok(entries) = fs::read_dir("/proc") else {
        return vec![];
    };
    use std::os::unix::fs::MetadataExt;
    let uid = fs::metadata("/proc/self")
        .map(|metadata| metadata.uid())
        .ok();
    entries
        .flatten()
        .filter_map(|entry| {
            if Some(entry.metadata().ok()?.uid()) != uid {
                return None;
            }
            (fs::read_to_string(entry.path().join("comm")).ok()?.trim() == "steam")
                .then(|| entry.file_name().to_str()?.parse().ok())
                .flatten()
        })
        .collect()
}

// Only the browser main process belonging to our running Steam client. Never
// terminate a CEF renderer, a zygote, or another user's Steam session.
fn browser_pid(steam_pids: &[u32]) -> Option<u32> {
    for entry in fs::read_dir("/proc").ok()?.flatten() {
        let path = entry.path();
        if fs::read_to_string(path.join("comm"))
            .ok()
            .as_deref()
            .map(str::trim)
            != Some("steamwebhelper")
        {
            continue;
        }
        let Ok(args) = fs::read(path.join("cmdline")) else {
            continue;
        };
        let args = args.split(|byte| *byte == 0).collect::<Vec<_>>();
        if args.iter().any(|arg| arg.starts_with(b"--type=")) {
            continue;
        }
        if steam_pids
            .iter()
            .any(|pid| args.contains(&format!("-steampid={pid}").as_bytes()))
        {
            return entry.file_name().to_str()?.parse().ok();
        }
    }
    None
}

fn prepare(mut steam: Command, mut steamvr: Command, selected: Option<String>) -> Result<()> {
    configure(true)?;
    if steam_pids().is_empty() {
        info!("Starting Steam before preparing Intel X11 WebHelpers");
        super::spawn_and_reap(&mut steam)?;
    }
    let start = Instant::now();
    let mut restarted = false;
    while start.elapsed() < Duration::from_secs(120) {
        if selector() != selected {
            bail!("Render GPU changed while preparing WebHelpers; launch cancelled");
        }
        let pids = steam_pids();
        if let Some(browser) = browser_pid(&pids) {
            // Steam's updater can restore the first script while starting.
            // Prepare again after the browser exists, then restart it once.
            configure(true)?;
            let expected = steamvr
                .get_envs()
                .find_map(|(key, value)| {
                    (key == "MESA_VK_DEVICE_SELECT")
                        .then(|| value?.to_str().map(str::to_owned))
                        .flatten()
                })
                .context("Intel Vulkan selector is missing")?;
            let ids = expected.trim_end_matches('!');
            if process_env(browser, "MESA_VK_DEVICE_SELECT").as_deref() == Some(&expected)
                && process_env(browser, "DRI_PRIME").as_deref() == Some(ids)
                && process_env(browser, "__GLX_VENDOR_LIBRARY_NAME").as_deref() == Some("mesa")
            {
                info!("Intel X11 WebHelper ready (PID {browser}); starting SteamVR");
                super::spawn_and_reap(&mut steamvr)?;
                return Ok(());
            }
            if !restarted {
                let system = System::new_all();
                let process = system
                    .process(Pid::from_u32(browser))
                    .context("Steam browser exited")?;
                if process.kill_with(Signal::Term) != Some(true) {
                    bail!("Could not restart Steam's browser on the selected Intel GPU");
                }
                info!("Restarting only Steam's 64-bit browser to apply Intel GL (PID {browser})");
                restarted = true;
            }
        }
        thread::sleep(Duration::from_millis(250));
    }
    bail!("Intel X11 WebHelper did not become ready in 120 seconds; SteamVR was not launched")
}

pub(super) fn launch(steam: Command, steamvr: Command) {
    if PREPARING
        .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
        .is_err()
    {
        info!("Intel X11 WebHelper preparation is already running");
        return;
    }
    let selected = selector();
    thread::spawn(move || {
        let _preparation = Preparation;
        if let Err(error) = prepare(steam, steamvr, selected) {
            error!("Intel X11 SteamVR startup failed: {error:#}");
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    const ORIGINAL: &str =
        "#!/bin/sh\nprintf '%s|%s' \"${DRI_PRIME-unset}\" \"${__GLX_VENDOR_LIBRARY_NAME-unset}\"\n";

    #[test]
    fn installation_and_removal_preserve_vendor_script() {
        let script = launcher_script(ORIGINAL, true).unwrap();
        assert_eq!(launcher_script(&script, true).unwrap(), script);
        assert_eq!(launcher_script(&script, false).unwrap(), ORIGINAL);
        assert_eq!(launcher_script(ORIGINAL, false).unwrap(), ORIGINAL);
        assert!(launcher_script("ELF", true).is_err());
        assert!(launcher_script(&format!("#!/bin/sh\n{BEGIN}broken"), true).is_err());
        assert!(launcher_script(&format!("{ORIGINAL}{END}"), false).is_err());
    }

    #[test]
    fn atomic_update_preserves_permissions_and_rejects_symlinks() {
        use std::{
            os::unix::fs::{PermissionsExt, symlink},
            time::SystemTime,
        };
        let directory = std::env::temp_dir().join(format!(
            "alvr-webhelper-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(SystemTime::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir(&directory).unwrap();
        let path = directory.join("launcher.sh");
        fs::write(&path, ORIGINAL).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o755)).unwrap();
        update_launcher(&path, true).unwrap();
        assert_eq!(
            fs::metadata(&path).unwrap().permissions().mode() & 0o777,
            0o755
        );
        update_launcher(&path, false).unwrap();
        assert_eq!(fs::read_to_string(&path).unwrap(), ORIGINAL);
        let link = directory.join("custom.sh");
        symlink(&path, &link).unwrap();
        assert!(update_launcher(&link, true).is_err());
        assert_eq!(fs::read_to_string(&path).unwrap(), ORIGINAL);
        fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn intel_gl_is_local_to_x11_webhelpers() {
        let script = launcher_script(ORIGINAL, true).unwrap();
        for (session, wayland, gpu, expected) in [
            ("x11", "", "8086:a782!", "8086:a782|mesa"),
            ("wayland", "wayland-0", "8086:a782!", "unset|nvidia"),
            ("wayland", "", "8086:a782!", "unset|nvidia"),
            ("", "", "8086:a782!", "8086:a782|mesa"),
            ("x11", "", "10de:2504!", "unset|nvidia"),
            ("x11", "", "", "unset|nvidia"),
        ] {
            let output = Command::new("sh")
                .args(["-c", &script])
                .env_clear()
                .env("XDG_SESSION_TYPE", session)
                .env("DISPLAY", ":1")
                .env("WAYLAND_DISPLAY", wayland)
                .env("MESA_VK_DEVICE_SELECT", gpu)
                .env("__GLX_VENDOR_LIBRARY_NAME", "nvidia")
                .output()
                .unwrap();
            assert!(output.status.success());
            assert_eq!(String::from_utf8(output.stdout).unwrap(), expected);
        }
    }
}
