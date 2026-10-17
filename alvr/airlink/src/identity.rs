//! Persistent TLS and endpoint identity for the Meta Air Link host.

use rcgen::generate_simple_self_signed;
use ring::digest::{SHA256, digest};
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer};
use std::{
    fs::{self, OpenOptions},
    io::{ErrorKind, Write},
    path::{Path, PathBuf},
};
use uuid::Uuid;

const CERT_FILE: &str = "host-cert.der";
const KEY_FILE: &str = "host-key.der";
const ENDPOINT_FILE: &str = "endpoint-uuid";
const TRUST_DIR: &str = "trusted-headsets";

pub struct HostIdentity {
    state_dir: PathBuf,
    certificate: Vec<u8>,
    private_key: Vec<u8>,
    endpoint_uuid: String,
}

impl HostIdentity {
    pub fn load_or_create(
        state_dir: impl AsRef<Path>,
    ) -> Result<Self, Box<dyn std::error::Error + Send + Sync>> {
        let state_dir = state_dir.as_ref().to_owned();
        fs::create_dir_all(&state_dir)?;
        set_private_dir_permissions(&state_dir)?;

        let certificate_path = state_dir.join(CERT_FILE);
        let key_path = state_dir.join(KEY_FILE);
        let (certificate, private_key) = match (
            fs::read(&certificate_path),
            fs::read(&key_path),
        ) {
            (Ok(certificate), Ok(private_key)) => (certificate, private_key),
            (Err(cert_error), Err(key_error))
                if cert_error.kind() == ErrorKind::NotFound
                    && key_error.kind() == ErrorKind::NotFound =>
            {
                let certified = generate_simple_self_signed(vec!["localhost".to_owned()])?;
                let certificate = certified.cert.der().to_vec();
                let private_key = certified.key_pair.serialize_der();
                write_private_file(&certificate_path, &certificate, 0o644)?;
                write_private_file(&key_path, &private_key, 0o600)?;
                (certificate, private_key)
            }
            (Err(error), _) | (_, Err(error)) => return Err(error.into()),
        };

        let endpoint_path = state_dir.join(ENDPOINT_FILE);
        let endpoint_uuid = match fs::read_to_string(&endpoint_path) {
            Ok(value) => Uuid::parse_str(value.trim())?.to_string(),
            Err(error) if error.kind() == ErrorKind::NotFound => {
                let value = Uuid::new_v4().to_string();
                write_private_file(&endpoint_path, format!("{value}\n").as_bytes(), 0o600)?;
                value
            }
            Err(error) => return Err(error.into()),
        };

        Ok(Self {
            state_dir,
            certificate,
            private_key,
            endpoint_uuid,
        })
    }

    pub fn state_dir(&self) -> &Path {
        &self.state_dir
    }

    pub fn endpoint_uuid(&self) -> &str {
        &self.endpoint_uuid
    }

    pub fn certificate_der(&self) -> &[u8] {
        &self.certificate
    }

    fn trust_path(&self, headset_id: [u8; 16]) -> PathBuf {
        let id: String = headset_id.iter().map(|byte| format!("{byte:02x}")).collect();
        self.state_dir.join(TRUST_DIR).join(id)
    }

    fn certificate_fingerprint(certificate: &[u8]) -> String {
        digest(&SHA256, certificate)
            .as_ref()
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect()
    }

    /// A trusted ALVR client id alone is insufficient: the TLS peer must
    /// present the same certificate that was approved for that id.
    pub fn matches_trusted_headset_certificate(
        &self,
        headset_id: [u8; 16],
        certificate: &[u8],
    ) -> bool {
        fs::read_to_string(self.trust_path(headset_id))
            .is_ok_and(|stored| stored.trim_end() == Self::certificate_fingerprint(certificate))
    }

    /// Store only the public certificate's SHA-256 fingerprint after the
    /// user accepts the pairing. A new certificate for the same endpoint id
    /// replaces the old binding only after another explicit acceptance.
    pub fn trust_headset_certificate(
        &self,
        headset_id: [u8; 16],
        certificate: &[u8],
    ) -> std::io::Result<()> {
        let directory = self.state_dir.join(TRUST_DIR);
        fs::create_dir_all(&directory)?;
        set_private_dir_permissions(&directory)?;
        let temporary = directory.join(format!(".{}", Uuid::new_v4()));
        write_private_file(
            &temporary,
            format!("{}\n", Self::certificate_fingerprint(certificate)).as_bytes(),
            0o600,
        )?;
        fs::rename(temporary, self.trust_path(headset_id))
    }

    pub fn rustls_identity(
        &self,
    ) -> (Vec<CertificateDer<'static>>, PrivateKeyDer<'static>) {
        (
            vec![CertificateDer::from(self.certificate.clone())],
            PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(self.private_key.clone())),
        )
    }
}

#[cfg(test)]
mod trust_tests {
    use super::HostIdentity;
    use uuid::Uuid;

    #[test]
    fn accepted_certificate_is_bound_to_endpoint_id() {
        let directory = std::env::temp_dir().join(format!("alvr-airlink-trust-{}", Uuid::new_v4()));
        let identity = HostIdentity::load_or_create(&directory).unwrap();
        let id = [7u8; 16];
        assert!(!identity.matches_trusted_headset_certificate(id, b"first"));
        identity.trust_headset_certificate(id, b"first").unwrap();
        assert!(identity.matches_trusted_headset_certificate(id, b"first"));
        assert!(!identity.matches_trusted_headset_certificate(id, b"other"));
        assert!(!identity.matches_trusted_headset_certificate([8u8; 16], b"first"));
        identity.trust_headset_certificate(id, b"other").unwrap();
        assert!(!identity.matches_trusted_headset_certificate(id, b"first"));
        assert!(identity.matches_trusted_headset_certificate(id, b"other"));
        std::fs::remove_dir_all(directory).unwrap();
    }
}

fn write_private_file(path: &Path, contents: &[u8], mode: u32) -> std::io::Result<()> {
    let mut options = OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(mode);
    }
    match options.open(path) {
        Ok(mut file) => file.write_all(contents),
        Err(error) if error.kind() == ErrorKind::AlreadyExists => Ok(()),
        Err(error) => Err(error),
    }
}

fn set_private_dir_permissions(path: &Path) -> std::io::Result<()> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(path, fs::Permissions::from_mode(0o700))?;
    }
    Ok(())
}
