// Optional video output for the mock client: shows the received stream in an
// ffplay window and records it, so a stream can be checked on a monitor
// without a headset. Configured by environment variables:
//   MOCK_VIEW=0       do not open the ffplay window (default: open it)
//   MOCK_OUT=<dir>    write stream.<codec>, frames.csv and poses.csv there
use alvr_session::CodecType;
use std::{
    fs::{self, File},
    io::{BufWriter, Write},
    path::PathBuf,
    process::{Child, Command, Stdio},
    sync::mpsc::{self, SyncSender, TrySendError},
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

// About three seconds of 72 Hz video; beyond that the viewer is behind and
// packets are dropped instead of blocking the network thread.
const QUEUE_PACKETS: usize = 216;
const VIEWER_QUEUE_PACKETS: usize = 720;

pub fn output_dir() -> Option<PathBuf> {
    std::env::var_os("MOCK_OUT").map(PathBuf::from)
}

struct Packet {
    received: Duration,
    timestamp: Duration,
    data: Vec<u8>,
}

pub struct VideoSink {
    sender: Option<SyncSender<Packet>>,
    worker: Option<JoinHandle<()>>,
    origin: Instant,
}

impl VideoSink {
    pub fn start(codec: CodecType, config_nal: &[u8], fps: f32, origin: Instant) -> Self {
        let format = match codec {
            CodecType::H264 => "h264",
            CodecType::Hevc => "hevc",
            CodecType::AV1 => "obu",
        };
        let viewer = if std::env::var("MOCK_VIEW").as_deref() == Ok("0") {
            None
        } else {
            spawn_viewer(format, fps)
        };
        let mut recorder = output_dir().and_then(|dir| {
            fs::create_dir_all(&dir).ok()?;
            let stream = File::create(dir.join(format!("stream.{format}"))).ok()?;
            let mut frames = BufWriter::new(File::create(dir.join("frames.csv")).ok()?);
            writeln!(frames, "received_ns,timestamp_ns,bytes").ok()?;
            Some((BufWriter::new(stream), frames))
        });
        if let Some((stream, _)) = &mut recorder {
            stream.write_all(config_nal).ok();
        }

        // The window gets its own queue so a slow viewer never costs the
        // recording (or the server) a frame. If the viewer falls behind by
        // a whole queue, its packets are dropped and the picture may break
        // until the next IDR; the recording stays complete.
        let viewer_sender = viewer.map(|mut child| {
            let (sender, receiver) = mpsc::sync_channel::<Vec<u8>>(VIEWER_QUEUE_PACKETS);
            let config = config_nal.to_vec();
            thread::spawn(move || {
                let mut alive = child
                    .stdin
                    .as_mut()
                    .is_some_and(|stdin| stdin.write_all(&config).is_ok());
                while let Ok(data) = receiver.recv() {
                    alive = alive
                        && child
                            .stdin
                            .as_mut()
                            .is_some_and(|stdin| stdin.write_all(&data).is_ok());
                    // After the window is closed, keep draining the queue.
                }
                drop(child.stdin.take());
                if !alive {
                    child.kill().ok();
                }
                child.wait().ok();
            });
            sender
        });

        let (sender, receiver) = mpsc::sync_channel::<Packet>(QUEUE_PACKETS);
        let worker = thread::spawn(move || {
            let mut written = 0u64;
            let mut viewer_dropped = 0u64;
            while let Ok(packet) = receiver.recv() {
                if let Some(viewer) = &viewer_sender
                    && let Err(TrySendError::Full(_)) = viewer.try_send(packet.data.clone())
                {
                    viewer_dropped += 1;
                    if viewer_dropped == 1 || viewer_dropped % 720 == 0 {
                        eprintln!("mock viewer behind: {viewer_dropped} packets not shown");
                    }
                }
                if let Some((stream, frames)) = &mut recorder {
                    stream.write_all(&packet.data).ok();
                    writeln!(
                        frames,
                        "{},{},{}",
                        packet.received.as_nanos(),
                        packet.timestamp.as_nanos(),
                        packet.data.len()
                    )
                    .ok();
                    // Bounded loss if the process is killed instead of closed.
                    written += 1;
                    if written % 64 == 0 {
                        stream.flush().ok();
                        frames.flush().ok();
                    }
                }
            }
            if let Some((mut stream, mut frames)) = recorder {
                stream.flush().ok();
                frames.flush().ok();
            }
            drop(viewer_sender);
        });

        Self {
            sender: Some(sender),
            worker: Some(worker),
            origin,
        }
    }

    // Never blocks: a full queue drops the packet and reports it as not
    // submitted, which makes the client core request an IDR frame.
    pub fn push(&self, timestamp: Duration, data: &[u8]) -> bool {
        let Some(sender) = &self.sender else {
            return false;
        };
        let packet = Packet {
            received: self.origin.elapsed(),
            timestamp,
            data: data.to_vec(),
        };
        match sender.try_send(packet) {
            Ok(()) => true,
            Err(TrySendError::Full(_)) | Err(TrySendError::Disconnected(_)) => false,
        }
    }
}

impl Drop for VideoSink {
    fn drop(&mut self) {
        drop(self.sender.take());
        if let Some(worker) = self.worker.take() {
            worker.join().ok();
        }
    }
}

fn spawn_viewer(format: &str, fps: f32) -> Option<Child> {
    Command::new("ffplay")
        .args([
            "-loglevel",
            "warning",
            "-window_title",
            "ALVR mock client video",
            "-x",
            "1600",
            "-fflags",
            "nobuffer",
            "-framedrop",
            "-probesize",
            "32",
            "-f",
            format,
            "-framerate",
            &format!("{fps}"),
            "-i",
            "-",
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .spawn()
        .map_err(|e| eprintln!("ffplay could not start: {e}"))
        .ok()
}

// Scripted head motion: yaw = amplitude * sin(2 pi t / period).
// MOCK_SWEEP="<amplitude degrees>,<period seconds>", e.g. "30,4".
#[derive(Clone, Copy, PartialEq)]
pub struct Sweep {
    pub amplitude_rad: f32,
    pub period_s: f32,
}

impl Sweep {
    pub fn from_env() -> Option<Self> {
        let value = std::env::var("MOCK_SWEEP").ok()?;
        let (amplitude, period) = value.split_once(',')?;
        let amplitude_deg: f32 = amplitude.trim().parse().ok()?;
        let period_s: f32 = period.trim().parse().ok()?;
        (period_s > 0.0).then_some(Self {
            amplitude_rad: amplitude_deg.to_radians(),
            period_s,
        })
    }

    // Returns (yaw, yaw angular velocity) at time t.
    pub fn at(&self, t: Duration) -> (f32, f32) {
        let w = std::f32::consts::TAU / self.period_s;
        let phase = w * t.as_secs_f32();
        (
            self.amplitude_rad * phase.sin(),
            self.amplitude_rad * w * phase.cos(),
        )
    }
}

// Scripted sideways head motion: x = amplitude * sin(2 pi t / period).
// MOCK_SWAY="<amplitude metres>,<period seconds>", e.g. "0.3,4". Unlike yaw,
// the headset's rotational reprojection cannot hide judder from this.
#[derive(Clone, Copy, PartialEq)]
pub struct Sway {
    pub amplitude_m: f32,
    pub period_s: f32,
}

impl Sway {
    pub fn from_env() -> Option<Self> {
        let value = std::env::var("MOCK_SWAY").ok()?;
        let (amplitude, period) = value.split_once(',')?;
        let amplitude_m: f32 = amplitude.trim().parse().ok()?;
        let period_s: f32 = period.trim().parse().ok()?;
        (period_s > 0.0).then_some(Self {
            amplitude_m,
            period_s,
        })
    }

    // Returns (x offset, x velocity) at time t.
    pub fn at(&self, t: Duration) -> (f32, f32) {
        let w = std::f32::consts::TAU / self.period_s;
        let phase = w * t.as_secs_f32();
        (
            self.amplitude_m * phase.sin(),
            self.amplitude_m * w * phase.cos(),
        )
    }
}

pub struct PoseLog(Option<BufWriter<File>>);

impl PoseLog {
    pub fn open() -> Self {
        let file = output_dir().and_then(|dir| {
            fs::create_dir_all(&dir).ok()?;
            let mut file = BufWriter::new(File::create(dir.join("poses.csv")).ok()?);
            writeln!(file, "poll_ns,yaw_rad,pitch_rad,yaw_velocity_rad_s,x_m").ok()?;
            Some(file)
        });
        Self(file)
    }

    pub fn write(&mut self, poll: Duration, yaw: f32, pitch: f32, yaw_velocity: f32, x: f32) {
        if let Some(file) = &mut self.0 {
            writeln!(file, "{},{yaw},{pitch},{yaw_velocity},{x}", poll.as_nanos()).ok();
        }
    }
}

impl Drop for PoseLog {
    fn drop(&mut self) {
        if let Some(file) = &mut self.0 {
            file.flush().ok();
        }
    }
}
