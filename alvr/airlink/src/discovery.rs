//! Meta Air Link mDNS advertisement.
//!
//! This advertises the runtime DISCO listener. It is intentionally separate
//! from ALVR's `_alvr._tcp` client discovery service.

use mdns_sd::{ServiceDaemon, ServiceInfo};
use std::net::IpAddr;

pub const DISCO_SERVICE_TYPE: &str = "_metaal_sp_v1._tcp.local.";
pub const ENDPOINT_TYPE: &str = "HighwindService";
pub const ENDPOINT_VERSION: &str = "1.0.0";
pub const DEFAULT_HOST_APP_VERSION: &str = "208.0.0.74.535";

pub struct AirLinkAdvertiser {
    daemon: ServiceDaemon,
    full_name: String,
}

impl AirLinkAdvertiser {
    /// Register a Meta DISCO service until this advertiser is dropped.
    pub fn register(
        hostname: &str,
        endpoint_name: &str,
        endpoint_uuid: &str,
        address: IpAddr,
        port: u16,
    ) -> Result<Self, Box<dyn std::error::Error + Send + Sync>> {
        Self::register_with_host_version(
            hostname,
            endpoint_name,
            endpoint_uuid,
            address,
            port,
            DEFAULT_HOST_APP_VERSION,
        )
    }

    pub fn register_with_host_version(
        hostname: &str,
        endpoint_name: &str,
        endpoint_uuid: &str,
        address: IpAddr,
        port: u16,
        host_app_version: &str,
    ) -> Result<Self, Box<dyn std::error::Error + Send + Sync>> {
        let daemon = ServiceDaemon::new()?;
        let properties = [
            ("tt", "Tcp"),
            ("etn", ENDPOINT_TYPE),
            ("etv", ENDPOINT_VERSION),
            ("ein", endpoint_name),
            ("eiu", endpoint_uuid),
            ("capabilities", ""),
            ("ovrservice_version", host_app_version),
        ];
        let service = ServiceInfo::new(
            DISCO_SERVICE_TYPE,
            hostname,
            &format!("{hostname}.local."),
            address,
            port,
            &properties[..],
        )?;
        let full_name = service.get_fullname().to_owned();
        daemon.register(service)?;

        Ok(Self { daemon, full_name })
    }
}

impl Drop for AirLinkAdvertiser {
    fn drop(&mut self) {
        let _ = self.daemon.unregister(&self.full_name);
        let _ = self.daemon.shutdown();
    }
}
