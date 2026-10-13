//! Drop-down for the device a Meta Air Link stream uses: the headset, the
//! system default or one of the system's outputs or inputs, listed by their
//! description. Stands in for the generic control of `AirLinkAudioDevice`.

use super::{NestingInfo, reset};
use alvr_packets::{PathSegment, PathValuePair};
use eframe::{
    egui::{ComboBox, Layout, Ui},
    emath::Align,
};
use serde_json as json;
use std::time::{Duration, Instant};

const HEADSET: &str = "Headset";
const SYSTEM_DEFAULT: &str = "SystemDefault";
const CUSTOM: &str = "Custom";

pub struct Control {
    nesting_info: NestingInfo,
    is_output: bool,
    // (name, description)
    devices: Vec<(String, String)>,
    listed_at: Option<Instant>,
    combobox_id: usize,
}

impl Control {
    /// The control for this setting, if it is one of the Air Link devices.
    pub fn for_setting(nesting_info: &NestingInfo) -> Option<Self> {
        let is_output = match nesting_info.path.last()? {
            PathSegment::Name(name) if name == "airlink_speakers" => true,
            PathSegment::Name(name) if name == "airlink_microphone" => false,
            _ => return None,
        };

        Some(Self {
            nesting_info: nesting_info.clone(),
            is_output,
            devices: vec![],
            listed_at: None,
            combobox_id: alvr_gui_common::get_id(),
        })
    }

    // Devices come and go (a speaker box is switched on): list them again
    // every few seconds while the setting is shown.
    fn refresh_devices(&mut self) {
        if self
            .listed_at
            .is_some_and(|at| at.elapsed() < Duration::from_secs(3))
        {
            return;
        }
        self.listed_at = Some(Instant::now());
        #[cfg(target_os = "linux")]
        {
            let kind = if self.is_output { "sinks" } else { "sources" };
            self.devices = alvr_audio::linux::list_audio_devices(kind).unwrap_or_default();
        }
    }

    pub fn ui(
        &mut self,
        ui: &mut Ui,
        session_fragment: &mut json::Value,
        allow_inline: bool,
    ) -> Option<PathValuePair> {
        super::grid_flow_inline(ui, allow_inline);
        self.refresh_devices();

        let variant = session_fragment["variant"].as_str().unwrap_or(HEADSET).to_owned();
        let custom = session_fragment[CUSTOM].as_str().unwrap_or_default().to_owned();

        let headset_label = if self.is_output {
            "Headset speakers"
        } else {
            "Headset microphone"
        };
        let system_label = "System default (not changed)";
        let selected_text = match variant.as_str() {
            HEADSET => headset_label.to_owned(),
            SYSTEM_DEFAULT => system_label.to_owned(),
            _ => self
                .devices
                .iter()
                .find(|(name, _)| *name == custom)
                .map_or_else(
                    || format!("{custom} (not present)"),
                    |(_, description)| description.clone(),
                ),
        };

        // (variant, device name)
        let mut chosen: Option<(&str, String)> = None;
        ui.with_layout(Layout::left_to_right(Align::Center), |ui| {
            ComboBox::from_id_salt(self.combobox_id)
                .selected_text(selected_text)
                .width(320.0)
                .show_ui(ui, |ui| {
                    if ui.selectable_label(variant == HEADSET, headset_label).clicked() {
                        chosen = Some((HEADSET, custom.clone()));
                    }
                    if ui
                        .selectable_label(variant == SYSTEM_DEFAULT, system_label)
                        .clicked()
                    {
                        chosen = Some((SYSTEM_DEFAULT, custom.clone()));
                    }
                    for (name, description) in &self.devices {
                        if ui
                            .selectable_label(variant == CUSTOM && *name == custom, description)
                            .clicked()
                        {
                            chosen = Some((CUSTOM, name.clone()));
                        }
                    }
                });

            if reset::reset_button(ui, variant != HEADSET, &format!("\"{headset_label}\""))
                .clicked()
            {
                chosen = Some((HEADSET, custom.clone()));
            }
        });

        let (new_variant, new_custom) = chosen?;
        if new_variant == variant && new_custom == custom {
            return None;
        }
        // Variant and device name change together: the whole value is sent.
        *session_fragment = json::json!({ "variant": new_variant, CUSTOM: new_custom });
        Some(PathValuePair {
            path: self.nesting_info.path.clone(),
            value: session_fragment.clone(),
        })
    }
}
