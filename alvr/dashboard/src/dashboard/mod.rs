mod components;

use self::components::{
    DevicesTab, LogsTab, NotificationBar, SettingsTab, SetupWizard, SetupWizardRequest,
};
use crate::{
    DataSources,
    dashboard::components::{CloseAction, NewVersionPopup, StatisticsTab},
};
use alvr_common::{
    LogEntry,
    parking_lot::{Condvar, Mutex},
};
use alvr_events::EventType;
use alvr_gui_common::theme;
use alvr_packets::{ClientConnectionsAction, PathValuePair};
use alvr_session::SessionConfig;
use eframe::egui::{
    self, Align, CentralPanel, Direction, Frame, Layout, Margin, Panel, RichText, Ui,
};
use serde::{Deserialize, Serialize};
use std::{collections::BTreeMap, path::PathBuf, sync::Arc};

#[derive(Serialize, Deserialize, Debug)]
pub enum ServerRequest {
    Log(LogEntry),
    GetSession,
    UpdateSession(Box<SessionConfig>),
    SetSessionValues(Vec<PathValuePair>),
    UpdateClientList {
        hostname: String,
        action: ClientConnectionsAction,
    },
    InsertIdr,
    StartRecording,
    StopRecording,
    AddFirewallRules,
    RemoveFirewallRules,
    GetDriverList,
    RegisterAlvrDriver,
    UnregisterDriver(PathBuf),
    GetAirLinkDevices,
    SetAirLinkDevices(bool),
    AnswerAirLinkPairing(bool),
    RestartSteamvr,
    ShutdownSteamvr,
}

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
enum Tab {
    Devices,
    Statistics,
    Settings,
    #[cfg(not(target_arch = "wasm32"))]
    Installation,
    Logs,
    Debug,
    About,
}

pub struct Dashboard {
    data_sources: DataSources,
    just_opened: bool,
    server_restarting: Arc<Mutex<bool>>,
    server_restarting_condvar: Arc<Condvar>,
    selected_tab: Tab,
    tab_labels: BTreeMap<Tab, &'static str>,
    connections_tab: DevicesTab,
    statistics_tab: StatisticsTab,
    settings_tab: SettingsTab,
    #[cfg(not(target_arch = "wasm32"))]
    installation_tab: components::InstallationTab,
    logs_tab: LogsTab,
    notification_bar: NotificationBar,
    setup_wizard: SetupWizard,
    new_version_popup: Option<components::NewVersionPopup>,
    setup_wizard_open: bool,
    session: Option<SessionConfig>,
    airlink_streaming: bool,
    streaming_audio_active: bool,
    streaming_audio_since: std::time::Instant,
    streaming_audio_applied: bool,
    streaming_audio_tried_at: Option<std::time::Instant>,
    // Address of the headset asking to pair and when the server last said so.
    airlink_pairing: Option<(String, Option<u32>, std::time::Instant)>,
    airlink_pairing_answered_at: Option<std::time::Instant>,
}

impl Dashboard {
    /// Makes the output and input chosen for streaming the system's
    /// defaults. Starts from the defaults of before streaming, so that a
    /// choice changed meanwhile does not leave an earlier one behind. False
    /// while a device is not there yet.
    #[cfg(target_os = "linux")]
    fn select_streaming_audio_devices(&self) -> bool {
        let Some(session) = &self.session else {
            return false;
        };
        let audio = session.to_settings().audio;
        let speakers = audio.game_audio.as_option().map(|config| &config.streaming_device);
        let microphone = audio.microphone.as_option().map(|config| &config.streaming_device);
        alvr_audio::linux::streaming_select_default_devices(speakers, microphone).is_ok()
    }

    /// Follows whether a headset streams, by Air Link or with ALVR's
    /// client, and keeps the system's default audio devices on the user's
    /// choice meanwhile. Called once per frame: ALVR's devices appear a
    /// moment after streaming starts.
    #[cfg(target_os = "linux")]
    fn update_streaming_audio(&mut self) {
        let streaming = self.airlink_streaming
            || self.session.as_ref().is_some_and(|session| {
                session.client_connections.values().any(|client| {
                    client.connection_state == alvr_common::ConnectionState::Streaming
                })
            });

        let now = std::time::Instant::now();
        let due = |every_s: u64| {
            self.streaming_audio_tried_at
                .is_none_or(|at| now.duration_since(at) >= std::time::Duration::from_secs(every_s))
        };

        if streaming != self.streaming_audio_active {
            self.streaming_audio_applied = false;
            self.streaming_audio_active = streaming;
            self.streaming_audio_since = now;
            self.streaming_audio_tried_at = None;
            if !streaming {
                alvr_audio::linux::streaming_restore_default_devices();
            }
            return;
        }

        if !streaming {
            // What "System default" means, and what comes back afterwards.
            // Not right after a stream: its devices take a moment to go.
            if now.duration_since(self.streaming_audio_since).as_secs() >= 3 && due(3) {
                self.streaming_audio_tried_at = Some(now);
                alvr_audio::linux::remember_idle_default_devices();
            }
        } else if !self.streaming_audio_applied && due(1) {
            self.streaming_audio_tried_at = Some(now);
            // The session manager makes ALVR's devices the default when they
            // appear, whatever the user chose, and they appear a moment after
            // streaming starts: keep at it for the first seconds.
            let done = self.select_streaming_audio_devices();
            self.streaming_audio_applied =
                done && now.duration_since(self.streaming_audio_since).as_secs() >= 8;
        }
    }

    pub fn new(creation_context: &eframe::CreationContext<'_>, data_sources: DataSources) -> Self {
        alvr_gui_common::theme::set_theme(&creation_context.egui_ctx);

        data_sources.request(ServerRequest::GetSession);

        Self {
            data_sources,
            just_opened: true,
            server_restarting: Arc::new(Mutex::new(false)),
            server_restarting_condvar: Arc::new(Condvar::new()),
            selected_tab: Tab::Devices,
            tab_labels: [
                (Tab::Devices, "🔌  Devices"),
                (Tab::Statistics, "📈  Statistics"),
                (Tab::Settings, "🔧  Settings"),
                #[cfg(not(target_arch = "wasm32"))]
                (Tab::Installation, "💾  Installation"),
                (Tab::Logs, "📝  Logs"),
                (Tab::Debug, "🐞  Debug"),
                (Tab::About, "ℹ  About"),
            ]
            .into_iter()
            .collect(),
            connections_tab: DevicesTab::new(),
            statistics_tab: StatisticsTab::new(),
            settings_tab: SettingsTab::new(),
            #[cfg(not(target_arch = "wasm32"))]
            installation_tab: components::InstallationTab::new(),
            logs_tab: LogsTab::new(),
            notification_bar: NotificationBar::new(),
            setup_wizard: SetupWizard::new(),
            setup_wizard_open: false,
            session: None,
            airlink_streaming: false,
            streaming_audio_active: false,
            streaming_audio_since: std::time::Instant::now(),
            streaming_audio_applied: false,
            streaming_audio_tried_at: None,
            airlink_pairing: None,
            airlink_pairing_answered_at: None,
            new_version_popup: None,
        }
    }

    // This call may block
    fn restart_steamvr(&self, requests: &mut Vec<ServerRequest>) {
        requests.push(ServerRequest::RestartSteamvr);

        let mut server_restarting_lock = self.server_restarting.lock();

        if *server_restarting_lock {
            self.server_restarting_condvar
                .wait(&mut server_restarting_lock);
        }

        *server_restarting_lock = true;

        #[cfg(not(target_arch = "wasm32"))]
        std::thread::spawn({
            let server_restarting = Arc::clone(&self.server_restarting);
            let condvar = Arc::clone(&self.server_restarting_condvar);
            move || {
                crate::steamvr_launcher::LAUNCHER.lock().restart_steamvr();

                *server_restarting.lock() = false;
                condvar.notify_one();
            }
        });
    }
}

impl eframe::App for Dashboard {
    fn ui(&mut self, ui: &mut Ui, _: &mut eframe::Frame) {
        let mut requests = vec![];

        let connected_to_server = self.data_sources.server_connected();

        while let Some(event) = self.data_sources.poll_event() {
            self.logs_tab.push_event(event.inner.clone());

            match event.inner.event_type {
                EventType::Log(log_event) => {
                    self.notification_bar
                        .push_notification(log_event, event.from_dashboard);
                }
                EventType::GraphStatistics(graph_statistics) => self
                    .statistics_tab
                    .update_graph_statistics(graph_statistics),
                EventType::StatisticsSummary(statistics) => {
                    self.statistics_tab.update_statistics(statistics)
                }
                EventType::Session(session) => {
                    let settings = session.to_settings();

                    self.connections_tab.update_client_list(&session);
                    self.settings_tab.update_session(&session.session_settings);
                    self.logs_tab.update_settings(&settings);
                    self.notification_bar.update_settings(&settings);
                    if self.just_opened {
                        if settings.extra.open_setup_wizard {
                            self.setup_wizard_open = true;
                        }

                        self.just_opened = false;
                    }

                    self.session = Some(*session);
                    // A choice changed while the headset streams applies now.
                    self.streaming_audio_applied = false;
                }
                EventType::ServerRequestsSelfRestart => self.restart_steamvr(&mut requests),
                #[cfg(not(target_arch = "wasm32"))]
                EventType::DriversList(list) => self.installation_tab.update_drivers(list),
                EventType::AirLinkDevices(enabled) => {
                    self.connections_tab.update_airlink_devices(enabled)
                }
                // The dashboard switches the system's default devices: the
                // server runs in SteamVR's container, which has no pactl.
                EventType::AirLinkHeadset { address, streaming } => self
                    .connections_tab
                    .update_airlink_headset(address, streaming),
                EventType::AirLinkPairing { address, headset, pairing_code } => {
                    // The server may repeat the request once more before it
                    // has taken the answer.
                    let just_answered = self
                        .airlink_pairing_answered_at
                        .is_some_and(|at| at.elapsed() < std::time::Duration::from_secs(4));
                    self.airlink_pairing = address.filter(|_| !just_answered).map(|address| {
                        let headset = headset.unwrap_or_else(|| "A headset".into());
                        (format!("{headset} at {address}"), pairing_code, std::time::Instant::now())
                    });
                }
                EventType::AirLinkStreaming(streaming) => self.airlink_streaming = streaming,
                EventType::Adb(adb_event) => self
                    .connections_tab
                    .update_adb_download_progress(adb_event.download_progress),
                EventType::NewVersionFound { version, message } => {
                    self.new_version_popup = Some(NewVersionPopup::new(version, message));
                }
                EventType::DebugGroup { .. }
                | EventType::Tracking(_)
                | EventType::Buttons(_)
                | EventType::Haptics(_) => (),
            }
        }

        if *self.server_restarting.lock() {
            CentralPanel::default().show(ui, |ui| {
                // todo: find a way to center both vertically and horizontally
                ui.vertical_centered(|ui| {
                    ui.add_space(100.0);
                    ui.heading(RichText::new("SteamVR is restarting").size(30.0));
                });
            });

            return;
        }

        self.notification_bar.ui(ui);

        if self.setup_wizard_open {
            CentralPanel::default().show(ui, |ui| {
                if let Some(request) = self.setup_wizard.ui(ui) {
                    match request {
                        SetupWizardRequest::ServerRequest(request) => {
                            requests.push(request);
                        }
                        SetupWizardRequest::Close { finished } => {
                            if finished {
                                requests.push(ServerRequest::SetSessionValues(vec![
                                    PathValuePair {
                                        path: alvr_packets::parse_path(
                                            "session_settings.extra.open_setup_wizard",
                                        )
                                        .unwrap(),
                                        value: serde_json::Value::Bool(false),
                                    },
                                ]))
                            }

                            self.setup_wizard_open = false;
                        }
                    }
                }
            });
        } else {
            Panel::left("side_panel")
                .resizable(false)
                .frame(
                    Frame::new()
                        .fill(theme::LIGHTER_BG)
                        .inner_margin(Margin::same(7)),
                )
                .exact_size(160.0)
                .show(ui, |ui| {
                    ui.with_layout(Layout::top_down_justified(Align::Center), |ui| {
                        ui.add_space(13.0);
                        ui.heading(RichText::new("ALVR").size(25.0).strong());
                        egui::warn_if_debug_build(ui);
                    });

                    ui.with_layout(Layout::top_down_justified(Align::Min), |ui| {
                        for (tab, label) in &self.tab_labels {
                            ui.selectable_value(&mut self.selected_tab, *tab, *label);
                        }
                    });

                    #[cfg(not(target_arch = "wasm32"))]
                    ui.with_layout(
                        Layout::bottom_up(Align::Center).with_cross_justify(true),
                        |ui| {
                            ui.add_space(5.0);

                            if connected_to_server {
                                if ui.button("Restart SteamVR").clicked() {
                                    self.restart_steamvr(&mut requests);
                                }
                            } else if ui.button("Launch SteamVR").clicked() {
                                crate::steamvr_launcher::LAUNCHER.lock().launch_steamvr();
                            }

                            ui.horizontal(|ui| {
                                ui.add_space(4.0);
                                ui.label(RichText::new("SteamVR:").size(13.0));
                                ui.add_space(-10.0);
                                ui.with_layout(
                                    Layout::centered_and_justified(Direction::LeftToRight),
                                    |ui| {
                                        ui.label(
                                            if connected_to_server {
                                                RichText::new("Connected").color(theme::OK_GREEN)
                                            } else {
                                                RichText::new("Disconnected").color(theme::KO_RED)
                                            }
                                            .size(13.0),
                                        )
                                    },
                                );
                            })
                        },
                    )
                });

            CentralPanel::default()
                .frame(Frame::new().inner_margin(Margin::same(20)).fill(theme::BG))
                .show(ui, |ui| {
                    ui.with_layout(Layout::top_down_justified(Align::LEFT), |ui| {
                        ui.heading(RichText::new(self.tab_labels[&self.selected_tab]).size(25.0));
                        match self.selected_tab {
                            Tab::Devices => {
                                requests.extend(self.connections_tab.ui(ui, connected_to_server));
                            }
                            Tab::Statistics => {
                                if let Some(request) = self.statistics_tab.ui(ui) {
                                    requests.push(request);
                                }
                            }
                            Tab::Settings => {
                                requests.extend(self.settings_tab.ui(ui));
                            }
                            #[cfg(not(target_arch = "wasm32"))]
                            Tab::Installation => {
                                for request in self.installation_tab.ui(ui) {
                                    match request {
                                        components::InstallationTabRequest::OpenSetupWizard => {
                                            self.setup_wizard_open = true
                                        }
                                        components::InstallationTabRequest::ServerRequest(
                                            request,
                                        ) => {
                                            requests.push(request);
                                        }
                                    }
                                }
                            }
                            Tab::Logs => self.logs_tab.ui(ui),
                            Tab::Debug => {
                                if let Some(request) = components::debug_tab_ui(ui) {
                                    requests.push(request);
                                }
                            }
                            Tab::About => components::about_tab_ui(ui),
                        }
                    })
                });
        }

        #[cfg(target_os = "linux")]
        self.update_streaming_audio();

        let shutdown_alvr = || {
            self.data_sources.request(ServerRequest::ShutdownSteamvr);

            crate::steamvr_launcher::LAUNCHER
                .lock()
                .ensure_steamvr_shutdown();
        };

        // A request the server stopped repeating is over.
        if self
            .airlink_pairing
            .as_ref()
            .is_some_and(|(_, _, at)| at.elapsed() > std::time::Duration::from_secs(8))
        {
            self.airlink_pairing = None;
        }
        if let Some((address, pairing_code, _)) = &self.airlink_pairing {
            let accept = alvr_gui_common::ModalButton::Custom("Accept".into());
            let refuse = alvr_gui_common::ModalButton::Custom("Refuse".into());
            let address = address.clone();
            let pairing_code = *pairing_code;
            // Do not let a new headset bypass the certificate comparison
            // while the exchange is still in progress or supplied no cert.
            let buttons = if pairing_code.is_some() {
                vec![accept.clone(), refuse]
            } else {
                vec![refuse]
            };
            let answer = alvr_gui_common::modal(
                ui,
                "Meta Air Link: new headset",
                Some(move |ui: &mut eframe::egui::Ui| {
                    ui.label(format!(
                        "{address} asks to connect to this computer through Air Link."
                    ));
                    if let Some(code) = pairing_code {
                        ui.heading(format!("{code:06}"));
                        ui.label("Check that the headset shows the same code.");
                    } else {
                        ui.label("Waiting for the headset's pairing code.");
                    }
                    ui.label(
                        "Accepted, it stays in the list of trusted devices and connects \
                         without asking until you remove it there.",
                    );
                }),
                &buttons,
                Some(420.0),
            );
            if let Some(answer) = answer {
                requests.push(ServerRequest::AnswerAirLinkPairing(answer == accept));
                self.airlink_pairing = None;
                self.airlink_pairing_answered_at = Some(std::time::Instant::now());
            }
        }

        if let Some(popup) = &self.new_version_popup
            && let Some(action) = popup.ui(ui, shutdown_alvr)
        {
            if let CloseAction::CloseWithRequest(request) = action {
                requests.push(request);
            }

            self.new_version_popup = None;
        }

        for request in requests {
            self.data_sources.request(request);
        }

        if ui.input(|state| state.viewport().close_requested())
            && self.session.as_ref().is_some_and(|s| {
                s.to_settings()
                    .extra
                    .steamvr_launcher
                    .open_close_steamvr_with_dashboard
            })
        {
            shutdown_alvr();
        }
    }
}
